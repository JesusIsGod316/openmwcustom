#ifndef OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H
#define OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H

#include "v4engineframesource.hpp"
#include "v4renderroutestatus.hpp"

#include "../mwworld/scenerenderlifecycle.hpp"

#include <components/render/backend/vsg/vsgruntimebootstrap.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/renderer.hpp>

#include <memory>
#include <map>
#include <optional>
#include <string>

namespace VFS
{
    class Manager;
}

namespace RenderVsg
{
    class VsgSemanticSession;
}

namespace MWRender
{
    class RenderingManager;
    // Build-gated application bridge for the distinct VSG route. It creates the
    // session directly from OpenMW's winning VFS, hands the world an observer
    // before its first cell activation, and publishes main-camera frames without
    // consulting the OSG scene graph or OSG graphics-window state.
    class V4EngineRenderBridge final
    {
    public:
        // Out-of-line probe used by the guarded production executable target.
        // Referencing it forces the bridge/session/backend archive chain through
        // the final link even while renderer selection remains unadvertised.
        [[nodiscard]] static bool linkedRuntimeAvailable() noexcept;

        [[nodiscard]] static std::unique_ptr<V4EngineRenderBridge> create(
            const VFS::Manager& vfs, RenderVsg::VsgRuntimeBootstrapOptions options = {});
        [[nodiscard]] static std::unique_ptr<V4EngineRenderBridge> createConfigured(const VFS::Manager& vfs);

        ~V4EngineRenderBridge();
        V4EngineRenderBridge(const V4EngineRenderBridge&) = delete;
        V4EngineRenderBridge& operator=(const V4EngineRenderBridge&) = delete;

        // Exactly one scene owns the lifecycle observer. The observer shares
        // session ownership so teardown remains GPU-safe even if application
        // members are later reordered; the registered VFS must still outlive it.
        [[nodiscard]] std::unique_ptr<MWWorld::SceneRenderLifecycle> takeSceneRenderLifecycle();
        [[nodiscard]] bool sceneRenderLifecycleTaken() const noexcept { return mLifecycleTaken; }

        [[nodiscard]] std::optional<RenderCore::Extent2D> outputExtent() const noexcept;
        [[nodiscard]] bool captureDynamicFrameState(
            const RenderingManager& rendering, V4MainFrameSource& source);
        RenderCore::RenderFrameResult renderMainFrame(const V4MainFrameSource& source);
        void waitIdle();

        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        V4EngineRenderBridge(const VFS::Manager& vfs, std::shared_ptr<RenderVsg::VsgSemanticSession> session);

        const VFS::Manager& mVfs;
        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        std::shared_ptr<V4RenderRouteStatus> mRouteStatus;
        std::string mLastDiagnostic;
        bool mLifecycleTaken = false;
        unsigned int mPoseTraversal = 0;
        struct ComposedActorEntry
        {
            RenderCore::ModelHandle model;
            std::string signature;
        };
        std::map<std::string, ComposedActorEntry, std::less<>> mComposedActors;
        RenderCore::WorldEpoch mComposedActorEpoch;
    };
}

#endif
