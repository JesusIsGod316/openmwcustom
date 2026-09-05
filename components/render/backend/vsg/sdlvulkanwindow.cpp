#include "sdlvulkanwindow.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vsg/core/Exception.h>
#include <vsg/vk/Instance.h>
#include <vsg/vk/Surface.h>

#include <utility>

namespace RenderVsg
{
    SdlVulkanWindow::SdlVulkanWindow(SDL_Window* window, vsg::ref_ptr<vsg::WindowTraits> traits)
        : vsg::Inherit<vsg::Window, SdlVulkanWindow>(std::move(traits))
        , mWindow(window)
    {
        if (!mWindow)
            throw vsg::Exception{ "SdlVulkanWindow requires a valid SDL_Window", VK_ERROR_INITIALIZATION_FAILED };
        if ((SDL_GetWindowFlags(mWindow) & SDL_WINDOW_VULKAN) == 0)
            throw vsg::Exception{ "SdlVulkanWindow requires SDL_WINDOW_VULKAN", VK_ERROR_INITIALIZATION_FAILED };
        if (!refreshExtent())
            throw vsg::Exception{ "Unable to query SDL Vulkan window pixel extent", VK_ERROR_INITIALIZATION_FAILED };
    }

    SdlVulkanWindow::~SdlVulkanWindow()
    {
        // Release Vulkan resources and the VkSurfaceKHR while SDL's native
        // window is still alive. Engine/probe code destroys SDL_Window only
        // after all VSG references to this adapter have been released.
        clear();
        mWindow = nullptr;
    }

    const char* SdlVulkanWindow::instanceExtensionSurfaceName() const
    {
#ifdef _WIN32
        return "VK_KHR_win32_surface";
#else
#    error V4 CP2 SdlVulkanWindow is currently validated for Windows only
#endif
    }

    bool SdlVulkanWindow::valid() const
    {
        return mWindow != nullptr;
    }

    bool SdlVulkanWindow::visible() const
    {
        if (!mWindow)
            return false;
        const SDL_WindowFlags flags = SDL_GetWindowFlags(mWindow);
        return (flags & SDL_WINDOW_HIDDEN) == 0 && (flags & SDL_WINDOW_MINIMIZED) == 0;
    }

    void SdlVulkanWindow::releaseWindow()
    {
        mWindow = nullptr;
    }

    bool SdlVulkanWindow::refreshExtent() noexcept
    {
        if (!mWindow)
            return false;

        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(mWindow, &width, &height) || width <= 0 || height <= 0)
            return false;

        _extent2D.width = static_cast<std::uint32_t>(width);
        _extent2D.height = static_cast<std::uint32_t>(height);
        if (_traits)
        {
            _traits->width = _extent2D.width;
            _traits->height = _extent2D.height;
        }
        return true;
    }

    void SdlVulkanWindow::resize()
    {
        if (!refreshExtent())
            return;

        // buildSwapchain() performs a device-idle synchronization before
        // replacing swapchain-owned resources. CP2 intentionally favors the
        // conservative correctness path; CP6+ may make resize recreation more
        // asynchronous once frame ownership is fully integrated.
        if (_swapchain)
            buildSwapchain();
    }

    void SdlVulkanWindow::_initSurface()
    {
        if (!_instance)
            _initInstance();

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        const auto* allocator = static_cast<const VkAllocationCallbacks*>(_instance->getAllocationCallbacks());
        if (!SDL_Vulkan_CreateSurface(mWindow, _instance->vk(), allocator, &surface))
            throw vsg::Exception{ std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError(),
                VK_ERROR_INITIALIZATION_FAILED };

        _surface = vsg::Surface::create(surface, _instance);
    }
}
