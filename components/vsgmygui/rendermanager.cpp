#include "rendermanager.hpp"

#include <cstring>
#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#include <MyGUI_VertexData.h>

#include <components/debug/debuglog.hpp>

#include "texture.hpp"

namespace VsgMyGui
{
    namespace
    {
        // MyGUI's vertex buffer: a plain CPU run of MyGUI::Vertex that MyGUI fills via lock(). We copy out of it in
        // doRender, so unlock() has nothing to flush.
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
    }

    RenderManager::RenderManager(const RenderVsg::UiPipeline& pipeline, ImageDecoder decoder)
        : mPipeline(pipeline)
        , mDecoder(std::move(decoder))
        , mVertexFormat(MyGUI::VertexColourType::ColourABGR)
    {
        mViewSize.set(0, 0);
        mInfo.maximumDepth = 1;
    }

    void RenderManager::initialise(int viewW, int viewH)
    {
        setViewSizePixels(viewW, viewH);
        mIsInitialise = true;
    }

    void RenderManager::shutdown()
    {
        mTextures.clear();
        mBatches.clear();
        mIsInitialise = false;
    }

    RenderManager::~RenderManager() {} // mTextures owns its Textures by value; nothing to free manually

    void RenderManager::setViewSizePixels(int width, int height)
    {
        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;
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
        onResizeView(mViewSize); // notifies MyGUI's layers (only meaningful once MyGUI::Gui is live — P1.2)
    }

    bool RenderManager::isFormatSupported(MyGUI::PixelFormat /*format*/, MyGUI::TextureUsage /*usage*/)
    {
        return true;
    }

    MyGUI::IVertexBuffer* RenderManager::createVertexBuffer()
    {
        return new VertexBuffer();
    }

    void RenderManager::destroyVertexBuffer(MyGUI::IVertexBuffer* buffer)
    {
        delete buffer;
    }

    MyGUI::ITexture* RenderManager::createTexture(const std::string& name)
    {
        auto [it, inserted] = mTextures.insert_or_assign(name, Texture(name));
        it->second.setDecoder(mDecoder);
        (void)inserted;
        return &it->second;
    }

    void RenderManager::destroyTexture(MyGUI::ITexture* texture)
    {
        if (texture == nullptr)
            return;
        forgetTexture(dynamic_cast<const Texture*>(texture));
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

    bool RenderManager::checkTexture(MyGUI::ITexture* /*texture*/)
    {
        return true;
    }

    void RenderManager::forgetTexture(const Texture* texture)
    {
        if (!texture)
            return;
        const std::uint64_t identity = texture->identity();
        std::erase_if(mDsCache, [identity](const auto& value) { return value.first.first == identity; });
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

    void RenderManager::doRender(MyGUI::IVertexBuffer* buffer, MyGUI::ITexture* texture, size_t count)
    {
        if (buffer == nullptr || count == 0)
            return;
        auto* vb = static_cast<VertexBuffer*>(buffer);
        const size_t bytes = count * sizeof(MyGUI::Vertex);
        if (vb->byteSize() < bytes)
            return; // not locked/filled this frame

        Batch batch;
        batch.vertices = vsg::ubyteArray::create(bytes);
        std::memcpy(batch.vertices->dataPointer(), vb->bytes(), bytes);
        // Checked cast: widgets can carry a texture created by a different backend (InventoryWindow's avatar
        // hands MyGUI an OSG-backed ITexture). Those aren't ours — fall back to the white texture in
        // buildOverlayNode rather than reinterpreting a foreign object as a VsgMyGui::Texture.
        batch.texture = dynamic_cast<Texture*>(texture);
        if (texture && !batch.texture)
        {
            if (!mWarnedForeignTexture)
            {
                Log(Debug::Warning) << "VsgMyGui: skipping an OSG or foreign render-target texture; "
                                       "cross-API map, preview, save and video views are not implemented";
                mWarnedForeignTexture = true;
            }
            return;
        }
        batch.count = static_cast<uint32_t>(count);
        mBatches.push_back(std::move(batch));
    }

    vsg::ref_ptr<vsg::Node> RenderManager::buildOverlayNode() const
    {
        if (mBatches.empty() || !mPipeline)
            return {};

        auto root = vsg::Group::create();
        for (const Batch& b : mBatches)
        {
            vsg::ref_ptr<vsg::Data> tex = (b.texture && b.texture->data()) ? b.texture->data() : mPipeline.whiteTexture;
            auto info = vsg::ImageInfo::create(mPipeline.sampler, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            auto image = vsg::DescriptorImage::create(info, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            auto ds = vsg::DescriptorSet::create(mPipeline.descriptorSetLayout, vsg::Descriptors{ image });
            auto bindDs
                = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline.pipelineLayout, 0, ds);

            auto sg = vsg::StateGroup::create();
            sg->add(mPipeline.bindPipeline);
            sg->add(bindDs);
            sg->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ b.vertices }));
            sg->addChild(vsg::Draw::create(b.count, 1, 0, 0));
            root->addChild(sg);
        }
        return root;
    }

    void RenderManager::collect()
    {
        begin();
        onRenderToTarget(this, true); // MyGUI walks its layers → doRender per batch (requires MyGUI::Gui)
        end();
    }

    vsg::ref_ptr<vsg::Node> RenderManager::collectDrawCalls()
    {
        collect();
        return buildOverlayNode();
    }

    namespace
    {
        constexpr uint32_t kInitialVertsPerSlot = 4096;

        [[nodiscard]] uint32_t slotCapacity(uint32_t required)
        {
            uint32_t result = kInitialVertsPerSlot;
            while (result < required && result <= std::numeric_limits<uint32_t>::max() / 2)
                result *= 2;
            return std::max(result, required);
        }
    }

    vsg::ref_ptr<vsg::Node> RenderManager::buildPersistentOverlay()
    {
        mSlots.clear();
        if (!mPipeline)
            return {};

        auto root = vsg::Group::create();
        for (size_t i = 0; i < mBatches.size(); ++i)
        {
            const Batch& b = mBatches[i];
            Slot slot;
            slot.texture = b.texture;
            const TextureKey key = textureKey(b.texture);
            slot.textureIdentity = key.first;
            slot.textureRevision = key.second;
            // Reuse a pooled DYNAMIC buffer for this slot index (allocated + GPU-compiled once, then reused across
            // rebuilds) rather than allocating a fresh one each rebuild — the fresh-alloc path was the leak.
            if (i >= mVertPool.size() || mVertPoolCapacities[i] < b.count)
            {
                const uint32_t capacity = slotCapacity(b.count);
                auto buf = vsg::ubyteArray::create(capacity * sizeof(MyGUI::Vertex));
                buf->properties.dataVariance = vsg::DYNAMIC_DATA; // dirty() re-uploads without recompiling
                if (i >= mVertPool.size())
                {
                    mVertPool.push_back(buf);
                    mVertPoolCapacities.push_back(capacity);
                }
                else
                {
                    mVertPool[i] = buf;
                    mVertPoolCapacities[i] = capacity;
                }
            }
            slot.verts = mVertPool[i];
            slot.capacity = mVertPoolCapacities[i];
            const uint32_t n = b.count;
            if (b.vertices && n > 0)
                std::memcpy(slot.verts->dataPointer(), b.vertices->dataPointer(), n * sizeof(MyGUI::Vertex));
            slot.verts->dirty(); // pooled buffer already compiled → mark for re-upload
            slot.draw = vsg::Draw::create(n, 1, 0, 0);

            auto sg = vsg::StateGroup::create();
            sg->add(mPipeline.bindPipeline);
            sg->add(bindDescriptorSetFor(b.texture)); // cached per texture → compile is a no-op
            sg->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ slot.verts }));
            sg->addChild(slot.draw);
            root->addChild(sg);
            mSlots.push_back(slot);
        }
        return root;
    }

    vsg::ref_ptr<vsg::BindDescriptorSet> RenderManager::bindDescriptorSetFor(const Texture* texture)
    {
        // One compiled descriptor set (bind command) per texture identity, cached so overlay rebuilds reference the
        // already-compiled resource instead of allocating a new one each frame. Evicted in create/destroyTexture when
        // the underlying image changes, so a cached entry never points at stale texture data.
        const TextureKey key = textureKey(texture);
        if (auto it = mDsCache.find(key); it != mDsCache.end())
            return it->second;
        if (key.first != 0)
        {
            std::erase_if(mDsCache,
                [&key](const auto& value) { return value.first.first == key.first && value.first != key; });
        }
        vsg::ref_ptr<vsg::Data> tex = (texture && texture->data()) ? texture->data() : mPipeline.whiteTexture;
        auto info = vsg::ImageInfo::create(mPipeline.sampler, tex, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto image = vsg::DescriptorImage::create(info, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        auto ds = vsg::DescriptorSet::create(mPipeline.descriptorSetLayout, vsg::Descriptors{ image });
        auto bindDs = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, mPipeline.pipelineLayout, 0, ds);
        mDsCache.emplace(key, bindDs);
        return bindDs;
    }

    RenderManager::TextureKey RenderManager::textureKey(const Texture* texture) noexcept
    {
        return texture ? TextureKey{ texture->identity(), texture->revision() } : TextureKey{};
    }

    void RenderManager::updatePersistentOverlay()
    {
        // Leave last frame's overlay untouched if the batch structure changed (widget set / atlas differs).
        if (mSlots.empty() || mBatches.size() != mSlots.size())
        {
            if (!mWarnedStructure && !mSlots.empty())
            {
                Log(Debug::Warning) << "VsgMyGui: overlay batch structure changed (" << mSlots.size() << " -> "
                                    << mBatches.size() << "); overlay frozen at its previous state";
                mWarnedStructure = true;
            }
            return;
        }
        for (size_t i = 0; i < mSlots.size(); ++i)
        {
            const TextureKey key = textureKey(mBatches[i].texture);
            if (key.first != mSlots[i].textureIdentity || key.second != mSlots[i].textureRevision)
                continue; // texture identity changed → don't feed it through a stale descriptor
            const uint32_t n = mBatches[i].count;
            if (mBatches[i].vertices && n > 0)
            {
                std::memcpy(
                    mSlots[i].verts->dataPointer(), mBatches[i].vertices->dataPointer(), n * sizeof(MyGUI::Vertex));
                mSlots[i].verts->dirty();
            }
            mSlots[i].draw->vertexCount = n;
        }
    }

    bool RenderManager::overlayStructureChanged() const
    {
        // A different number of batches, or a batch bound to a different texture than its slot's descriptor was built
        // for, means an in-place refresh can't represent the new frame — the host must rebuild the node. The rebuild is
        // cheap and non-leaking: buildPersistentOverlay reuses cached descriptor sets + pooled vertex buffers, so
        // compileManager->compile() allocates nothing. (The game HUD reorders batches most frames, so this is common.)
        if (mBatches.size() != mSlots.size())
            return true;
        for (size_t i = 0; i < mSlots.size(); ++i)
        {
            const TextureKey key = textureKey(mBatches[i].texture);
            if (key.first != mSlots[i].textureIdentity || key.second != mSlots[i].textureRevision)
                return true;
            if (mBatches[i].count > mSlots[i].capacity)
                return true;
        }
        return false;
    }

    vsg::ref_ptr<vsg::Node> buildSelfTest(RenderManager& rm)
    {
        // 1) A generated checkerboard via createManual + lock/unlock — the exact upload path MyGUI uses for font
        //    atlases and dynamic images. Proves createManual + the RGBA expansion + GPU sampling end-to-end.
        constexpr int ts = 64;
        auto* tex = static_cast<Texture*>(rm.createTexture("__vsgmygui_selftest"));
        tex->createManual(ts, ts, MyGUI::TextureUsage::Static, MyGUI::PixelFormat::R8G8B8A8);
        auto* px = static_cast<uint8_t*>(tex->lock(MyGUI::TextureUsage::Static));
        for (int y = 0; y < ts; ++y)
            for (int x = 0; x < ts; ++x)
            {
                const bool c = (((x / 8) + (y / 8)) & 1) != 0;
                uint8_t* p = px + (static_cast<size_t>(y) * ts + x) * 4;
                p[0] = c ? 245 : 25;
                p[1] = c ? 205 : 25;
                p[2] = c ? 45 : 130;
                p[3] = 255;
            }
        tex->unlock();

        // 2) A MyGUI::VertexBuffer holding a textured quad in clip space (MyGUI's GL Y-up convention; the UI shader
        //    flips Y). White vertex colour so the checkerboard shows unmodified.
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

        // 3) Drive the render path exactly as MyGUI would.
        rm.begin();
        rm.doRender(buffer, tex, 6);
        rm.end();

        auto node = rm.buildOverlayNode();
        rm.destroyVertexBuffer(buffer);
        return node;
    }
}
