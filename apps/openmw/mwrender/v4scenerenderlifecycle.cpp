#include "v4scenerenderlifecycle.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

#include "v4semanticsource.hpp"
#include "vulkanmw/staticworldsource.hpp"

#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/containerstore.hpp"
#include "../mwworld/ptr.hpp"

#include <components/esm3/loadcont.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/misc/constants.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/nifrender/enchantedglow.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/render/native/nifassetservice.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace MWRender
{
    bool V4SceneRenderLifecycle::usesLegacyTerrainPreload() const noexcept
    {
        // V4EngineRenderBridge owns the visible terrain and groundcover route.
        // Retain both the old full-frontload control and a grid-only control.
        return usesLegacyTerrainFrontload()
            || std::getenv("OPENMW_V4_LEGACY_TERRAIN_PRELOAD_CONTROL") != nullptr;
    }

    bool V4SceneRenderLifecycle::usesLegacyTerrainFrontload() const noexcept
    {
        // Independent same-executable control; never changes OpenGL behavior.
        return std::getenv("OPENMW_V4_LEGACY_TERRAIN_FRONTLOAD") != nullptr;
    }

    namespace
    {
        [[nodiscard]] bool accepted(RenderCore::ActiveCellPublishStatus status) noexcept
        {
            return status == RenderCore::ActiveCellPublishStatus::Applied
                || status == RenderCore::ActiveCellPublishStatus::AlreadyPresent;
        }

        [[nodiscard]] std::runtime_error publicationError(const char* operation, unsigned int status)
        {
            return std::runtime_error(
                std::string("V4 scene lifecycle ") + operation + " failed with status " + std::to_string(status));
        }

        [[nodiscard]] std::runtime_error staticWorldError(
            const char* operation, const RenderNative::StaticWorldMutationResult& result)
        {
            return std::runtime_error(std::string("VulkanMW static world ") + operation
                + " failed with status " + std::to_string(static_cast<unsigned int>(result.status))
                + ", cell status " + std::to_string(static_cast<unsigned int>(result.cellStatus))
                + ", population status " + std::to_string(static_cast<unsigned int>(result.populationStatus)));
        }

        [[nodiscard]] std::runtime_error enchantedGlowError(NifRender::EnchantedGlowPublishStatus status)
        {
            using Status = NifRender::EnchantedGlowPublishStatus;
            switch (status)
            {
                case Status::MissingTexture:
                    return std::runtime_error(
                        "V4 enchanted world reference is missing one or more canonical caustic texture frames");
                case Status::ExistingEnvironmentBinding:
                    return std::runtime_error(
                        "V4 enchanted world reference also owns an authored environment map; combined semantics remain fail-closed");
                case Status::UnsupportedLightingOrder:
                    return std::runtime_error(
                        "V4 enchanted glow requires the pre-light environment-map compatibility facet because Apply Lighting to Environment Maps is enabled");
                case Status::ReservationFailed:
                case Status::BatchBuildFailed:
                case Status::PublishRejected:
                case Status::InvalidSource:
                    return publicationError("enchanted world-reference variant publication",
                        static_cast<unsigned int>(status));
                case Status::Published:
                case Status::Reused:
                    break;
            }
            return std::runtime_error("V4 enchanted world-reference variant publication returned an invalid status");
        }

        [[nodiscard]] bool isLight(const MWWorld::Ptr& ptr) noexcept
        {
            return !ptr.isEmpty()
                && (ptr.getType() == ESM::Light::sRecordId || ptr.getType() == ESM4::Light::sRecordId);
        }

        [[nodiscard]] bool requiresModelPlayback(const RenderCore::ModelRecord& model) noexcept
        {
            if (!RenderCore::validModelDynamicRequirements(model.dynamicRequirements)
                || model.dynamicRequirements != 0 || !model.payload)
                return true;
            return std::any_of(model.payload->nodes.begin(), model.payload->nodes.end(),
                [](const RenderCore::ModelNodeRecord& node) { return node.controllerFlags != 0; });
        }

        void applyReferenceVisualSemantics(const MWWorld::Ptr& ptr, std::uint64_t modelCapabilities,
            RenderCore::StaticInstanceSource& source)
        {
            source.semanticFlags |= modelCapabilities;
            if (!Settings::game().mGraphicHerbalism
                || (modelCapabilities & RenderCore::HerbalismSwitchCapabilitySemanticFlag) == 0
                || ptr.getType() != ESM::Container::sRecordId || ptr.getRefData().getCustomData() == nullptr)
                return;

            const MWWorld::LiveCellRef<ESM::Container>* ref = ptr.get<ESM::Container>();
            if (!ref || !ref->mBase || !(ref->mBase->mFlags & ESM::Container::Organic))
                return;

            const MWWorld::ContainerStore& store = ptr.getClass().getContainerStore(ptr);
            if (!store.hasVisibleItems())
                source.semanticFlags |= RenderCore::HerbalismHarvestedSemanticFlag;
        }
    }

    V4SceneRenderLifecycle::V4SceneRenderLifecycle(
        std::shared_ptr<RenderVsg::VsgSemanticSession> session, const VFS::Manager& vfs,
        std::shared_ptr<V4RenderRouteStatus> routeStatus)
        : mSession(std::move(session))
        , mRouteStatus(std::move(routeStatus))
        , mVfs(vfs)
        , mTextureIdentities(vfs)
    {
        if (!mSession || !mRouteStatus)
            throw std::invalid_argument("V4 scene lifecycle requires a semantic session and route status");
        mNativeAssets = std::make_unique<RenderNative::NifAssetService>(
            mVfs, &mTextureIdentities, mSession->models());
        mNativeStaticWorld = std::make_unique<RenderNative::StaticWorldService>(
            mSession->cells(), mSession->populations());
    }

    void V4SceneRenderLifecycle::cellActivated(const MWWorld::CellStore& cell)
    {
        try
        {
            requireHealthy();
            const std::optional<RenderNative::StaticWorldCellSource> source
                = VulkanMW::makeStaticWorldCellSource(cell);
            if (!source)
                throw std::runtime_error("VulkanMW static world rejected an invalid active cell");
            const RenderNative::StaticWorldMutationResult result = mNativeStaticWorld->activateCell(*source);
            if (!result.accepted())
                throw staticWorldError("cell activation", result);
        }
        catch (const std::exception& error)
        {
            recordFailure(error.what());
            throw;
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle cell activation threw an unknown exception");
            throw;
        }
    }

    void V4SceneRenderLifecycle::cellDeactivating(const MWWorld::CellStore& cell) noexcept
    {
        try
        {
            const std::optional<std::string> identity = VulkanMW::makeCellIdentity(cell);
            if (!identity)
            {
                recordFailure("VulkanMW static world could not identify a deactivating cell");
                return;
            }
            const RenderNative::StaticWorldMutationResult result = mNativeStaticWorld->deactivateCell(*identity);
            if (!result.accepted())
                recordFailure(staticWorldError("cell retirement", result).what());
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle cell retirement threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::objectAdded(const MWWorld::Ptr& ptr)
    {
        try
        {
            publishObject(ptr);
        }
        catch (const std::exception& error)
        {
            recordFailure(error.what());
            throw;
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle object publication threw an unknown exception");
            throw;
        }
    }

    void V4SceneRenderLifecycle::objectChanged(const MWWorld::Ptr& ptr)
    {
        try
        {
            publishObject(ptr);
        }
        catch (const std::exception& error)
        {
            recordFailure(error.what());
            throw;
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle object mutation threw an unknown exception");
            throw;
        }
    }

    void V4SceneRenderLifecycle::objectRemoving(const MWWorld::Ptr& ptr) noexcept
    {
        try
        {
            const std::optional<std::string> identity = VulkanMW::makeReferenceIdentity(ptr);
            if (!identity)
                return;
            if (ptr.getClass().isActor())
            {
                const RenderCore::ActiveCellPublishResult instance = mSession->cells().removeInstance(*identity);
                if (instance.status != RenderCore::ActiveCellPublishStatus::Applied
                    && instance.status != RenderCore::ActiveCellPublishStatus::NotFound)
                    recordFailure("V4 scene lifecycle failed to retire an actor instance");
            }
            else
            {
                const RenderNative::StaticWorldMutationResult staticResult = mNativeStaticWorld->removeStatic(*identity);
                if (!staticResult.accepted())
                    recordFailure(staticWorldError("object retirement", staticResult).what());
            }
            const RenderCore::ActiveCellPublishResult light = mSession->cells().removeLight(*identity);
            if (light.status != RenderCore::ActiveCellPublishStatus::Applied
                && light.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordFailure("V4 scene lifecycle failed to retire a cell light");
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle object retirement threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::worldResetting() noexcept
    {
        try
        {
            mNativeAssets->clearSourceMetadata();
            mTextureIdentities.clear();
            if (!mSession->resetWorld())
                recordFailure("V4 scene lifecycle failed to reset the semantic world");
        }
        catch (...)
        {
            recordFailure("V4 scene lifecycle world reset threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::publishObject(const MWWorld::Ptr& ptr)
    {
        requireHealthy();
        if (ptr.isEmpty() || !ptr.getCell())
            return;

        const std::optional<std::string> identity = VulkanMW::makeReferenceIdentity(ptr);
        if (!ptr.getRefData().isEnabled())
        {
            if (identity)
                objectRemoving(ptr);
            return;
        }

        // Actor publication is captured after OpenMW has assembled/evaluated
        // its Animation. This lifecycle notification establishes cell order,
        // while V4EngineRenderBridge publishes the exact selected actor parts
        // and the matching first pose together at the frame boundary.
        const bool actor = ptr.getClass().isActor();
        if (actor)
            return;

        const bool lightObject = isLight(ptr);
        if (lightObject)
        {
            const std::optional<RenderCore::CellLightSource> light = makeV4CellLightSource(ptr);
            if (!light)
                throw std::runtime_error("V4 scene lifecycle rejected an enabled cell light");
            const RenderCore::ActiveCellPublishResult result = mSession->cells().upsertLight(*light);
            if (!accepted(result.status))
                throw publicationError("cell light publication", static_cast<unsigned int>(result.status));
            // Do not return here when a visible model exists. Light emission and
            // visible fixture geometry are independent semantic resources.
        }

        const VFS::Path::Normalized modelPath = ptr.getClass().getCorrectedModel(ptr);
        if (modelPath.empty() || Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return;

        const auto retirePersistentObject = [&]() {
            if (!identity)
                throw std::runtime_error("V4 evaluated non-actor object has no stable content identity");
            const RenderNative::StaticWorldMutationResult retired = mNativeStaticWorld->removeStatic(*identity);
            if (!retired.accepted())
                throw staticWorldError("evaluated object retirement", retired);
        };

        // OpenMW's existing Animation/ObjectAnimation graph remains the
        // authoritative evaluator for useAnim() objects, including external KF
        // sources supplied by mods. Do not publish a second frozen V4 instance;
        // V4EngineRenderBridge snapshots the evaluated source graph into neutral
        // frame draws after the update traversal instead.
        const bool animatedClass = ptr.getClass().useAnim();
        if (animatedClass)
        {
            retirePersistentObject();
            return;
        }

        const std::string modelIdentity = modelPath.value();
        const RenderNative::NifAssetResolveResult resolved = mNativeAssets->resolve(modelPath);
        if (!resolved.available())
            throw std::runtime_error("VulkanMW native NIF asset resolution failed for " + modelIdentity + ": "
                + resolved.diagnostic);
        std::uint64_t visualCapabilities = resolved.namedVisualCapabilities;
        std::optional<RenderCore::ModelHandle> model = resolved.model;

        const RenderCore::ModelRecord* modelRecord = mSession->world().get(*model);
        if (!modelRecord)
            throw std::runtime_error("V4 static model cache returned a stale model handle");
        if (requiresModelPlayback(*modelRecord))
        {
            // Some non-useAnim classes can still receive authored embedded
            // controller/effect/deformation state. Route those through the same
            // evaluated compatibility seam instead of silently freezing them.
            retirePersistentObject();
            return;
        }

        // SceneUtil::addEnchantedGlow is reference state, not immutable model
        // state. Keep the shared translated source model untouched and bind this
        // one reference to a cached material/model variant derived from gameplay
        // enchantment data. Exterior populations naturally split by variant
        // model handle, preserving instancing for references with matching RGB.
        if (!ptr.getClass().getEnchantment(ptr).empty())
        {
            const osg::Vec4f sourceColor = ptr.getClass().getEnchantmentColor(ptr);
            const RenderCore::Color color{ sourceColor.r(), sourceColor.g(), sourceColor.b(), sourceColor.a() };
            const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(
                mSession->world(), mSession->publisher(), mVfs, *model, color,
                Settings::shaders().mApplyLightingToEnvironmentMaps);
            if (!glow.available())
                throw enchantedGlowError(glow.status);
            model = glow.model;
            modelRecord = mSession->world().get(*model);
            if (!modelRecord)
                throw std::runtime_error("V4 enchanted world-reference variant returned a stale model handle");
        }

        std::optional<RenderCore::StaticInstanceSource> source
            = VulkanMW::makeStaticInstanceSource(ptr, *model, modelRecord->bounds);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an eligible static object");
        applyReferenceVisualSemantics(ptr, visualCapabilities, *source);

        // Dense immutable exterior statics use data-oriented population chunks;
        // interiors stay individually addressable. The native service owns the
        // transition between those representations so the app adapter does not
        // duplicate RenderWorld mutation policy.
        const RenderNative::StaticWorldMutationResult staticResult
            = mNativeStaticWorld->upsertStatic(*source, ptr.getCell()->getCell()->isExterior());
        if (!staticResult.accepted())
            throw staticWorldError("static object publication", staticResult);
    }

    void V4SceneRenderLifecycle::requireHealthy() const
    {
        if (mRouteStatus->healthy())
            return;
        if (!mRouteStatus->firstDiagnostic().empty())
            throw std::runtime_error(mRouteStatus->firstDiagnostic());
        throw std::runtime_error("V4 scene publication route is unhealthy");
    }

    void V4SceneRenderLifecycle::recordFailure(std::string_view message) noexcept
    {
        mRouteStatus->fail(message);
    }
}
