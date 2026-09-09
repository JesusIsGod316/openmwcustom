#ifndef OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIPLATFORM_H
#define OPENMW_COMPONENTS_MYGUIPLATFORM_MYGUIPLATFORM_H

#include <filesystem>
#include <memory>
#include <string>

#include <components/vfs/pathutil.hpp>

#include "platformbase.hpp"

namespace osgViewer
{
    class Viewer;
}
namespace osg
{
    class Group;
}
namespace Resource
{
    class ImageManager;
}
namespace MyGUI
{
    class LogManager;
}
namespace VFS
{
    class Manager;
}

namespace MyGUIPlatform
{

    class RenderManager;
    class DataManager;
    class LogFacility;

    class Platform final : public PlatformBase
    {
    public:
        Platform(osgViewer::Viewer* viewer, osg::Group* guiRoot, Resource::ImageManager* imageManager,
            const VFS::Manager* vfs, float uiScalingFactor, VFS::Path::NormalizedView resourcePath,
            const std::filesystem::path& logName = "MyGUI.log");

        ~Platform() override;

        void shutdown() override;
        void enableShaders(Shader::ShaderManager& shaderManager) override;
        void setViewSize(int width, int height) override;

        RenderManager* getRenderManagerPtr();

        DataManager* getDataManagerPtr();

    private:
        std::unique_ptr<LogFacility> mLogFacility;
        std::unique_ptr<MyGUI::LogManager> mLogManager;
        std::unique_ptr<DataManager> mDataManager;
        std::unique_ptr<RenderManager> mRenderManager;
    };

}

#endif
