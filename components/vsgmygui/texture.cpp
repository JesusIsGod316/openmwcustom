#include "texture.hpp"

#include <stdexcept>
#include <atomic>

#include <components/debug/debuglog.hpp>

namespace VsgMyGui
{
    namespace
    {
        std::atomic_uint64_t sNextTextureIdentity{ 1 };

        // Bytes per pixel MyGUI hands us for each source format.
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
        mWidth = width;
        mHeight = height;
        mFormat = format;
        mUsage = usage;
        mNumElemBytes = nb;
        mData = {}; // filled on unlock()
        ++mRevision;
    }

    void Texture::destroy()
    {
        mData = {};
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

        // Expand the source pixels into RGBA8. GL sampled L8/L8A8 as (L,L,L,1)/(L,L,L,A); we bake that here.
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
                    case 1: // L8
                        out = vsg::ubvec4(p[0], p[0], p[0], 255);
                        break;
                    case 2: // L8A8
                        out = vsg::ubvec4(p[0], p[0], p[0], p[1]);
                        break;
                    case 3: // R8G8B8
                        out = vsg::ubvec4(p[0], p[1], p[2], 255);
                        break;
                    default: // R8G8B8A8
                        out = vsg::ubvec4(p[0], p[1], p[2], p[3]);
                        break;
                }
                (*rgba)(x, y) = out;
            }
        }
        mData = rgba;
        ++mRevision;
        mLockBuffer.clear();
        mLocked = false;
    }

    void Texture::setData(vsg::ref_ptr<vsg::Data> data)
    {
        mData = data;
        ++mRevision;
        mUsage = MyGUI::TextureUsage::Static;
        mFormat = MyGUI::PixelFormat::R8G8B8A8;
        mNumElemBytes = 4;
        mWidth = data ? static_cast<int>(data->width()) : 0;
        mHeight = data ? static_cast<int>(data->height()) : 0;
    }

    void Texture::loadFromFile(const std::string& fname)
    {
        mUsage = MyGUI::TextureUsage::Static;
        mFormat = MyGUI::PixelFormat::R8G8B8A8;
        mNumElemBytes = 4;

        vsg::ref_ptr<vsg::Data> decoded = mDecoder ? mDecoder(fname) : vsg::ref_ptr<vsg::Data>{};
        if (decoded)
        {
            mData = decoded;
            ++mRevision;
            mWidth = static_cast<int>(decoded->width());
            mHeight = static_cast<int>(decoded->height());
            return;
        }

        // No decoder, or the file couldn't be decoded: a 2x2 magenta placeholder so the widget is visibly
        // "missing texture" rather than aborting GUI setup.
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
