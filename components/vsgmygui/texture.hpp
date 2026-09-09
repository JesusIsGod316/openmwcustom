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

    // MyGUI texture backed by a VSG image. MyGUI creates these two ways:
    //   * createManual + lock/unlock — a CPU-generated atlas (this is how fonts are rasterised, and how MyGUI
    //     builds dynamic images). We keep a CPU staging buffer during the lock and, on unlock, EXPAND whatever
    //     MyGUI pixel format it used (L8 / L8A8 / R8G8B8 / R8G8B8A8) into a single RGBA8 vsg image. The expansion
    //     matters: OpenGL sampled luminance formats as (L,L,L,A); Vulkan has no such replication, so we bake it in
    //     here and keep one simple RGBA UI shader.
    //   * loadFromFile — a skin texture on disk. This backend (used by the standalone VSG path, which has no
    //     Resource::ImageManager) does not yet decode arbitrary image files; that is wired in P1.2. Until then it
    //     produces a visible magenta placeholder rather than throwing, so a missing decoder can't abort GUI setup.
    // NOT final: the local map subclasses this to own its own textures (they are not created through the render
    // manager's by-name map, so they have to evict their cached descriptor themselves — see forgetTexture).
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

        // Internal: the current RGBA8 image (may be null until first unlock/load). Rebuilt on each unlock, so the
        // overlay node captured this frame keeps its own ref while a later frame swaps in a fresh image.
        vsg::ref_ptr<vsg::Data> data() const { return mData; }
        std::uint64_t identity() const noexcept { return mIdentity; }
        std::uint64_t revision() const noexcept { return mRevision; }

        // Internal: adopt an RGBA8 image the HOST owns and keeps writing to (the local map's rendered cell, and
        // its fog-of-war layer). The point is that the host can keep the SAME vsg::Data and dirty() it — the
        // descriptor the render manager cached for this texture stays valid, which an unlock()-style rebuild
        // (a fresh image every time) would not. Does not copy.
        void setData(vsg::ref_ptr<vsg::Data> data);

    private:
        std::string mName;
        int mWidth = 0;
        int mHeight = 0;
        MyGUI::PixelFormat mFormat = MyGUI::PixelFormat::Unknow;
        MyGUI::TextureUsage mUsage = MyGUI::TextureUsage::Default;
        size_t mNumElemBytes = 0;
        bool mLocked = false;
        std::vector<uint8_t> mLockBuffer; // CPU staging held between lock() and unlock()
        vsg::ref_ptr<vsg::Data> mData; // RGBA8 (ubvec4Array2D) uploaded to the GPU by the overlay compile
        ImageDecoder mDecoder; // host image-file decoder for loadFromFile (may be empty)
        std::uint64_t mIdentity = 0;
        std::uint64_t mRevision = 0;
    };
}

#endif
