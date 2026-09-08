#include "vsgruntimebootstrap.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vsg/app/WindowTraits.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] VkPresentModeKHR presentMode(VsgPresentMode mode) noexcept
        {
            switch (mode)
            {
                case VsgPresentMode::Immediate: return VK_PRESENT_MODE_IMMEDIATE_KHR;
                case VsgPresentMode::Adaptive: return VK_PRESENT_MODE_FIFO_RELAXED_KHR;
                case VsgPresentMode::VSync: return VK_PRESENT_MODE_FIFO_KHR;
            }
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        [[nodiscard]] bool valid(VsgWindowMode mode) noexcept
        {
            return mode == VsgWindowMode::Windowed || mode == VsgWindowMode::Fullscreen
                || mode == VsgWindowMode::WindowedFullscreen;
        }

        [[nodiscard]] bool valid(VsgPresentMode mode) noexcept
        {
            return mode == VsgPresentMode::Immediate || mode == VsgPresentMode::VSync
                || mode == VsgPresentMode::Adaptive;
        }

        [[nodiscard]] SDL_DisplayID selectDisplay(int index) noexcept
        {
            int count = 0;
            SDL_DisplayID* const displays = SDL_GetDisplays(&count);
            SDL_DisplayID result = 0;
            if (displays && index >= 0 && index < count)
                result = displays[index];
            SDL_free(displays);
            return result ? result : SDL_GetPrimaryDisplay();
        }
    }

    std::unique_ptr<VsgRuntimeBootstrap> VsgRuntimeBootstrap::create(
        StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options)
    {
        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
            throw std::runtime_error("VsgRuntimeBootstrap requires SDL video initialization by the application host");
        if (!textureResolver || options.title.empty() || options.width == 0 || options.height == 0
            || options.displayIndex < 0 || !valid(options.windowMode) || !valid(options.presentMode)
            || options.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
            || options.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
            throw std::invalid_argument("VsgRuntimeBootstrap received invalid window or texture-resolver options");

        std::unique_ptr<VsgRuntimeBootstrap> result(new VsgRuntimeBootstrap);
        try
        {
            if (!SDL_Vulkan_LoadLibrary(nullptr))
                throw std::runtime_error(std::string("SDL_Vulkan_LoadLibrary failed: ") + SDL_GetError());
            result->mVulkanLoaderAcquired = true;

            SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
            if (options.resizable)
                flags |= SDL_WINDOW_RESIZABLE;
            if (options.highPixelDensity)
                flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
            if (!options.windowBorder)
                flags |= SDL_WINDOW_BORDERLESS;
            if (options.hidden)
                flags |= SDL_WINDOW_HIDDEN;
            SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, options.minimizeOnFocusLoss ? "1" : "0");
            result->mSdlWindow = SDL_CreateWindow(options.title.c_str(), static_cast<int>(options.width),
                static_cast<int>(options.height), flags);
            if (!result->mSdlWindow)
                throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

            const SDL_DisplayID display = selectDisplay(options.displayIndex);
            if (options.windowMode == VsgWindowMode::Windowed)
                SDL_SetWindowPosition(result->mSdlWindow, SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                    SDL_WINDOWPOS_CENTERED_DISPLAY(display));
            else
            {
                if (options.windowMode == VsgWindowMode::Fullscreen)
                {
                    SDL_DisplayMode mode{};
                    if (display && SDL_GetClosestFullscreenDisplayMode(display, static_cast<int>(options.width),
                                       static_cast<int>(options.height), 0.f, true, &mode))
                    {
                        if (!SDL_SetWindowFullscreenMode(result->mSdlWindow, &mode))
                            throw std::runtime_error(
                                std::string("SDL_SetWindowFullscreenMode failed: ") + SDL_GetError());
                    }
                }
                else if (!SDL_SetWindowFullscreenMode(result->mSdlWindow, nullptr))
                    throw std::runtime_error(
                        std::string("SDL_SetWindowFullscreenMode failed: ") + SDL_GetError());
                if (!SDL_SetWindowFullscreen(result->mSdlWindow, true))
                    throw std::runtime_error(std::string("SDL_SetWindowFullscreen failed: ") + SDL_GetError());
            }

            auto traits = vsg::WindowTraits::create(options.width, options.height, options.title);
            traits->vulkanVersion = VK_API_VERSION_1_2;
            traits->swapchainPreferences.presentMode = presentMode(options.presentMode);
            traits->deviceTypePreferences = {
                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                VK_PHYSICAL_DEVICE_TYPE_CPU,
            };
            result->mVsgWindow = SdlVulkanWindow::create(result->mSdlWindow, traits);
            if (!result->mVsgWindow->getOrCreatePhysicalDevice())
                throw std::runtime_error("VSG could not select a Vulkan physical device");

            result->mRenderer = std::make_unique<VsgRuntimeHost>(
                result->mVsgWindow, std::move(textureResolver), std::move(options.host));
            return result;
        }
        catch (...)
        {
            result->release();
            throw;
        }
    }

    VsgRuntimeBootstrap::~VsgRuntimeBootstrap()
    {
        release();
    }

    void VsgRuntimeBootstrap::release() noexcept
    {
        mRenderer.reset();
        mVsgWindow = {};
        if (mSdlWindow)
        {
            SDL_DestroyWindow(mSdlWindow);
            mSdlWindow = nullptr;
        }
        if (mVulkanLoaderAcquired)
        {
            SDL_Vulkan_UnloadLibrary();
            mVulkanLoaderAcquired = false;
        }
    }
}
