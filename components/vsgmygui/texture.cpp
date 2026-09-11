#include "texture.hpp"

#include <atomic>
#include <stdexcept>

#include <components/debug/debuglog.hpp>

namespace VsgMyGui
{
    namespace
    {
        std::atomic_uint64_t sNextTextureIdentity{ 1 };

        size_t elemBytes(MyGUI::PixelFormat format)
        {
            switch (format.getValue())
            {
                case MyGUI::PixelFormat::L8:
                    return 1;
                case MyGUI::PixelFormat::L8A8:
                    return 2;
                case MyGUI::PixelFormat::R8G8B8:
                    return 3;
                case MyGUI::PixelFormat::R8G8B8A8:
                    return 4;
                default:
                    return 0;
            }
        }
    }

    Texture::Texture(const std::string& name)
        : mName(name)
        , mIdentity(sNextTextureIdentity.fetch_add(1, std::memory_order_relaxed))
    {
    }

    Texture::~Texture() = default;

    void Texture::createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format)
    {
        const size_t nb = elemBytes(format);
        if (nb == 0)
            throw std::runtime_error("VsgMyGui::Texture: unsupported pixel format");
        if (width <= 0 || height <= 0)
            throw std::runtime_error("VsgMyGui::Texture: invalid manual texture extent");
        mWidth = width;
        mHeight = height;
        mFormat = format;
        mUsage = usage;
        mNumElemBytes = nb;
        mData = {};
        mImageView = {};
        mLockBuffer.clear();
        mLocked = false;
        ++mRevision;
    }

    void Texture::destroy()
    {
        mData = {};
        mImageView = {};
        mLockBuffer.clear();
        mLocked = false;
        mFormat = MyGUI::PixelFormat::Unknow;
        mUsage = MyGUI::TextureUsage::Default;
        mNumElemBytes = 0;
        mWidth = mHeight = 0;
        ++mRevision;
    }

    void* Texture::lock(MyGUI::TextureUsage /*access*/)
    {
        if (mImageView)
            throw std::runtime_error("VsgMyGui::Texture: external render-target images are read-only to MyGUI");
        if (mWidth <= 0 || mHeight <= 0 || mNumElemBytes == 0)
            throw std::runtime_error("VsgMyGui::Texture: lock() before createManual()");
        if (mLocked)
            throw std::runtime_error("VsgMyGui::Texture: already locked");
        mLockBuffer.assign(static_cast<size_t>(mWidth) * mHeight * mNumElemBytes, 0);
        mLocked = true;
        return mLockBuffer.data();
    }

    void Texture::unlock()
    {
        if (!mLocked)
            throw std::runtime_error("VsgMyGui::Texture: unlock() without lock()");

        auto rgba = vsg::ubvec4Array2D::create(mWidth, mHeight, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        const uint8_t* src = mLockBuffer.data();
        for (int y = 0; y < mHeight; ++y)
        {
            for (int x = 0; x < mWidth; ++x)
            {
                const uint8_t* p = src + (static_cast<size_t>(y) * mWidth + x) * mNumElemBytes;
                vsg::ubvec4 out(255, 255, 255, 255);
                switch (mNumElemBytes)
                {
                    case 1:
                        out = vsg::ubvec4(p[0], p[0], p[0], 255);
                        break;
                    case 2:
                        out = vsg::ubvec4(p[0], p[0], p[0], p[1]);
                        break;
                    case 3:
                        out = vsg::ubvec4(p[0], p[1], p[2], 255);
                        break;
                    default:
                        out = vsg::ubvec4(p[0], p[1], p[2], p[3]);
                        break;
                }
                (*rgba)(x, y) = out;
            }
        }
        mData = rgba;
        mImageView = {};
        ++mRevision;
        mLockBuffer.clear();
        mLocked = false;
    }

    void Texture::setData(vsg::ref_ptr<vsg::Data> data)
    {
        mData = std::move(data);
        mImageView = {};
        ++mRevision;
        mUsage = MyGUI::TextureUsage::Static;
        mFormat = MyGUI::PixelFormat::R8G8B8A8;
        mNumElemBytes = 4;
        mWidth = mData ? static_cast<int>(mData->width()) : 0;
        mHeight = mData ? static_cast<int>(mData->height()) : 0;
        mLockBuffer.clear();
        mLocked = false;
    }

    void Texture::setImageView(
        vsg::ref_ptr<vsg::ImageView> imageView, int width, int height, MyGUI::PixelFormat format)
    {
        if (!imageView || width <= 0 || height <= 0)
            throw std::runtime_error("VsgMyGui::Texture: invalid external Vulkan image");
        const size_t nb = elemBytes(format);
        if (nb == 0)
            throw std::runtime_error("VsgMyGui::Texture: unsupported external image pixel format");
        mData = {};
        mImageView = std::move(imageView);
        mWidth = width;
        mHeight = height;
        mFormat = format;
        mUsage = MyGUI::TextureUsage::Static;
        mNumElemBytes = nb;
        mLockBuffer.clear();
        mLocked = false;
        ++mRevision;
    }

    void Texture::loadFromFile(const std::string& fname)
    {
        mUsage = MyGUI::TextureUsage::Static;
        mFormat = MyGUI::PixelFormat::R8G8B8A8;
        mNumElemBytes = 4;
        mImageView = {};

        vsg::ref_ptr<vsg::Data> decoded = mDecoder ? mDecoder(fname) : vsg::ref_ptr<vsg::Data>{};
        if (decoded)
        {
            mData = decoded;
            ++mRevision;
            mWidth = static_cast<int>(decoded->width());
            mHeight = static_cast<int>(decoded->height());
            return;
        }

        Log(Debug::Warning) << "VsgMyGui::Texture: could not decode '" << fname << "' — using placeholder";
        mWidth = mHeight = 2;
        auto rgba = vsg::ubvec4Array2D::create(2, 2, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                (*rgba)(x, y) = vsg::ubvec4(255, 0, 255, 255);
        mData = rgba;
        ++mRevision;
    }

    void Texture::saveToFile(const std::string& fname)
    {
        Log(Debug::Warning) << "VsgMyGui::Texture: saveToFile('" << fname << "') is not implemented";
    }

    void Texture::setShader(const std::string& /*shaderName*/)
    {
        Log(Debug::Warning) << "VsgMyGui::Texture: setShader is not implemented";
    }
}