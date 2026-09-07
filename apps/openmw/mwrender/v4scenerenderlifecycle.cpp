#include "v4scenerenderlifecycle.hpp"

#include "v4semanticsource.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include <components/misc/resourcehelpers.hpp>
#include <components/nif/niffile.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/vfs/manager.hpp>

#include <stdexcept>

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
    }

    V4SceneRenderLifecycle::V4SceneRenderLifecycle(
        RenderVsg::VsgSemanticSession& session, const VFS::Manager& vfs)
        : mSession(session)
        , mVfs(vfs)
    {
    }

    void V4SceneRenderLifecycle::cellActivated(const MWWorld::CellStore& cell)
    {
        const std::optional<RenderCore::ActiveCellSource> source = makeV4ActiveCellSource(cell);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an invalid active cell");
        const RenderCore::ActiveCellPublishResult result = mSession.cells().addCell(*source);
        if (!accepted(result.status))
            throw publicationError("cell activation", static_cast<unsigned int>(result.status));
    }

    void V4SceneRenderLifecycle::cellDeactivating(const MWWorld::CellStore& cell) noexcept
    {
        try
        {
            const std::optional<std::string> identity = makeV4CellIdentity(cell);
            if (!identity)
            {
                recordRetirementFailure("V4 scene lifecycle could not identify a deactivating cell");
                return;
            }
            const RenderCore::ActiveCellPublishResult result = mSession.cells().removeCell(*identity);
            if (result.status != RenderCore::ActiveCellPublishStatus::Applied
                && result.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordRetirementFailure("V4 scene lifecycle failed to retire a cell");
        }
        catch (...)
        {
            recordRetirementFailure("V4 scene lifecycle cell retirement threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::objectAdded(const MWWorld::Ptr& ptr)
    {
        publishStaticObject(ptr);
    }

    void V4SceneRenderLifecycle::objectChanged(const MWWorld::Ptr& ptr)
    {
        publishStaticObject(ptr);
    }

    void V4SceneRenderLifecycle::objectRemoving(const MWWorld::Ptr& ptr) noexcept
    {
        try
        {
            const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
            if (!identity)
                return;
            const RenderCore::ActiveCellPublishResult result = mSession.cells().removeStaticInstance(*identity);
            if (result.status != RenderCore::ActiveCellPublishStatus::Applied
                && result.status != RenderCore::ActiveCellPublishStatus::NotFound)
                recordRetirementFailure("V4 scene lifecycle failed to retire a static object");
        }
        catch (...)
        {
            recordRetirementFailure("V4 scene lifecycle object retirement threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::worldResetting() noexcept
    {
        try
        {
            if (!mSession.resetWorld())
                recordRetirementFailure("V4 scene lifecycle failed to reset the semantic world");
        }
        catch (...)
        {
            recordRetirementFailure("V4 scene lifecycle world reset threw unexpectedly");
        }
    }

    void V4SceneRenderLifecycle::publishStaticObject(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || !ptr.getCell() || ptr.getClass().isActor() || ptr.getClass().useAnim())
            return;

        const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
        if (!ptr.getRefData().isEnabled())
        {
            if (identity)
                objectRemoving(ptr);
            return;
        }

        const VFS::Path::Normalized modelPath = ptr.getClass().getCorrectedModel(ptr);
        if (modelPath.empty() || Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
            return;

        std::optional<RenderCore::ModelHandle> model = mSession.models().find(modelPath.value());
        if (!model)
        {
            if (!mVfs.exists(modelPath))
                throw std::runtime_error("V4 static model is missing from the winning VFS: " + modelPath.value());

            Nif::NIFFile nifFile(modelPath);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(mVfs.get(modelPath));
            const NifRender::TranslationBundle bundle
                = NifRender::translateStaticNif(Nif::FileView(nifFile), mVfs);
            const NifRender::StaticModelCacheResult published = mSession.models().publish(bundle);
            if (!published.available())
                throw publicationError("static model publication", static_cast<unsigned int>(published.status));
            model = published.model;
        }

        const RenderCore::ModelRecord* modelRecord = mSession.world().get(*model);
        if (!modelRecord)
            throw std::runtime_error("V4 static model cache returned a stale model handle");
        const std::optional<RenderCore::StaticInstanceSource> source
            = makeV4StaticInstanceSource(ptr, *model, modelRecord->bounds);
        if (!source)
            throw std::runtime_error("V4 scene lifecycle rejected an eligible static object");

        const RenderCore::ActiveCellPublishResult result = mSession.cells().upsertStaticInstance(*source);
        if (!accepted(result.status))
            throw publicationError("static object publication", static_cast<unsigned int>(result.status));
    }

    void V4SceneRenderLifecycle::recordRetirementFailure(std::string_view message) noexcept
    {
        mHealthy = false;
        try
        {
            mLastDiagnostic = message;
        }
        catch (...)
        {
        }
    }
}
