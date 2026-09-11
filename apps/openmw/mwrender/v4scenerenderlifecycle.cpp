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
#include <components/nif/niffile.hpp>
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

        [[nodiscard]] bool isLight(const MWWorld::Ptr& ptr) noexcept
        {
            return !ptr.isEmpty()
                && (ptr.getType() == ESM::Light::sRecordId || ptr.getType() == ESM4::Light::sRecordId);
        }

        // useAnim() is a class capability, not proof that this particular
        // reference actually has a playable model animation. Objects::insertModel
        // only attaches an external animation source when x<model>.kf exists.
        // Keep that exact distinction at the semantic boundary: controller-free
        // instances may use the immutable model plus live reference transform,
        // while an authored external animation remains fail-closed.
        [[nodiscard]] bool hasExternalAnimationSource(
            VFS::Path::NormalizedView modelPath, const VFS::Manager& vfs)
        {
            if (modelPath.empty())
                return false;
            const VFS::Path::Normalized corrected = Misc::ResourceHelpers::correctActorModelPath(modelPath, &vfs);
            return corrected.view() != modelPath.value();
        }

        [[nodiscard]] bool requiresModelPlayback(const RenderCore::ModelRecord& model) noexcept
        {
            if (!RenderCore::validModelDynamicRequirements(model.dynamicRequirements)
                || model.dynamicRequirements != 0 || !model.payload)
                return true;
            return std::any_of(model.payload->nodes.begin(), model.payload->nodes.end(),
                [](const RenderCore::ModelNodeRecord& node) { return node.controllerFlags != 0; });
        }

        [[nodiscard]] bool hasNamedNode(const RenderCore::ModelRecord& model, std::string_view name) noexcept
        {
            return model.payload
                && std::any_of(model.payload->nodes.begin(), model.payload->nodes.end(),
                    [&](const RenderCore::ModelNodeRecord& node) { return node.name == name; });
        }

        // makeV4StaticInstanceSource intentionally rejects every useAnim() class.
        // This narrowly-scoped companion is called only after publishObject has
        // proved that the winning model has neither an external animation source
        // nor any neutral controller/effect/deformation playback requirement.
        // Reference translation, rotation and scale remain live and objectChanged()
        // republishes them.
        [[nodiscard]] std::optional<RenderCore::StaticInstanceSource> makeControllerFreeAnimatedInstanceSource(
            const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds)
        {
            if (ptr.isEmpty() || !ptr.getCell() || !model.valid() || !ptr.getRefData().isEnabled()
                || ptr.getClass().isActor() || !ptr.getClass().useAnim())
                return std::nullopt;
            const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
            const std::optional<RenderCore::ActiveCellSource> cell = makeV4ActiveCellSource(*ptr.getCell());
            if (!identity || !cell)
                return std::nullopt;

            const ESM::Position& position = ptr.getRefData().getPosition();
            const osg::Quat rotation = Misc::Convert::makeOsgQuat(position);
            const float scale = ptr.getCellRef().getScale();
            RenderCore::StaticInstanceSource result;
            result.identity = *identity;
            result.cellIdentity = cell->identity;
            result.model = model;
            result.transform.translation = { position.pos[0], position.pos[1], position.pos[2] };
            result.transform.rotation = { static_cast<float>(rotation.w()), static_cast<float>(rotation.x()),
                static_cast<float>(rotation.y()), static_cast<float>(rotation.z()) };
            result.transform.scale = { scale, scale, scale };
            result.localBounds = localBounds;
            return result;
        }

        void applyReferenceVisualSemantics(const MWWorld::Ptr& ptr, const RenderCore::ModelRecord& model,
            RenderCore::StaticInstanceSource& source)
        {
            if (!Settings::game().mGraphicHerbalism || ptr.getType() != ESM::Container::sRecordId
                || ptr.getRefData().getCustomData() == nullptr || !hasNamedNode(model, Constants::HerbalismLabel))
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

        const bool animatedClass = ptr.getClass().useAnim();
        if (animatedClass && hasExternalAnimationSource(modelPath, mVfs))
        {
            throw std::runtime_error(
                "V4 scene lifecycle encountered an object with an external animation source before model-animation compatibility is available: "
                + modelPath.value());
        }

        std::optional<RenderCore::ModelHandle> model = mSession->models().find(modelPath.value());
        if (!model)
        {
            if (!mVfs.exists(modelPath))
                throw std::runtime_error("V4 static model is missing from the winning VFS: " + modelPath.value());

            Nif::NIFFile nifFile(modelPath);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(mVfs.get(modelPath));
            const NifRender::TranslationBundle bundle
                = NifRender::translateStaticNif(Nif::FileView(nifFile), mVfs);
            const NifRender::StaticModelCacheResult published = mSession->models().publish(bundle);
            if (!published.available())
                throw publicationError("static model publication", static_cast<unsigned int>(published.status));
            model = published.model;
        }

        const RenderCore::ModelRecord* modelRecord = mSession->world().get(*model);
        if (!modelRecord)
            throw std::runtime_error("V4 static model cache returned a stale model handle");
        if (requiresModelPlayback(*modelRecord))
        {
            throw std::runtime_error(
                "V4 scene lifecycle encountered a non-actor model with controller/effect/deformation playback requirements before model-animation compatibility is available: "
                + modelPath.value());
        }

        std::optional<RenderCore::StaticInstanceSource> source = animatedClass
            ? makeControllerFreeAnimatedInstanceSource(ptr, *model, modelRecord->bounds)
            : makeV4StaticInstanceSource(ptr, *model, modelRecord->bounds);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an eligible static/reference-animated object");
        applyReferenceVisualSemantics(ptr, *modelRecord, *source);

        // Dense immutable exterior statics keep the data-oriented population
        // path. Interactive/useAnim references stay individually addressable so
        // live door transforms and per-reference switch state cannot be collapsed
        // into one model-global population realization.
        if (ptr.getCell()->getCell()->isExterior() && !animatedClass)
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
