#include "v4scenerenderlifecycle.hpp"

#include "v4semanticsource.hpp"

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
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nifrender/enchantedglow.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace MWRender
{
    namespace
    {
        [[nodiscard]] bool accepted(RenderCore::ActiveCellPublishStatus status) noexcept
        {
            return status == RenderCore::ActiveCellPublishStatus::Applied
                || status == RenderCore::ActiveCellPublishStatus::AlreadyPresent;
        }

        [[nodiscard]] bool accepted(RenderCore::StaticPopulationPublishStatus status) noexcept
        {
            return status == RenderCore::StaticPopulationPublishStatus::Applied
                || status == RenderCore::StaticPopulationPublishStatus::AlreadyPresent;
        }

        [[nodiscard]] std::runtime_error publicationError(const char* operation, unsigned int status)
        {
            return std::runtime_error(
                std::string("V4 scene lifecycle ") + operation + " failed with status " + std::to_string(status));
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

        // SceneUtil::hasUserDescription() performs exact description equality.
        // Recover the same capability information from the winning source NIF
        // before OSG exists, then publish only neutral semantic bits downstream.
        [[nodiscard]] std::uint64_t inspectNamedVisualCapabilities(Nif::FileView file) noexcept
        {
            std::uint64_t result = 0;
            for (std::size_t rootIndex = 0; rootIndex < file.numRoots(); ++rootIndex)
            {
                const Nif::Record* record = file.getRoot(rootIndex);
                const auto* root = dynamic_cast<const Nif::NiAVObject*>(record);
                if (!root)
                    continue;
                for (const Nif::ExtraPtr& extra : root->getExtraList())
                {
                    if (extra.empty() || extra->mRecordType != Nif::RC_NiStringExtraData)
                        continue;
                    const auto* value = static_cast<const Nif::NiStringExtraData*>(extra.getPtr());
                    if (value->mData == Constants::NightDayLabel)
                        result |= RenderCore::NightDaySwitchCapabilitySemanticFlag;
                    else if (value->mData == Constants::HerbalismLabel)
                        result |= RenderCore::HerbalismSwitchCapabilitySemanticFlag;
                }
            }
            return result;
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
    {
        if (!mSession || !mRouteStatus)
            throw std::invalid_argument("V4 scene lifecycle requires a semantic session and route status");
    }

    void V4SceneRenderLifecycle::cellActivated(const MWWorld::CellStore& cell)
    {
        try
        {
            requireHealthy();
            const std::optional<RenderCore::ActiveCellSource> source = makeV4ActiveCellSource(cell);
            if (!source)
                throw std::runtime_error("V4 scene lifecycle rejected an invalid active cell");
            const RenderCore::ActiveCellPublishResult result = mSession->cells().addCell(*source);
            if (!accepted(result.status))
                throw publicationError("cell activation", static_cast<unsigned int>(result.status));
            if (cell.getCell()->isExterior())
            {
                RenderCore::StaticPopulationCellSource population;
                population.identity = source->identity;
                population.worldspaceIdentity = source->worldspaceIdentity;
                population.gridX = cell.getCell()->getGridX();
                population.gridY = cell.getCell()->getGridY();
                const RenderCore::StaticPopulationPublishStatus populationResult
                    = mSession->populations().addCell(std::move(population));
                if (!accepted(populationResult))
                    throw publicationError("exterior population activation", static_cast<unsigned int>(populationResult));
            }
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
            const std::optional<std::string> identity = makeV4CellIdentity(cell);
            if (!identity)
            {
                recordFailure("V4 scene lifecycle could not identify a deactivating cell");
                return;
            }
            const RenderCore::ActiveCellPublishResult result = mSession->cells().removeCell(*identity);
            if (result.status != RenderCore::ActiveCellPublishStatus::Applied
                && result.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordFailure("V4 scene lifecycle failed to retire a cell");
            const RenderCore::StaticPopulationPublishStatus population = mSession->populations().removeCell(*identity);
            if (population != RenderCore::StaticPopulationPublishStatus::Applied
                && population != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
                recordFailure("V4 scene lifecycle failed to retire an exterior population");
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
            const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
            if (!identity)
                return;
            const RenderCore::ActiveCellPublishResult instance = mSession->cells().removeInstance(*identity);
            if (instance.status != RenderCore::ActiveCellPublishStatus::Applied
                && instance.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordFailure("V4 scene lifecycle failed to retire an object instance");
            const RenderCore::StaticPopulationPublishStatus population = mSession->populations().remove(*identity);
            if (population != RenderCore::StaticPopulationPublishStatus::Applied
                && population != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
                recordFailure("V4 scene lifecycle failed to retire an exterior population placement");
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
            mModelVisualCapabilities.clear();
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

        const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
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
            const RenderCore::ActiveCellPublishResult instance = mSession->cells().removeInstance(*identity);
            if (instance.status != RenderCore::ActiveCellPublishStatus::Applied
                && instance.status != RenderCore::ActiveCellPublishStatus::NotFound)
                throw publicationError("evaluated object static retirement", static_cast<unsigned int>(instance.status));
            const RenderCore::StaticPopulationPublishStatus population = mSession->populations().remove(*identity);
            if (!accepted(population))
                throw publicationError(
                    "evaluated object population retirement", static_cast<unsigned int>(population));
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
        std::uint64_t visualCapabilities = 0;
        auto capability = mModelVisualCapabilities.find(modelIdentity);
        std::optional<RenderCore::ModelHandle> model = mSession->models().find(modelIdentity);
        if (!model)
        {
            if (!mVfs.exists(modelPath))
                throw std::runtime_error("V4 static model is missing from the winning VFS: " + modelIdentity);

            Nif::NIFFile nifFile(modelPath);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(mVfs.get(modelPath));
            const Nif::FileView file(nifFile);
            visualCapabilities = inspectNamedVisualCapabilities(file);
            mModelVisualCapabilities.insert_or_assign(modelIdentity, visualCapabilities);
            const NifRender::TranslationBundle bundle = NifRender::translateStaticNif(file, mVfs);
            const NifRender::StaticModelCacheResult published = mSession->models().publish(bundle);
            if (!published.available())
                throw publicationError("static model publication", static_cast<unsigned int>(published.status));
            model = published.model;
        }
        else if (capability != mModelVisualCapabilities.end())
            visualCapabilities = capability->second;
        else
        {
            // Another source path (notably actor model composition) can populate
            // the shared model cache before this lifecycle sees a world object.
            // Recover the root descriptions once, then retain them for live
            // objectChanged updates such as door rotation and harvesting.
            if (!mVfs.exists(modelPath))
                throw std::runtime_error("V4 cached static model is missing from the winning VFS: " + modelIdentity);
            Nif::NIFFile nifFile(modelPath);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(mVfs.get(modelPath));
            visualCapabilities = inspectNamedVisualCapabilities(Nif::FileView(nifFile));
            mModelVisualCapabilities.emplace(modelIdentity, visualCapabilities);
        }

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
            = makeV4StaticInstanceSource(ptr, *model, modelRecord->bounds);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an eligible static object");
        applyReferenceVisualSemantics(ptr, visualCapabilities, *source);

        // Dense immutable exterior statics keep the data-oriented population
        // path. Evaluated animated references never reach this branch: they stay
        // individually authoritative on the OpenMW animation side and are copied
        // into neutral frame state by V4EngineRenderBridge.
        if (ptr.getCell()->getCell()->isExterior())
        {
            const RenderCore::ActiveCellPublishResult removed = mSession->cells().removeInstance(source->identity);
            if (removed.status != RenderCore::ActiveCellPublishStatus::Applied
                && removed.status != RenderCore::ActiveCellPublishStatus::NotFound)
                throw publicationError("interior static retirement", static_cast<unsigned int>(removed.status));
            RenderCore::StaticPopulationInstanceSource population;
            population.identity = source->identity;
            population.cellIdentity = source->cellIdentity;
            population.model = source->model;
            population.transform = source->transform;
            population.localBounds = source->localBounds;
            population.lod = source->lod;
            population.semanticFlags = source->semanticFlags;
            population.lightingEnabled = source->lightingEnabled;
            const RenderCore::StaticPopulationPublishStatus result
                = mSession->populations().upsert(std::move(population));
            if (!accepted(result))
                throw publicationError("exterior static population", static_cast<unsigned int>(result));
            return;
        }

        const RenderCore::StaticPopulationPublishStatus removed = mSession->populations().remove(source->identity);
        if (!accepted(removed))
            throw publicationError("exterior static retirement", static_cast<unsigned int>(removed));
        const RenderCore::ActiveCellPublishResult result = mSession->cells().upsertStaticInstance(*source);
        if (!accepted(result.status))
            throw publicationError("static object publication", static_cast<unsigned int>(result.status));
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
