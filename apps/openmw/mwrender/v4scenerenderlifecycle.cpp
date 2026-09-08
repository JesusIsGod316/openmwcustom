#include "v4scenerenderlifecycle.hpp"

#include "v4semanticsource.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include <components/misc/resourcehelpers.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/nif/niffile.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/vfs/manager.hpp>

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
            publishStaticObject(ptr);
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
            publishStaticObject(ptr);
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
            const RenderCore::ActiveCellPublishResult instance = mSession->cells().removeStaticInstance(*identity);
            if (instance.status != RenderCore::ActiveCellPublishStatus::Applied
                && instance.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordFailure("V4 scene lifecycle failed to retire a static object");
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

    void V4SceneRenderLifecycle::publishStaticObject(const MWWorld::Ptr& ptr)
    {
        requireHealthy();
        if (ptr.isEmpty() || !ptr.getCell() || ptr.getClass().isActor())
            return;

        const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
        if (!ptr.getRefData().isEnabled())
        {
            if (identity)
                objectRemoving(ptr);
            return;
        }

        if (isLight(ptr))
        {
            const std::optional<RenderCore::CellLightSource> light = makeV4CellLightSource(ptr);
            if (!light)
                throw std::runtime_error("V4 scene lifecycle rejected an enabled cell light");
            const RenderCore::ActiveCellPublishResult result = mSession->cells().upsertLight(*light);
            if (!accepted(result.status))
                throw publicationError("cell light publication", static_cast<unsigned int>(result.status));

            // Light models follow the animated-object path in OpenMW even when
            // their NIF is visually static. Keep their light semantics without
            // misclassifying the model as a static translation; the VSG host
            // remains fail-closed until that animated model path is available.
            return;
        }

        if (ptr.getClass().useAnim())
            return;

        const VFS::Path::Normalized modelPath = ptr.getClass().getCorrectedModel(ptr);
        if (modelPath.empty() || Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return;

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
        const std::optional<RenderCore::StaticInstanceSource> source
            = makeV4StaticInstanceSource(ptr, *model, modelRecord->bounds);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an eligible static object");

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
