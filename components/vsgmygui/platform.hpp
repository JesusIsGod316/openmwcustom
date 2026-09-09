#ifndef OPENMW_COMPONENTS_VSGMYGUI_PLATFORM_H
#define OPENMW_COMPONENTS_VSGMYGUI_PLATFORM_H

#include <filesystem>
#include <functional>
#include <memory>

#include <components/myguiplatform/platformbase.hpp>
#include <components/vfs/pathutil.hpp>
#include <components/render/backend/vsg/uipipeline.hpp>

#include "texture.hpp" // ImageDecoder

namespace VFS
{
    class Manager;
}
namespace MyGUI
{
    class LogManager;
}
namespace MyGUIPlatform
{
    class DataManager;
    class LogFacility;
}

namespace VsgMyGui
{
    class RenderManager;

    // Ties the MyGUI platform together for the VSG backend: a log facility + log manager, the renderer-agnostic
    // MyGUIPlatform::DataManager (VFS-backed, reused as-is), and our VSG RenderManager. Mirrors
    // MyGUIPlatform::Platform but with no OSG viewer — the view size is passed explicitly and the image decoder is
    // supplied by the host. Construct this BEFORE MyGUI::Gui (its constructors register the MyGUI singletons).
    class Platform : public MyGUIPlatform::PlatformBase
    {
    public:
        using ShutdownCallback = std::function<void(RenderManager*)>;
        Platform(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder, const VFS::Manager* vfs, int viewWidth,
            int viewHeight, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logName = {},
            ShutdownCallback shutdownCallback = {});
        ~Platform();

        void shutdown() override;

        // VSG bakes shaders into the UI pipeline, so there is nothing to switch on here.
        void enableShaders(Shader::ShaderManager&) override {}
        void setViewSize(int width, int height) override;

        RenderManager* getRenderManagerPtr();
        MyGUIPlatform::DataManager* getDataManagerPtr();

    private:
        std::unique_ptr<MyGUIPlatform::LogFacility> mLogFacility;
        std::unique_ptr<MyGUI::LogManager> mLogManager;
        std::unique_ptr<MyGUIPlatform::DataManager> mDataManager;
        std::unique_ptr<RenderManager> mRenderManager;
        ShutdownCallback mShutdownCallback;
    };
}

#endif
