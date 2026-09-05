#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_SDLVULKANWINDOW_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_SDLVULKANWINDOW_H

#include <vsg/app/Window.h>

struct SDL_Window;

namespace RenderVsg
{
    // VSG window adapter for an SDL3-owned native window. SDL remains the
    // platform/input/window authority established by CP1B; VSG owns only the
    // Vulkan instance/device/surface/swapchain objects attached to it.
    class SdlVulkanWindow final : public vsg::Inherit<vsg::Window, SdlVulkanWindow>
    {
    public:
        SdlVulkanWindow(SDL_Window* window, vsg::ref_ptr<vsg::WindowTraits> traits);

        [[nodiscard]] const char* instanceExtensionSurfaceName() const override;
        [[nodiscard]] bool valid() const override;
        [[nodiscard]] bool visible() const override;
        void releaseWindow() override;
        void resize() override;

        [[nodiscard]] SDL_Window* sdlWindow() const noexcept { return mWindow; }

    protected:
        ~SdlVulkanWindow() override;
        void _initSurface() override;

    private:
        bool refreshExtent() noexcept;

        SDL_Window* mWindow = nullptr;
    };
}

#endif