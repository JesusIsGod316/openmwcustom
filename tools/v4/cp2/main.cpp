#include <components/render/backend/vsg/residencyledger.hpp>
#include <components/render/backend/vsg/sdlvulkanwindow.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vsg/all.h>

#include <charconv>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace
{
    int parseFrameLimit(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::string_view(argv[i]) != "--frames")
                continue;
            int value = -1;
            const std::string_view text(argv[i + 1]);
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            if (result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= 0)
                return value;
        }
        return -1;
    }

    void printMemoryTelemetry(vsg::PhysicalDevice& physicalDevice)
    {
        const VkPhysicalDeviceProperties& properties = physicalDevice.getProperties();
        std::cout << "V4 CP2 Vulkan device: " << properties.deviceName << '\n';
        std::cout << "Vulkan API: " << VK_VERSION_MAJOR(properties.apiVersion) << '.'
                  << VK_VERSION_MINOR(properties.apiVersion) << '.' << VK_VERSION_PATCH(properties.apiVersion) << '\n';

        VkPhysicalDeviceMemoryProperties2 memoryProperties{};
        memoryProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;

        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;

        const bool hasBudget = physicalDevice.supportsDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        if (hasBudget)
            memoryProperties.pNext = &budget;
        vkGetPhysicalDeviceMemoryProperties2(physicalDevice.vk(), &memoryProperties);

        constexpr double mib = 1024.0 * 1024.0;
        for (std::uint32_t i = 0; i < memoryProperties.memoryProperties.memoryHeapCount; ++i)
        {
            const VkMemoryHeap& heap = memoryProperties.memoryProperties.memoryHeaps[i];
            const bool deviceLocal = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
            std::cout << "Memory heap " << i << (deviceLocal ? " device-local" : " host/shared")
                      << ": size=" << static_cast<double>(heap.size) / mib << " MiB";
            if (hasBudget)
            {
                std::cout << " budget=" << static_cast<double>(budget.heapBudget[i]) / mib
                          << " usage=" << static_cast<double>(budget.heapUsage[i]) / mib;
            }
            std::cout << '\n';
        }
    }

    vsg::ref_ptr<vsg::Node> createPipelineProofScene()
    {
        auto builder = vsg::Builder::create();
        vsg::GeometryInfo geometry;
        geometry.position = vsg::vec3(0.0f, 0.0f, 0.0f);
        geometry.dx = vsg::vec3(1.5f, 0.0f, 0.0f);
        geometry.dy = vsg::vec3(0.0f, 1.5f, 0.0f);
        geometry.dz = vsg::vec3(0.0f, 0.0f, 1.5f);
        geometry.color = vsg::vec4(0.72f, 0.82f, 1.0f, 1.0f);

        vsg::StateInfo state;
        state.lighting = false;
        state.two_sided = false;
        state.blending = false;

        return builder->createBox(geometry, state);
    }
}

int main(int argc, char** argv)
{
    const int frameLimit = parseFrameLimit(argc, argv);

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << '\n';
        return 1;
    }

    bool vulkanLibraryLoaded = false;
    SDL_Window* sdlWindow = nullptr;
    int exitCode = 0;

    try
    {
        if (!SDL_Vulkan_LoadLibrary(nullptr))
            throw std::runtime_error(std::string("SDL_Vulkan_LoadLibrary failed: ") + SDL_GetError());
        vulkanLibraryLoaded = true;

        constexpr int initialWidth = 1280;
        constexpr int initialHeight = 720;
        constexpr SDL_WindowFlags windowFlags
            = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        sdlWindow = SDL_CreateWindow("OpenMW V4 CP2 - VSG/Vulkan Foundation", initialWidth, initialHeight, windowFlags);
        if (!sdlWindow)
            throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

        {
            auto traits = vsg::WindowTraits::create(
                static_cast<std::uint32_t>(initialWidth), static_cast<std::uint32_t>(initialHeight),
                "OpenMW V4 CP2 - VSG/Vulkan Foundation");
            traits->vulkanVersion = VK_API_VERSION_1_2;
            traits->deviceTypePreferences = {
                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                VK_PHYSICAL_DEVICE_TYPE_CPU,
            };

            auto window = RenderVsg::SdlVulkanWindow::create(sdlWindow, traits);
            auto physicalDevice = window->getOrCreatePhysicalDevice();
            if (!physicalDevice)
                throw std::runtime_error("VSG could not select a Vulkan physical device");

            if (physicalDevice->supportsDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
                traits->deviceExtensionNames.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

            printMemoryTelemetry(*physicalDevice);

            auto scene = createPipelineProofScene();
            if (!scene)
                throw std::runtime_error("VSG Builder failed to create the CP2 pipeline-proof scene");

            auto viewer = vsg::Viewer::create();
            viewer->addWindow(window);

            const VkExtent2D extent = window->extent2D();
            const double aspect = extent.height == 0 ? 1.0 : static_cast<double>(extent.width) / extent.height;
            auto lookAt = vsg::LookAt::create(
                vsg::dvec3(2.6, -3.8, 2.2), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, 1.0));
            auto perspective = vsg::Perspective::create(45.0, aspect, 0.05, 100.0);
            auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(extent));

            auto commandGraph = vsg::createCommandGraphForView(window, camera, scene);
            viewer->assignRecordAndSubmitTaskAndPresentation({ commandGraph });

            viewer->compile();
            std::cout << "V4 CP2 VSG compile: PASS (render graph + shader/pipeline + swapchain)\n";

            RenderVsg::ResidencyLedger residency;
            const std::uint64_t colorBytes
                = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height) * 4u;
            const std::uint64_t depthBytes
                = static_cast<std::uint64_t>(extent.width) * static_cast<std::uint64_t>(extent.height) * 4u;
            const std::uint64_t estimatedFoundationTargets = colorBytes + depthBytes;
            residency.addLogicalLive(RenderVsg::ResidencyCategory::RenderTargetsHistory, estimatedFoundationTargets);
            std::cout << "CP2 tracked foundation target estimate: " << estimatedFoundationTargets / (1024.0 * 1024.0)
                      << " MiB (logical estimate; adapter heap usage above is authoritative for total driver residency)\n";

            bool running = true;
            int renderedFrames = 0;
            while (running && viewer->advanceToNextFrame())
            {
                SDL_Event event;
                while (SDL_PollEvent(&event))
                {
                    switch (event.type)
                    {
                        case SDL_EVENT_QUIT:
                        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                            running = false;
                            break;
                        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                        case SDL_EVENT_WINDOW_RESIZED:
                            window->resize();
                            if (window->extent2D().width != 0 && window->extent2D().height != 0)
                            {
                                camera->viewportState = vsg::ViewportState::create(window->extent2D());
                                perspective->aspectRatio = static_cast<double>(window->extent2D().width)
                                    / static_cast<double>(window->extent2D().height);
                            }
                            break;
                        default:
                            break;
                    }
                }

                if (!running)
                    break;

                viewer->update();
                viewer->recordAndSubmit();
                viewer->present();

                ++renderedFrames;
                if (frameLimit >= 0 && renderedFrames >= frameLimit)
                    running = false;
            }

            viewer->close();
            viewer->deviceWaitIdle();
            std::cout << "V4 CP2 clean shutdown after " << renderedFrames << " rendered frames\n";
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "V4 CP2 foundation failure: " << e.what() << '\n';
        exitCode = 2;
    }
    catch (...)
    {
        std::cerr << "V4 CP2 foundation failure: unknown exception\n";
        exitCode = 3;
    }

    if (sdlWindow)
        SDL_DestroyWindow(sdlWindow);
    if (vulkanLibraryLoaded)
        SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return exitCode;
}
