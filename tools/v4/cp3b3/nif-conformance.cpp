#include <components/files/conversion.hpp>
#include <components/nif/niffile.hpp>
#include <components/render/backend/vsg/sdlvulkanwindow.hpp>
#include <components/render/backend/vsg/staticassetconformance.hpp>
#include <components/render/backend/vsg/staticnifconformance.hpp>
#include <components/rendercore/updatebatch.hpp>
#include <components/vfs/bsaarchive.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <tools/v4/cp3b4/conformance-report.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vsg/all.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    struct Options
    {
        std::vector<std::filesystem::path> dataRoots;
        std::vector<std::filesystem::path> archives;
        std::string nifPath;
        std::filesystem::path reportJson;
        float lodDistance = 0.0f;
        double cameraDistance = 500.0;
        int frameLimit = -1;
        bool realizeOnly = false;
        bool help = false;
    };

    [[nodiscard]] bool parseInteger(std::string_view text, int& result)
    {
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
    }

    [[nodiscard]] bool parseFloat(std::string_view text, float& result)
    {
        try
        {
            std::size_t consumed = 0;
            const std::string value(text);
            result = std::stof(value, &consumed);
            return consumed == value.size();
        }
        catch (...)
        {
            return false;
        }
    }

    [[nodiscard]] bool parseDouble(std::string_view text, double& result)
    {
        try
        {
            std::size_t consumed = 0;
            const std::string value(text);
            result = std::stod(value, &consumed);
            return consumed == value.size();
        }
        catch (...)
        {
            return false;
        }
    }

    void printUsage(std::ostream& out)
    {
        out << "OpenMW V4 CP3B3/CP3B4 real-NIF Vulkan conformance tool\n\n"
               "Usage:\n"
               "  openmw-vulkan-nif-conformance --data <dir> [--data <dir> ...]\n"
               "      [--archive <bsa-or-ba2> ...] --nif <vfs/path/model.nif> [options]\n\n"
               "Mount order follows OpenMW VFS precedence: later data roots and later archives win.\n"
               "The NIF path must be relative to the mounted VFS (for example meshes/foo/bar.nif).\n\n"
               "Options:\n"
               "  --lod-distance <value>     Static LOD selection eye distance (default 0).\n"
               "  --camera-distance <value> Fixed conformance camera distance (default 500).\n"
               "  --frames <count>           Render exactly count frames, then exit.\n"
               "  --realize-only             Parse/translate/publish/plan/realize without opening a window.\n"
               "  --report-json <path>       Write CP3B4 machine-readable per-asset report JSON.\n"
               "  --help, -h                 Show this help.\n";
    }

    [[nodiscard]] Options parseOptions(int argc, char** argv)
    {
        Options options;
        for (int i = 1; i < argc; ++i)
        {
            const std::string_view arg(argv[i]);
            if (arg == "--help" || arg == "-h")
            {
                options.help = true;
                continue;
            }
            if (arg == "--realize-only")
            {
                options.realizeOnly = true;
                continue;
            }

            if (i + 1 >= argc)
                throw std::runtime_error("missing value for option " + std::string(arg));
            const std::string_view value(argv[++i]);

            if (arg == "--data")
                options.dataRoots.emplace_back(std::string(value));
            else if (arg == "--archive")
                options.archives.emplace_back(std::string(value));
            else if (arg == "--nif")
                options.nifPath = std::string(value);
            else if (arg == "--report-json")
                options.reportJson = std::filesystem::path(std::string(value));
            else if (arg == "--lod-distance")
            {
                if (!parseFloat(value, options.lodDistance) || options.lodDistance < 0.0f)
                    throw std::runtime_error("--lod-distance must be a non-negative number");
            }
            else if (arg == "--camera-distance")
            {
                if (!parseDouble(value, options.cameraDistance) || options.cameraDistance <= 0.0)
                    throw std::runtime_error("--camera-distance must be greater than zero");
            }
            else if (arg == "--frames")
            {
                if (!parseInteger(value, options.frameLimit) || options.frameLimit < 0)
                    throw std::runtime_error("--frames must be a non-negative integer");
            }
            else
                throw std::runtime_error("unknown option " + std::string(arg));
        }
        return options;
    }

    void printTranslationDiagnostics(const RenderVsg::StaticNifConformanceResult& result)
    {
        for (const NifRender::TranslationDiagnostic& diagnostic : result.translationDiagnostics)
        {
            const char* severity = "info";
            if (diagnostic.severity == NifRender::DiagnosticSeverity::Warning)
                severity = "warning";
            else if (diagnostic.severity == NifRender::DiagnosticSeverity::Error)
                severity = "error";

            std::cout << "translation " << severity << " [" << diagnostic.code << "]";
            if (diagnostic.sourceRecordId)
                std::cout << " record=" << *diagnostic.sourceRecordId;
            if (!diagnostic.sourceRecordType.empty())
                std::cout << " type=" << diagnostic.sourceRecordType;
            std::cout << ": " << diagnostic.message << '\n';
        }
    }

    void printResult(const RenderVsg::StaticNifConformanceResult& result)
    {
        const NifRender::TranslationSummary& summary = result.translationSummary;
        std::cout << "Translation outcomes: rendered=" << summary.rendered << " collisionOnly=" << summary.collisionOnly
                  << " hidden=" << summary.hidden << " deferred=" << summary.deferred
                  << " unsupported=" << summary.unsupported << " ignored=" << summary.ignored << '\n';
        printTranslationDiagnostics(result);

        if (!result.complete())
        {
            std::cout << "CP3B3 conformance stopped at stage " << static_cast<unsigned int>(result.stage) << '\n';
            for (const std::string& diagnostic : result.realization.diagnostics)
                std::cout << "realization: " << diagnostic << '\n';
            return;
        }

        const RenderVsg::StaticRealizationStats& stats = result.realization.stats;
        std::cout << "VSG realization: draws=" << stats.drawCount << " sorted=" << stats.sortedDrawCount
                  << " billboards=" << stats.billboardDraws << " pipelines=" << stats.pipelineKeys
                  << " materials=" << stats.materialKeys << " textureViews=" << stats.textureViewKeys
                  << " samplers=" << stats.samplerKeys << " textureLoads=" << stats.textureLoads
                  << " textureCacheHits=" << stats.textureCacheHits
                  << " unsupportedTextureBindings=" << stats.unsupportedTextureBindings << '\n';
        for (const std::string& diagnostic : result.realization.diagnostics)
            std::cout << "realization: " << diagnostic << '\n';
    }

    [[nodiscard]] int renderScene(vsg::ref_ptr<vsg::Node> scene, double cameraDistance, int frameLimit)
    {
        if (!scene)
            throw std::runtime_error("cannot render an empty CP3B3 scene");
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());

        bool vulkanLibraryLoaded = false;
        SDL_Window* sdlWindow = nullptr;
        try
        {
            if (!SDL_Vulkan_LoadLibrary(nullptr))
                throw std::runtime_error(std::string("SDL_Vulkan_LoadLibrary failed: ") + SDL_GetError());
            vulkanLibraryLoaded = true;

            constexpr int initialWidth = 1280;
            constexpr int initialHeight = 720;
            constexpr SDL_WindowFlags windowFlags
                = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
            sdlWindow = SDL_CreateWindow(
                "OpenMW V4 CP3B3 - real NIF Vulkan conformance", initialWidth, initialHeight, windowFlags);
            if (!sdlWindow)
                throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());

            auto traits = vsg::WindowTraits::create(static_cast<std::uint32_t>(initialWidth),
                static_cast<std::uint32_t>(initialHeight), "OpenMW V4 CP3B3 - real NIF Vulkan conformance");
            traits->vulkanVersion = VK_API_VERSION_1_2;
            traits->deviceTypePreferences = {
                VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU,
                VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU,
                VK_PHYSICAL_DEVICE_TYPE_CPU,
            };

            auto window = RenderVsg::SdlVulkanWindow::create(sdlWindow, traits);
            if (!window->getOrCreatePhysicalDevice())
                throw std::runtime_error("VSG could not select a Vulkan physical device");

            auto viewer = vsg::Viewer::create();
            viewer->addWindow(window);

            const VkExtent2D extent = window->extent2D();
            const double aspect = extent.height == 0 ? 1.0 : static_cast<double>(extent.width) / extent.height;
            auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, -cameraDistance, cameraDistance * 0.35),
                vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, 1.0));
            auto perspective = vsg::Perspective::create(
                45.0, aspect, std::max(0.01, cameraDistance * 0.0001), cameraDistance * 100.0);
            auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(extent));

            auto view = vsg::View::create(camera, scene);
            view->bins = RenderVsg::createStaticConformanceBins();
            auto renderGraph = vsg::RenderGraph::create(window);
            renderGraph->addChild(view);
            auto commandGraph = vsg::CommandGraph::create(window);
            commandGraph->addChild(renderGraph);
            viewer->assignRecordAndSubmitTaskAndPresentation({ commandGraph });
            viewer->compile();
            std::cout << "CP3B3 Vulkan compile: PASS (real NIF VSG scene + compatibility bins + swapchain)\n";

            bool running = true;
            int renderedFrames = 0;
            while (running && viewer->advanceToNextFrame())
            {
                bool resizePending = false;
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
                            resizePending = true;
                            break;
                        default:
                            break;
                    }
                }
                if (!running)
                    break;
                if (resizePending)
                    window->resize();

                viewer->update();
                viewer->recordAndSubmit();
                viewer->present();

                ++renderedFrames;
                if (frameLimit >= 0 && renderedFrames >= frameLimit)
                    running = false;
            }

            viewer->close();
            viewer->deviceWaitIdle();
            std::cout << "CP3B3 clean shutdown after " << renderedFrames << " rendered frames\n";
        }
        catch (...)
        {
            if (sdlWindow)
                SDL_DestroyWindow(sdlWindow);
            if (vulkanLibraryLoaded)
                SDL_Vulkan_UnloadLibrary();
            SDL_Quit();
            throw;
        }

        if (sdlWindow)
            SDL_DestroyWindow(sdlWindow);
        if (vulkanLibraryLoaded)
            SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return 0;
    }
}

int main(int argc, char** argv)
{
    try
    {
        const Options options = parseOptions(argc, argv);
        if (options.help)
        {
            printUsage(std::cout);
            return 0;
        }
        if (options.dataRoots.empty())
            throw std::runtime_error("at least one --data root is required");
        if (options.nifPath.empty())
            throw std::runtime_error("--nif <VFS path> is required");

        VFS::Manager vfs;
        for (const std::filesystem::path& root : options.dataRoots)
        {
            if (!std::filesystem::is_directory(root))
                throw std::runtime_error("data root is not a directory: " + Files::pathToUnicodeString(root));
            vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(root));
        }
        for (const std::filesystem::path& archive : options.archives)
        {
            if (!std::filesystem::is_regular_file(archive))
                throw std::runtime_error("archive is not a file: " + Files::pathToUnicodeString(archive));
            vfs.addArchive(VFS::makeBsaArchive(archive, nullptr));
        }
        vfs.buildIndex();

        const VFS::Path::Normalized nifPath(options.nifPath);
        if (!vfs.exists(nifPath))
            throw std::runtime_error("NIF is not present in the mounted VFS: " + options.nifPath);

        Nif::NIFFile nifFile(nifPath);
        Nif::Reader reader(nifFile, nullptr);
        reader.parse(vfs.get(nifPath));
        const Nif::FileView fileView(nifFile);

        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderVsg::StaticPlanOptions planOptions;
        planOptions.lodEyeDistance = options.lodDistance;
        auto sharedObjects = vsg::SharedObjects::create();
        RenderVsg::StaticNifConformanceResult result
            = RenderVsg::realizeStaticNif(fileView, vfs, world, publisher, planOptions, sharedObjects);
        printResult(result);
        if (!options.reportJson.empty())
        {
            Cp3b4::writeAssetReport(options.reportJson, options.nifPath, result);
            std::cout << "CP3B4 report: " << Files::pathToUnicodeString(options.reportJson) << '\n';
        }
        if (!result.complete())
            return 3;

        if (options.realizeOnly)
        {
            std::cout << "CP3B3 real-NIF source-to-VSG conformance: PASS (realize-only)\n";
            return 0;
        }

        return renderScene(result.realization.root, options.cameraDistance, options.frameLimit);
    }
    catch (const std::exception& e)
    {
        std::cerr << "CP3B3 conformance failure: " << e.what() << '\n';
        printUsage(std::cerr);
        return 2;
    }
    catch (...)
    {
        std::cerr << "CP3B3 conformance failure: unknown exception\n";
        return 4;
    }
}
