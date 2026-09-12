#include "rendermanager.hpp"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <MyGUI_VertexData.h>

#include <components/debug/debuglog.hpp>

#include "texture.hpp"

namespace VsgMyGui
{
    namespace
    {
        class VertexBuffer final : public MyGUI::IVertexBuffer
        {
        public:
            void setVertexCount(size_t count) override { mNeedCount = count; }
            size_t getVertexCount() const override { return mNeedCount; }

            MyGUI::Vertex* lock() override
            {
                mData.resize(mNeedCount * sizeof(MyGUI::Vertex));
                return reinterpret_cast<MyGUI::Vertex*>(mData.data());
            }
            void unlock() override {}

            const uint8_t* bytes() const { return mData.data(); }
            size_t byteSize() const { return mData.size(); }

        private:
            std::vector<uint8_t> mData;
            size_t mNeedCount = 0;
        };

        vsg::ref_ptr<vsg::ImageInfo> imageInfoFor(const RenderVsg::UiPipeline& pipeline,
            const vsg::ref_ptr<vsg::Data>& data, const vsg::ref_ptr<vsg::ImageView>& imageView)
        {
            if (imageView)
                return vsg::ImageInfo::create(pipeline.sampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return vsg::ImageInfo::create(
                pipeline.sampler, data ? data : pipeline.whiteTexture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    RenderManager::RenderManager(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder)
        : mPipeline(pipeline)
        , mDecoder(std::move(decoder))
        , mVertexFormat(MyGUI::VertexColourType::ColourABGR)
    {
        mViewSize.set(0, 0);
        mInfo.maximumDepth = 1;
    }

    RenderManager::~RenderManager() = default;

    void RenderManager::initialise(int viewW, int viewH)
    {
        setViewSizePixels(viewW, viewH);
        // Match the established OpenGL and upstream Vulkan MyGUI backends: individual RenderItems start
        // out-of-date and materialize themselves on first use. A full forced update is reserved for resize.
        mUpdate = false;
        mIsInitialise = true;
    }

    void RenderManager::shutdown()
    {
        mSlots.clear();
        mTextures.clear();
        mBatches.clear();
        // Deliberately retain mVertexBuffers until RenderManager destruction. MyGUI RenderItem keeps raw
        // IVertexBuffer pointers and may tear layer ownership down during transitional/loading-screen presents.
        mUpdate = false;
        mIsInitialise = false;
    }

    void RenderManager::setViewSizePixels(int width, int height)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);
        mViewSize.set(width, height);
        mInfo.maximumDepth = 1;
        mInfo.hOffset = 0;
        mInfo.vOffset = 0;
        mInfo.aspectCoef = float(mViewSize.height) / float(mViewSize.width);
        mInfo.pixScaleX = 1.0f / float(mViewSize.width);
        mInfo.pixScaleY = 1.0f / float(mViewSize.height);
    }

    void RenderManager::setViewSize(int width, int height)
    {
        setViewSizePixels(width, height);
        onResizeView(mViewSize);
        mUpdate = true;
    }

    bool RenderManager::isFormatSupported(MyGUI::PixelFormat /*format*/, MyGUI::TextureUsage /*usage*/)
    {
        return true;
    }

    MyGUI::IVertexBuffer* RenderManager::createVertexBuffer()
    {
        auto buffer = std::make_unique<VertexBuffer>();
        MyGUI::IVertexBuffer* const result = buffer.get();
        mVertexBuffers.push_back(std::move(buffer));
        return result;
    }

    void RenderManager::destroyVertexBuffer(MyGUI::IVertexBuffer* buffer)
    {
        if (!buffer)
            return;

        // MyGUI owns RenderItem through raw layer pointers and calls this hook from RenderItem destruction.
        // Do not immediately free the backend object: synchronous loading-screen GUI-only presents can traverse
        // a transitional layer snapshot. Keeping the small CPU staging buffers alive until RenderManager teardown
        // makes those raw references lifetime-safe without extending any VSG/GPU resource lifetime.
        const auto owned = std::find_if(mVertexBuffers.begin(), mVertexBuffers.end(),
            [buffer](const std::unique_ptr<MyGUI::IVertexBuffer>& candidate) { return candidate.get() == buffer; });
        if (owned == mVertexBuffers.end())
            Log(Debug::Warning) << "VsgMyGui: ignoring destruction request for an unowned vertex buffer";
    }

    MyGUI::ITexture* RenderManager::createTexture(const std::string& name)
    {
        auto [it, inserted] = mTextures.insert_or_assign(name, Texture(name));
        it->second.setDecoder(mDecoder);
        (void)inserted;
        return &it->second;
    }

    Texture* RenderManager::setExternalTexture(const std::string& name, vsg::ref_ptr<vsg::ImageView> imageView,
        int width, int height, MyGUI::PixelFormat format)
    {
        if (name.empty())
            return nullptr;
        auto it = mTextures.find(name);
        if (it == mTextures.end())
        {
            auto [created, inserted] = mTextures.emplace(name, Texture(name));
            (void)inserted;
            created->second.setDecoder(mDecoder);
            it = created;
        }
        it->second.setImageView(std::move(imageView), width, height, format);
        return &it->second;
    }

    void RenderManager::destroyTexture(MyGUI::ITexture* texture)
    {
        if (!texture)
            return;
        mTextures.erase(texture->getName());
    }

    MyGUI::ITexture* RenderManager::getTexture(const std::string& name)
    {
        if (name.empty())
            return nullptr;
        if (const auto it = mTextures.find(name); it != mTextures.end())
            return &it->second;
        MyGUI::ITexture* tex = createTexture(name);
        tex->loadFromFile(name);
        return tex;
    }

    bool RenderManager::checkTexture(MyGUI::ITexture* texture)
    {
        if (!texture || dynamic_cast<Texture*>(texture))
            return true;
        const std::string& name = texture->getName();
        return !name.empty() && mTextures.contains(name);
    }

    void RenderManager::registerShader(
        const std::string& /*shaderName*/, const std::string& /*vertex*/, const std::string& /*fragment*/)
    {
        Log(Debug::Warning) << "VsgMyGui::RenderManager::registerShader is not implemented";
    }

    void RenderManager::begin()
    {
        mBatches.clear();
    }

    void RenderManager::end() {}

    RenderManager::TextureSnapshot RenderManager::snapshotTexture(const Texture* texture)
    {
        TextureSnapshot result;
        if (!texture)
            return result;
        result.data = texture->data();
        result.imageView = texture->imageView();
        result.identity = texture->identity();
        result.revision = texture->revision();
        return result;
    }

    void RenderManager::doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count)
    {
        if (!buffer || count == 0)
            return;
        auto* vb = static_cast<VertexBuffer*>(buffer);
        const size_t bytes = count * sizeof(MyGUI::Vertex);
        if (vb->byteSize() < bytes)
            return;

        const Texture* nativeTexture = dynamic_cast<Texture*>(texture);
        if (texture && !nativeTexture)
        {
            const std::string& alias = texture->getName();
            if (!alias.empty())
            {
                if (auto native = mTextures.find(alias); native != mTextures.end())
                    nativeTexture = &native->second;
            }
            if (!nativeTexture)
            {
                if (!mWarnedForeignTexture)
                {
                    Log(Debug::Warning) << "VsgMyGui: skipping unresolved foreign render-target texture '"
                                        << alias << "'; native auxiliary surfaces must be published before an alias is rendered";
                    mWarnedForeignTexture = true;
                }
                return;
            }
        }

        Batch batch;
        batch.vertices = vsg::ubyteArray::create(bytes);
        std::memcpy(batch.vertices->dataPointer(), vb->bytes(), bytes);
        batch.texture = snapshotTexture(nativeTexture);
        batch.count = static_cast<uint32_t>(count);
        mBatches.push_back(std::move(batch));
    }

    vsg::ref_ptr<vsg::Node> RenderManager::buildOverlayNode() const
    {
        if (mBatches.empty() || !mPipeline)
            return {};

        auto root = vsg::Group::create();
        for (const Batch& batch : mBatches)
        {
            auto info = imageInfoFor(mPipeline, batch.texture.data, batch.texture.imageView);
            auto image = vsg::DescriptorImage::create(info, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            auto ds = vsg::DescriptorSet::create(mPipeline.descriptorSetLayout, vsg::Descriptors{ image });
            auto bindDs
                = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline.pipelineLayout, 0, ds);
            auto sg = vsg::StateGroup::create();
            sg->add(mPipeline.bindPipeline);
            sg->add(bindDs);
            sg->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ batch.vertices }));
            sg->addChild(vsg::Draw::create(batch.count, 1, 0, 0));
            root->addChild(sg);
        }
        return root;
    }

    void RenderManager::collect()
    {
        begin();
        onRenderToTarget(this, mUpdate);
        end();
        mUpdate = false;
    }

    vsg::ref_ptr<vsg::Node> RenderManager::collectDrawCalls()
    {
        collect();
        return buildOverlayNode();
    }

    vsg::ref_ptr<vsg::Node> RenderManager::buildPersistentOverlay()
    {
        // Hardware crash evidence from CP4F Run20 showed the mutable sidecar slot path retiring a corrupted
        // vsg::Draw ref_ptr while the previous GUI generation was still in the publication/retirement pipeline.
        // Until a proper frame-indexed dynamic-buffer ring is introduced, build an immutable graph from the
        // freshly collected batches and let VsgRuntimeHost retire the previously published root by completed frame.
        // This is correctness-first: it intentionally gives up the in-place UI update optimization.
        return buildOverlayNode();
    }

    vsg::ref_ptr<vsg::BindDescriptorSet> RenderManager::bindDescriptorSetFor(const TextureSnapshot& texture)
    {
        // Each rebuilt overlay owns the exact sampled backing it was collected with.
        // No mutable descriptor cache or raw MyGUI texture pointer crosses overlay lifetimes.
        auto info = imageInfoFor(mPipeline, texture.data, texture.imageView);
        auto image = vsg::DescriptorImage::create(info, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        auto ds = vsg::DescriptorSet::create(mPipeline.descriptorSetLayout, vsg::Descriptors{ image });
        return vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline.pipelineLayout, 0, ds);
    }

    void RenderManager::updatePersistentOverlay()
    {
        // Mutable in-place overlay updates are quarantined. synchronizeGui() is forced through a fresh immutable
        // overlay generation by overlayStructureChanged(), so no CPU-side VSG Draw/Data object is modified while
        // an earlier submission can still reference it.
    }

    bool RenderManager::overlayStructureChanged() const
    {
        // Always publish a fresh immutable GUI generation. This also handles transitions to an empty GUI correctly:
        // the runtime publishes an empty root and retires the previous generation instead of leaving stale UI visible.
        return true;
    }

    vsg::ref_ptr<vsg::Node> buildSelfTest(RenderManager& rm)
    {
        constexpr int ts = 64;
        auto* tex = static_cast<Texture*>(rm.createTexture("__vsgmygui_selftest"));
        tex->createManual(ts, ts, MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);
        auto* px = static_cast<uint8_t*>(tex->lock(MyGUI::TextureUsage::Static));
        for (int y = 0; y < ts; ++y)
        {
            for (int x = 0; x < ts; ++x)
            {
                const bool c = (((x / 8) + (y / 8)) & 1) != 0;
                uint8_t* p = px + (static_cast<size_t>(y) * ts + x) * 4;
                p[0] = c ? 245 : 25;
                p[1] = c ? 205 : 25;
                p[2] = c ? 45 : 130;
                p[3] = 255;
            }
        }
        tex->unlock();

        auto* buffer = rm.createVertexBuffer();
        buffer->setVertexCount(6);
        MyGUI::Vertex* v = buffer->lock();
        const uint32_t white = 0xffffffffu;
        const auto set = [](MyGUI::Vertex& vert, float x, float y, uint32_t col, float u, float w) {
            vert.x = x;
            vert.y = y;
            vert.z = 0.f;
            vert.colour = col;
            vert.u = u;
            vert.v = w;
        };
        set(v[0], 0.30f, 0.30f, white, 0.f, 0.f);
        set(v[1], 0.85f, 0.30f, white, 1.f, 0.f);
        set(v[2], 0.85f, 0.85f, white, 1.f, 1.f);
        set(v[3], 0.30f, 0.30f, white, 0.f, 0.f);
        set(v[4], 0.85f, 0.85f, white, 1.f, 1.f);
        set(v[5], 0.30f, 0.85f, white, 0.f, 1.f);
        buffer->unlock();

        rm.begin();
        rm.doRender(buffer, tex, 6);
        rm.end();
        auto node = rm.buildPersistentOverlay();
        rm.destroyVertexBuffer(buffer);
        return node;
    }
}
