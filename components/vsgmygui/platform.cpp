#include "platform.hpp"

#include <utility>

#include <MyGUI_LogManager.h>

#include <components/myguiplatform/myguidatamanager.hpp>
#include <components/myguiplatform/myguiloglistener.hpp>

#include "rendermanager.hpp"

namespace VsgMyGui
{
    Platform::Platform(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder, const VFS::Manager* vfs,
        int viewWidth, int viewHeight, VFS::Path::NormalizedView resourcePath, const std::filesystem::path& logName,
        ShutdownCallback shutdownCallback)
        : mLogFacility(logName.empty() ? nullptr : std::make_unique<MyGUIPlatform::LogFacility>(logName, false))
        , mLogManager(std::make_unique<MyGUI::LogManager>())
        , mDataManager(std::make_unique<MyGUIPlatform::DataManager>(resourcePath, vfs))
        , mRenderManager(std::make_unique<RenderManager>(pipeline, std::move(decoder)))
        , mShutdownCallback(std::move(shutdownCallback))
    {
        if (mLogFacility != nullptr)
            mLogManager->addLogSource(mLogFacility->getSource());

        mRenderManager->initialise(viewWidth, viewHeight);
    }

    Platform::~Platform()
    {
        if (mShutdownCallback)
            mShutdownCallback(mRenderManager.get());
    }

    void Platform::shutdown()
    {
        mRenderManager->shutdown();
    }

    void Platform::setViewSize(int width, int height)
    {
        mRenderManager->setViewSize(width, height);
    }

    RenderManager* Platform::getRenderManagerPtr()
    {
        return mRenderManager.get();
    }

    MyGUIPlatform::DataManager* Platform::getDataManagerPtr()
    {
        return mDataManager.get();
    }
}
