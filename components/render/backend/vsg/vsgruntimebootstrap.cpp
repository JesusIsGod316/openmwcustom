#include "vsgruntimebootstrap.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vsg/app/WindowTraits.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace RenderVsg
{
    std::unique_ptr<VsgRuntimeBootstrap> VsgRuntimeBootstrap::create(
        StaticTextureResolver textureResolver, VsgRuntimeBootstrapOptions options)
    {
        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0)
            throw std::runtime_error("VsgRuntimeBootstrap requires SDL video initialization by the application host");
        if (!textureResolver || options.title.empty() || options.width == 0 || options.height == 0
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
            if (options.hidden)
                flags |= SDL_WINDOW_HIDDEN;
            result->mSdlWindow = SDL_CreateWindow(options.title.c_str(), static_cast<int>(options.width),
                static_cast<int>(options.height), flags);
            if (!result->mSdlWindow)
                throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

            auto traits = vsg::WindowTraits::create(options.width, options.height, options.title);
            traits->vulkanVersion = VK_API_VERSION_1_2;
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
