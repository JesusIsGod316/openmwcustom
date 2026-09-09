#ifndef OPENMW_COMPONENTS_VSGMYGUI_RENDERMANAGER_H
#define OPENMW_COMPONENTS_VSGMYGUI_RENDERMANAGER_H

#include <map>
#include <string>

#include <MyGUI_RenderManager.h>

#include <vsg/all.h>

#include <components/render/backend/vsg/uipipeline.hpp>

#include "texture.hpp"

namespace VsgMyGui
{

    // MyGUI render backend for the VSG path (MW8 P1). It implements MyGUI::RenderManager + MyGUI::IRenderTarget so
    // MyGUI's own machinery drives it, and turns the per-frame draw batches MyGUI emits (via begin/doRender/end)
    // into a VSG scene node built on the UI pipeline (RenderVsg::UiPipeline). Each batch = one texture + one run of
    // MyGUI::Vertex; we copy the vertices and reference the texture's RGBA image, then buildOverlayNode() wraps each
    // batch in a StateGroup (bind pipeline, bind the texture descriptor, bind the interleaved vertex buffer, draw).
    //
    // The node is rebuilt from scratch each time it is collected (correctness-first; the batch/texture set changes
    // frame to frame). That per-build compile is the known cost to optimise later (persistent host-visible buffers
    // + dynamic descriptor binding, the vsgImGui pattern). collectDrawCalls() runs one begin/onRenderToTarget/end
    // and returns the fresh node.
    class RenderManager final : public MyGUI::RenderManager, public MyGUI::IRenderTarget
    {
    public:
        // decoder is used by textures for MyGUI::ITexture::loadFromFile (skin textures). May be empty (self-test).
        explicit RenderManager(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder = {});
        ~RenderManager() override;

        // Called by the Platform before MyGUI::Gui::initialise(): records the view size. (No OSG drawable/camera to
        // set up as the OSG backend does — the overlay is a plain node built on demand.)
        void initialise(int viewW, int viewH);
        void shutdown();

        void setViewSizePixels(int width, int height);

        // Run one MyGUI render pass (begin → onRenderToTarget → end) and return the resulting overlay node, or a
        // null ref if nothing was drawn. Requires MyGUI::Gui to be live; the P1.1 self-test bypasses this and calls
        // begin/doRender/end directly instead.
        vsg::ref_ptr<vsg::Node> collectDrawCalls();

        // Assemble the batches gathered since the last begin() into a VSG node (null if there were none).
        vsg::ref_ptr<vsg::Node> buildOverlayNode() const;

        // Batches collected in the last collect()/collectDrawCalls() (diagnostic).
        std::size_t batchCount() const { return mBatches.size(); }

        // --- live drive: a PERSISTENT overlay refreshed in place, with a rebuild on structural change ---
        // The per-frame-recompile path doesn't render under goVulkan's shadow+main two-pass graph (a node compiled
        // after finalize() isn't slotted into the main pass). Instead: collect() once, buildPersistentOverlay(), add
        // the node BEFORE finalize() (so it compiles in the main pass), then each frame collect() + either the cheap
        // in-place updatePersistentOverlay() (stable structure) or — when overlayStructureChanged() reports the batch
        // set has changed (a window opened/closed, a new skin/atlas appeared, as the real dynamic GUI does) — a
        // host-driven rebuild (buildPersistentOverlay again) + recompile + swap. Vertex
        // buffers are DYNAMIC_DATA (same mechanism as the animated water) so dirty() re-uploads them without a
        // recompile on the stable frames.
        void collect(); // run one MyGUI render pass, filling the batch list (no node build)
        vsg::ref_ptr<vsg::Node> buildPersistentOverlay();
        void updatePersistentOverlay();
        // True when the live batch structure (count, or any batch's texture identity) has diverged from the slots the
        // current overlay node was built for — the signal for the host to rebuild + recompile + swap the overlay
        // rather than refresh it in place (which would leave the changed widgets stale/frozen).
        bool overlayStructureChanged() const;

        // --- MyGUI::RenderManager ---
        const MyGUI::IntSize& getViewSize() const override { return mViewSize; }
        MyGUI::VertexColourType getVertexFormat() const override { return mVertexFormat; }
        bool isFormatSupported(MyGUI::PixelFormat format, MyGUI::TextureUsage usage) override;
        MyGUI::IVertexBuffer* createVertexBuffer() override;
        void destroyVertexBuffer(MyGUI::IVertexBuffer* buffer) override;
        MyGUI::ITexture* createTexture(const std::string& name) override;
        void destroyTexture(MyGUI::ITexture* texture) override;
        MyGUI::ITexture* getTexture(const std::string& name) override;
        bool checkTexture(MyGUI::ITexture* texture) override;

        // Drop the cached descriptor set for a texture this manager does NOT own. Textures created through
        // createTexture() live in mTextures and are evicted automatically; a Texture the host allocated itself
        // (the local map hands MyGUI widgets textures it owns) has to say so before it is destroyed, or the
        // pointer-keyed cache would hand a later Texture allocated at the same address the old image.
        void forgetTexture(const Texture* texture);
        void setViewSize(int width, int height) override;
        void registerShader(const std::string& shaderName, const std::string& vertexProgramFile,
            const std::string& fragmentProgramFile) override;

        // --- MyGUI::IRenderTarget ---
        void begin() override;
        void end() override;
        void doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count) override;
        const MyGUI::RenderTargetInfo& getInfo() const override { return mInfo; }

    private:
        struct Batch
        {
            vsg::ref_ptr<vsg::ubyteArray> vertices; // interleaved MyGUI::Vertex, copied at doRender time
            Texture* texture = nullptr; // owned by mTextures; RGBA image lives in Texture::data()
            uint32_t count = 0; // vertex count
        };

        // One persistent draw slot per batch: a DYNAMIC vertex buffer + a Draw whose count is set each frame, and
        // the texture its descriptor was built for (to detect a structural change).
        struct Slot
        {
            vsg::ref_ptr<vsg::ubyteArray> verts;
            vsg::ref_ptr<vsg::Draw> draw;
            const Texture* texture = nullptr;
            std::uint64_t textureIdentity = 0;
            std::uint64_t textureRevision = 0;
        };

        RenderVsg::UiPipeline mPipeline;
        ImageDecoder mDecoder;
        std::vector<Slot> mSlots;
        bool mWarnedStructure = false; // one-shot log when the live batch structure diverges from the slots
        MyGUI::IntSize mViewSize;
        MyGUI::RenderTargetInfo mInfo;
        MyGUI::VertexColourType mVertexFormat;
        bool mIsInitialise = false;
        // Stored BY VALUE (like the OSG backend): the map owns each Texture, so there is exactly one destruction
        // (no manual delete → no double-free with MyGUI's own texture teardown). std::map node addresses are
        // stable, so the ITexture*/Texture* MyGUI holds (and Batch::texture) stay valid until that key is erased.
        std::map<std::string, Texture> mTextures;
        std::vector<Batch> mBatches;
        // Leak fix: the game HUD reorders its batches most frames, forcing a full overlay rebuild. That rebuild must
        // NOT allocate fresh GPU resources each time — doing so exhausted device memory (VK_ERROR_DEVICE_LOST after a
        // few minutes). So cache the compiled descriptor set per texture, and pool the DYNAMIC vertex buffers by slot
        // index; buildPersistentOverlay reuses them, making compileManager->compile() a no-op. The descriptor cache is
        // keyed by texture identity and evicted when that texture is (re)created or destroyed.
        using TextureKey = std::pair<std::uint64_t, std::uint64_t>;
        std::map<TextureKey, vsg::ref_ptr<vsg::BindDescriptorSet>> mDsCache;
        std::vector<vsg::ref_ptr<vsg::ubyteArray>> mVertPool;
        vsg::ref_ptr<vsg::BindDescriptorSet> bindDescriptorSetFor(const Texture* texture);
        static TextureKey textureKey(const Texture* texture) noexcept;
    };

    // P1.1 self-test: drive the backend WITHOUT MyGUI::Gui — createManual+lock/unlock a generated texture (the
    // font upload path) and lock/write a MyGUI::Vertex quad, then doRender + buildOverlayNode. Returns the overlay
    // node so goVulkan can compile + display it, proving the whole backend on MoltenVK before MyGUI init (P1.2).
    vsg::ref_ptr<vsg::Node> buildSelfTest(RenderManager& rm);
}

#endif
