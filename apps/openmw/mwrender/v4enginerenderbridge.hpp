#ifndef OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H
#define OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H

#include "v4renderroutestatus.hpp"

#include "../mwworld/scenerenderlifecycle.hpp"

#include <components/render/backend/vsg/vsgruntimebootstrap.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/renderer.hpp>

#include <memory>
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
    class Camera;

    struct V4MainFrameSource
    {
        double verticalFieldOfViewDegrees = 55.0;
        double nearPlane = 1.0;
        double farPlane = 8192.0;
        double simulationTime = 0.0;
        double frameDelta = 0.0;
        float lodScale = 1.0f;
        RenderCore::FrameEnvironmentState environment;
        bool invalidateHistory = false;
    };

    // Build-gated application bridge for the distinct VSG route. It creates the
    // session directly from OpenMW's winning VFS, hands the world an observer
    // before its first cell activation, and publishes main-camera frames without
    // consulting the OSG scene graph or OSG graphics-window state.
    class V4EngineRenderBridge final
    {
    public:
        [[nodiscard]] static std::unique_ptr<V4EngineRenderBridge> create(
            const VFS::Manager& vfs, RenderVsg::VsgRuntimeBootstrapOptions options = {});

        ~V4EngineRenderBridge();
        V4EngineRenderBridge(const V4EngineRenderBridge&) = delete;
        V4EngineRenderBridge& operator=(const V4EngineRenderBridge&) = delete;

        // Exactly one scene owns the lifecycle observer. The observer shares
        // session ownership so teardown remains GPU-safe even if application
        // members are later reordered; the registered VFS must still outlive it.
        [[nodiscard]] std::unique_ptr<MWWorld::SceneRenderLifecycle> takeSceneRenderLifecycle();
        [[nodiscard]] bool sceneRenderLifecycleTaken() const noexcept { return mLifecycleTaken; }

        [[nodiscard]] std::optional<RenderCore::Extent2D> outputExtent() const noexcept;
        RenderCore::RenderFrameResult renderMainFrame(const Camera& camera, const V4MainFrameSource& source);
        void waitIdle();

        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        V4EngineRenderBridge(const VFS::Manager& vfs, std::shared_ptr<RenderVsg::VsgSemanticSession> session);

        const VFS::Manager& mVfs;
        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        std::shared_ptr<V4RenderRouteStatus> mRouteStatus;
        std::string mLastDiagnostic;
        bool mLifecycleTaken = false;
    };
}

#endif
