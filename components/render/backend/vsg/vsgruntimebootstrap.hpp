#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEBOOTSTRAP_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEBOOTSTRAP_H

#include "vsgruntimehost.hpp"

#include <cstdint>
#include <memory>
#include <string>

struct SDL_Window;

namespace RenderVsg
{
    enum class VsgWindowMode : std::uint8_t
    {
        Windowed,
        Fullscreen,
        WindowedFullscreen,
    };

    enum class VsgPresentMode : std::uint8_t
    {
        Immediate,
        VSync,
        Adaptive,
    };

    struct VsgRuntimeBootstrapOptions
    {
        std::string title = "OpenMW V4 Vulkan";
        std::uint32_t width = 1280;
        std::uint32_t height = 720;
        int displayIndex = 0;
        VsgWindowMode windowMode = VsgWindowMode::Windowed;
        VsgPresentMode presentMode = VsgPresentMode::VSync;
        bool resizable = true;
        bool highPixelDensity = true;
        bool windowBorder = true;
        bool minimizeOnFocusLoss = true;
        bool hidden = false;
        VsgRuntimeHostOptions host;
    };

    // Owns a distinct SDL Vulkan window and all backend objects attached to it.
    // SDL video initialization remains the application host's responsibility.
    // Destruction is deliberately ordered: runtime/fences, VSG window and
    // Vulkan surface, SDL window, then the loader reference acquired here.
    class VsgRuntimeBootstrap final
    {
    public:
        [[nodiscard]] static std::unique_ptr<VsgRuntimeBootstrap> create(
            StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options = {});

        ~VsgRuntimeBootstrap();
        VsgRuntimeBootstrap(const VsgRuntimeBootstrap&) = delete;
        VsgRuntimeBootstrap& operator=(const VsgRuntimeBootstrap&) = delete;

        [[nodiscard]] VsgRuntimeHost& renderer() noexcept { return *mRenderer; }
        [[nodiscard]] const VsgRuntimeHost& renderer() const noexcept { return *mRenderer; }
        [[nodiscard]] SDL_Window* sdlWindow() const noexcept { return mSdlWindow; }
        [[nodiscard]] vsg::ref_ptr<SdlVulkanWindow> vsgWindow() const noexcept { return mVsgWindow; }

    private:
        VsgRuntimeBootstrap() = default;
        void release() noexcept;

        bool mVulkanLoaderAcquired = false;
        SDL_Window* mSdlWindow = nullptr;
        vsg::ref_ptr<SdlVulkanWindow> mVsgWindow;
        std::unique_ptr<VsgRuntimeHost> mRenderer;
    };
}

#endif
