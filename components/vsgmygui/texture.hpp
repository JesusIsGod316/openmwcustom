#ifndef OPENMW_COMPONENTS_VSGMYGUI_TEXTURE_H
#define OPENMW_COMPONENTS_VSGMYGUI_TEXTURE_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <MyGUI_ITexture.h>

#include <vsg/all.h>

namespace VsgMyGui
{
    // Decodes an image file (by VFS path) to an RGBA8 vsg image. Supplied by the host (goVulkan) so this component
    // needs no image/OSG dependency; returns null if the file can't be loaded.
    using ImageDecoder = std::function<vsg::ref_ptr<vsg::Data>(const std::string&)>;

    // MyGUI texture backed by either CPU-authored VSG image data or a backend-owned Vulkan image view. The external
    // image-view path is CP4E's generic bridge for render-to-texture surfaces such as local maps and previews: the
    // render target remains VSG-owned and MyGUI only retains a sampled view of it.
    class Texture : public MyGUI::ITexture
    {
    public:
        explicit Texture(const std::string& name);
        ~Texture() override;

        void setDecoder(ImageDecoder decoder) { mDecoder = std::move(decoder); }

        const std::string& getName() const override { return mName; }

        void createManual(int width, int height, MyGUI::TextureUsage usage, MyGUI::PixelFormat format) override;
        void loadFromFile(const std::string& fname) override;
        void saveToFile(const std::string& fname) override;
        void destroy() override;

        void* lock(MyGUI::TextureUsage access) override;
        void unlock() override;
        bool isLocked() const override { return mLocked; }

        int getWidth() const override { return mWidth; }
        int getHeight() const override { return mHeight; }

        MyGUI::PixelFormat getFormat() const override { return mFormat; }
        MyGUI::TextureUsage getUsage() const override { return mUsage; }
        size_t getNumElemBytes() const override { return mNumElemBytes; }

        MyGUI::IRenderTarget* getRenderTarget() override { return nullptr; }
        void setShader(const std::string& shaderName) override;

        // Internal: exactly one backing path is active at a time. data() is used for ordinary MyGUI images and
        // imageView() for a VSG-owned render target already resident on the GPU.
        vsg::ref_ptr<vsg::Data> data() const { return mData; }
        vsg::ref_ptr<vsg::ImageView> imageView() const { return mImageView; }
        bool externalImage() const noexcept { return static_cast<bool>(mImageView); }
        std::uint64_t identity() const noexcept { return mIdentity; }
        std::uint64_t revision() const noexcept { return mRevision; }

        // Adopt an RGBA8 image the host owns and keeps writing to. Does not copy.
        void setData(vsg::ref_ptr<vsg::Data> data);

        // Adopt a sampled Vulkan image view owned by VSG. The image must remain in
        // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL whenever the MyGUI overlay samples it. This path is deliberately
        // read-only from MyGUI: lock()/unlock() are invalid because the render target is written by a VSG render pass.
        void setImageView(vsg::ref_ptr<vsg::ImageView> imageView, int width, int height,
            MyGUI::PixelFormat format = MyGUI::PixelFormat::R8G8B8A8);

    private:
        std::string mName;
        int mWidth = 0;
        int mHeight = 0;
        MyGUI::PixelFormat mFormat = MyGUI::PixelFormat::Unknow;
        MyGUI::TextureUsage mUsage = MyGUI::TextureUsage::Default;
        size_t mNumElemBytes = 0;
        bool mLocked = false;
        std::vector<uint8_t> mLockBuffer;
        vsg::ref_ptr<vsg::Data> mData;
        vsg::ref_ptr<vsg::ImageView> mImageView;
        ImageDecoder mDecoder;
        std::uint64_t mIdentity = 0;
        std::uint64_t mRevision = 0;
    };
}

#endif