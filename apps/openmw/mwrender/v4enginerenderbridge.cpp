#include "v4enginerenderbridge.hpp"

#include "v4runtimeoptions.hpp"
#include "v4scenerenderlifecycle.hpp"

#include <components/render/backend/vsg/vfstextureresolver.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>

#include <SDL3/SDL.h>

#include <stdexcept>
#include <utility>

namespace MWRender
{
    bool V4EngineRenderBridge::linkedRuntimeAvailable() noexcept
    {
        return true;
    }

    std::unique_ptr<V4EngineRenderBridge> V4EngineRenderBridge::create(
        const VFS::Manager& vfs, RenderVsg::VsgRuntimeBootstrapOptions options)
    {
        std::shared_ptr<RenderVsg::VsgSemanticSession> session(
            RenderVsg::VsgSemanticSession::create(RenderVsg::makeVfsStaticTextureResolver(vfs), std::move(options)));
        if (!session)
            throw std::runtime_error("V4 engine render bridge received no semantic session");
        return std::unique_ptr<V4EngineRenderBridge>(new V4EngineRenderBridge(vfs, std::move(session)));
    }

    std::unique_ptr<V4EngineRenderBridge> V4EngineRenderBridge::createConfigured(const VFS::Manager& vfs)
    {
        return create(vfs, makeV4RuntimeBootstrapOptions());
    }

    V4EngineRenderBridge::V4EngineRenderBridge(
        const VFS::Manager& vfs, std::shared_ptr<RenderVsg::VsgSemanticSession> session)
        : mVfs(vfs)
        , mSession(std::move(session))
        , mRouteStatus(std::make_shared<V4RenderRouteStatus>())
    {
    }

    V4EngineRenderBridge::~V4EngineRenderBridge()
    {
        waitIdle();
    }

    std::unique_ptr<MWWorld::SceneRenderLifecycle> V4EngineRenderBridge::takeSceneRenderLifecycle()
    {
        if (mLifecycleTaken)
            throw std::logic_error("V4 scene render lifecycle was already taken");
        std::unique_ptr<MWWorld::SceneRenderLifecycle> result
            = std::make_unique<V4SceneRenderLifecycle>(mSession, mVfs, mRouteStatus);
        mLifecycleTaken = true;
        return result;
    }

    std::optional<RenderCore::Extent2D> V4EngineRenderBridge::outputExtent() const noexcept
    {
        if (!mSession)
            return std::nullopt;
        SDL_Window* const window = mSession->bootstrap().sdlWindow();
        if (!window)
            return std::nullopt;
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_HIDDEN) != 0 || (flags & SDL_WINDOW_MINIMIZED) != 0)
            return std::nullopt;
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0)
            return std::nullopt;
        const RenderCore::Extent2D result{
            static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
        if (!result.valid())
            return std::nullopt;
        return result;
    }

    RenderCore::RenderFrameResult V4EngineRenderBridge::renderMainFrame(const V4MainFrameSource& source)
    {
        mLastDiagnostic.clear();
        if (!mRouteStatus->healthy())
        {
            mLastDiagnostic = mRouteStatus->firstDiagnostic();
            if (mLastDiagnostic.empty())
                mLastDiagnostic = "V4 scene publication route is unhealthy";
            return RenderCore::RenderFrameResult::Failed;
        }
        if (!mLifecycleTaken)
        {
            mLastDiagnostic = "scene lifecycle must be attached before rendering a V4 frame";
            return RenderCore::RenderFrameResult::Failed;
        }

        const std::optional<RenderCore::Extent2D> extent = outputExtent();
        if (!extent)
        {
            mLastDiagnostic = "V4 output extent is temporarily unavailable";
            return RenderCore::RenderFrameResult::Skipped;
        }
        RenderCore::SingleViewFrameInput input;
        input.camera = source.camera;
        input.renderExtent = *extent;
        input.outputExtent = *extent;
        input.environment = source.environment;
        input.simulationTime = source.simulationTime;
        input.frameDelta = source.frameDelta;
        input.lodScale = source.lodScale;
        input.invalidateHistory = source.invalidateHistory;
        const RenderCore::RenderFrameResult result = mSession->renderFrame(input);
        mLastDiagnostic = mSession->lastDiagnostic();
        return result;
    }

    void V4EngineRenderBridge::waitIdle()
    {
        if (mSession)
            mSession->waitIdle();
    }
}
