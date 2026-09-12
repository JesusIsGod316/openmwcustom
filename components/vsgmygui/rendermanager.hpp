#ifndef OPENMW_COMPONENTS_VSGMYGUI_RENDERMANAGER_H
#define OPENMW_COMPONENTS_VSGMYGUI_RENDERMANAGER_H

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <MyGUI_RenderManager.h>

#include <vsg/all.h>

#include <components/render/backend/vsg/uipipeline.hpp>

#include "texture.hpp"

namespace VsgMyGui
{
    class RenderManager final : public MyGUI::RenderManager, public MyGUI::IRenderTarget
    {
    public:
        explicit RenderManager(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder = {});
        ~RenderManager() override;

        void initialise(int viewW, int viewH);
        void shutdown();
        void setViewSizePixels(int width, int height);

        vsg::ref_ptr<vsg::Node> collectDrawCalls();
        vsg::ref_ptr<vsg::Node> buildOverlayNode() const;
        std::size_t batchCount() const { return mBatches.size(); }

        void collect();
        vsg::ref_ptr<vsg::Node> buildPersistentOverlay();
        void updatePersistentOverlay();
        bool overlayStructureChanged() const;

        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;
        MyGUI::IVertexBuffer* createVertexBuffer() override;
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;
        MyGUI::ITexture* createTexture(const std::string& name) override;
        void destroyTexture(MyGUI::ITexture* texture) override;
        MyGUI::ITexture* getTexture(const std::string& name) override;
        bool checkTexture(MyGUI::ITexture* texture) override;

        // Publish or replace a named, sampled VSG image as a native MyGUI texture. The render target stays owned by
        // the VSG runtime; collected GUI batches retain a strong ImageView reference instead of a raw Texture pointer.
        Texture* setExternalTexture(const std::string& name, vsg::ref_ptr<vsg::ImageView> imageView,
            int width, int height, MyGUI::PixelFormat format = MyGUI::PixelFormat::R8G8B8A8);

        // Publish CPU-authored RGBA8 content without passing an OSG texture across
        // the VSG/MyGUI boundary. Used by persistent overlays such as local-map
        // fog-of-war whose authoritative state remains CPU/save-game owned.
        Texture* setRgba8Texture(const std::string& name, std::span<const std::uint8_t> rgba, int width, int height);

        // Remove a named native texture. Already-collected overlay graphs own
        // strong VSG backing references and remain valid until frame-safe retirement.
        bool removeTexture(const std::string& name) noexcept;

        void setViewSize(int width, int height) override;
        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        void begin() override;
        void end() override;
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;
        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

    private:
        struct TextureSnapshot
        {
            vsg::ref_ptr<vsg::Data> data;
            vsg::ref_ptr<vsg::ImageView> imageView;
            std::uint64_t identity = 0;
            std::uint64_t revision = 0;
        };

        struct Batch
        {
            vsg::ref_ptr<vsg::ubyteArray> vertices;
            TextureSnapshot texture;
            uint32_t count = 0;
        };

        struct Slot
        {
            vsg::ref_ptr<vsg::ubyteArray> verts;
            vsg::ref_ptr<vsg::Draw> draw;
            std::uint64_t textureIdentity = 0;
            std::uint64_t textureRevision = 0;
            std::uint32_t capacity = 0;
        };

        RenderVsg::UiPipeline mPipeline;
        ImageDecoder mDecoder;
        std::vector<Slot> mSlots;
        bool mWarnedStructure = false;
        bool mWarnedForeignTexture = false;
        MyGUI::IntSize mViewSize;
        MyGUI::RenderTargetInfo mInfo;
        MyGUI::VertexColourType mVertexFormat;
        bool mIsInitialise = false;
        bool mUpdate = false;
        // MyGUI RenderItem stores a raw IVertexBuffer*. Loading-screen GUI-only presents can overlap
        // transitional layer ownership, so keep every backend buffer alive until the RenderManager itself dies.
        // destroyVertexBuffer() only retires MyGUI's logical ownership; physical storage is released here later.
        std::vector<std::unique_ptr<MyGUI::IVertexBuffer>> mVertexBuffers;
        std::map<std::string, Texture> mTextures;
        std::vector<Batch> mBatches;
        vsg::ref_ptr<vsg::BindDescriptorSet> bindDescriptorSetFor(const TextureSnapshot& texture);
        static TextureSnapshot snapshotTexture(const Texture* texture);
    };

    vsg::ref_ptr<vsg::Node> buildSelfTest(RenderManager& rm);
}

#endif