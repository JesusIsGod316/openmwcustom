#include "videowidget.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

#include <osg-ffmpeg-videoplayer/videoplayer.hpp>

#include <MyGUI_RenderManager.h>

#include <osg/Image>
#include <osg/Texture2D>

#include <components/debug/debuglog.hpp>
#include <components/myguiplatform/myguitexture.hpp>
#include <components/vfs/manager.hpp>

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
#include <components/vsgmygui/rendermanager.hpp>
#include <components/vsgmygui/texture.hpp>
#endif

#include "../mwsound/movieaudiofactory.hpp"

namespace MWGui
{
    namespace
    {
#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        bool updateVulkanVideoTexture(Video::VideoPlayer& player, VsgMyGui::Texture& target)
        {
            osg::ref_ptr<osg::Texture2D> decodedTexture = player.getVideoTexture();
            osg::Image* const image = decodedTexture ? decodedTexture->getImage() : nullptr;
            if (!image || !image->data())
                return false;

            const int width = image->s();
            const int height = image->t();
            if (width <= 0 || height <= 0 || image->r() != 1 || image->getPixelFormat() != GL_RGBA
                || image->getDataType() != GL_UNSIGNED_BYTE)
            {
                Log(Debug::Warning) << "V4 Vulkan video bridge received an unsupported decoded frame layout";
                return false;
            }

            const std::size_t pixelCount = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
            if (pixelCount > std::numeric_limits<std::size_t>::max() / 4)
                return false;
            const std::size_t byteCount = pixelCount * 4;
            if (image->getImageSizeInBytes() < byteCount)
            {
                Log(Debug::Warning) << "V4 Vulkan video bridge received a truncated decoded RGBA frame";
                return false;
            }

            auto rgba = vsg::ubvec4Array2D::create(static_cast<std::uint32_t>(width),
                static_cast<std::uint32_t>(height), vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
            if (!rgba || !rgba->dataPointer())
                return false;
            rgba->properties.origin = vsg::TOP_LEFT;
            std::memcpy(rgba->dataPointer(), image->data(), byteCount);
            target.setData(std::move(rgba));
            return true;
        }
#endif
    }

    VideoWidget::VideoWidget()
        : mVFS(nullptr)
    {
        mPlayer = std::make_unique<Video::VideoPlayer>();
        setNeedKeyFocus(true);
    }

    VideoWidget::~VideoWidget() = default;

    void VideoWidget::setVFS(const VFS::Manager* vfs)
    {
        mVFS = vfs;
    }

    void VideoWidget::playVideo(const std::string& video)
    {
        mPlayer->setAudioFactory(new MWSound::MovieAudioFactory());

        Files::IStreamPtr videoStream;
        try
        {
            videoStream = mVFS->get(video);
        }
        catch (std::exception& e)
        {
            Log(Debug::Error) << "Failed to open video: " << e.what();
            return;
        }

        mPlayer->playVideo(std::move(videoStream), video);

        osg::ref_ptr<osg::Texture2D> texture = mPlayer->getVideoTexture();
        if (!texture)
            return;

#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        if (dynamic_cast<VsgMyGui::RenderManager*>(&MyGUI::RenderManager::getInstance()))
        {
            auto nativeTexture = std::make_unique<VsgMyGui::Texture>("__openmw_video_rgba8");
            if (!updateVulkanVideoTexture(*mPlayer, *nativeTexture))
            {
                Log(Debug::Error) << "V4 Vulkan video bridge could not publish the first decoded frame for '" << video
                                  << "'";
                return;
            }
            mTexture = std::move(nativeTexture);
            Log(Debug::Info) << "V4 Vulkan video bridge: publishing decoded RGBA8 frames for '" << video << "'";
        }
        else
#endif
        {
            mTexture = std::make_unique<MyGUIPlatform::OSGTexture>(texture);
        }

        setRenderItemTexture(mTexture.get());
        // Both the widget and the video frame are Y-down, so this UV is not inverted
        getSubWidgetMain()->_setUVSet(MyGUI::FloatRect(0.f, 0.f, 1.f, 1.f));
    }

    int VideoWidget::getVideoWidth()
    {
        return mPlayer->getVideoWidth();
    }

    int VideoWidget::getVideoHeight()
    {
        return mPlayer->getVideoHeight();
    }

    bool VideoWidget::update()
    {
        return mPlayer->update();
    }

    void VideoWidget::commitFrame()
    {
        mPlayer->commitFrame();
#if defined(OPENMW_ENABLE_V4_VULKAN_RUNTIME)
        if (auto* nativeTexture = dynamic_cast<VsgMyGui::Texture*>(mTexture.get()))
        {
            if (!updateVulkanVideoTexture(*mPlayer, *nativeTexture))
                Log(Debug::Warning) << "V4 Vulkan video bridge could not publish a committed decoded frame";
        }
#endif
    }

    void VideoWidget::stop()
    {
        mPlayer->close();
    }

    void VideoWidget::pause()
    {
        mPlayer->pause();
    }

    void VideoWidget::resume()
    {
        mPlayer->play();
    }

    bool VideoWidget::isPaused() const
    {
        return mPlayer->isPaused();
    }

    bool VideoWidget::hasAudioStream()
    {
        return mPlayer->hasAudioStream();
    }

    void VideoWidget::autoResize(bool stretch)
    {
        MyGUI::IntSize screenSize = MyGUI::RenderManager::getInstance().getViewSize();
        if (getParent())
            screenSize = getParent()->getSize();

        if (getVideoHeight() > 0 && !stretch)
        {
            double imageaspect = static_cast<double>(getVideoWidth()) / getVideoHeight();

            int leftPadding = std::max(0, static_cast<int>(screenSize.width - screenSize.height * imageaspect) / 2);
            int topPadding = std::max(0, static_cast<int>(screenSize.height - screenSize.width / imageaspect) / 2);

            setCoord(leftPadding, topPadding, screenSize.width - leftPadding * 2, screenSize.height - topPadding * 2);
        }
        else
            setCoord(0, 0, screenSize.width, screenSize.height);
    }

}
