#include "rendermanager.hpp"

#include <algorithm>
#include <cstring>
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

        vsg::ref_ptr<vsg::ImageInfo> imageInfoFor(
            const RenderVsg::UiPipeline& pipeline, const Texture* texture)
        {
            if (texture && texture->imageView())
                return vsg::ImageInfo::create(
                    pipeline.sampler, texture->imageView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            vsg::ref_ptr<vsg::Data> data
                = (texture && texture->data()) ? texture->data() : pipeline.whiteTexture;
            return vsg::ImageInfo::create(pipeline.sampler, data, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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
        mIsInitialise = true;
    }

    void RenderManager::shutdown()
    {
        mDsCache.clear();
        mSlots.clear();
        mTextures.clear();
        mBatches.clear();
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
        if (auto existing = mTextures.find(name); existing != mTextures.end())
            forgetTexture(&existing->second);
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
        else
            forgetTexture(&it->second);
        it->second.setImageView(std::move(imageView), width, height, format);
        return &it->second;
    }

    void RenderManager::destroyTexture(MyGUI::ITexture* texture)
    {
        if (!texture)
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

    bool RenderManager::checkTexture(MyGUI::ITexture* texture)
    {
        if (!texture || dynamic_cast<Texture*>(texture))
            return true;
        const std::string& name = texture->getName();
        return !name.empty() && mTextures.contains(name);
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
        if (!buffer || count == 0)
            return;
        auto* vb = static_cast<VertexBuffer*>(buffer);
        const size_t bytes = count * sizeof(MyGUI::Vertex);
        if (vb->byteSize() < bytes)
            return;

        Batch batch;
        batch.vertices = vsg::ubyteArray::create(bytes);
        std::memcpy(batch.vertices->dataPointer(), vb->bytes(), bytes);
        batch.texture = dynamic_cast<Texture*>(texture);
        if (texture && !batch.texture)
        {
            const std::string& alias = texture->getName();
            if (!alias.empty())
            {
                if (auto native = mTextures.find(alias); native != mTextures.end())
                    batch.texture = &native->second;
            }
            if (!batch.texture)
            {
                if (!mWarnedForeignTexture)
                {
                    Log(Debug::Warning) << "VsgMyGui: skipping an unresolved foreign render-target texture; "
                                           "native auxiliary surfaces must be published before an alias is rendered";
                    mWarnedForeignTexture = true;
                }
                return;
            }
        }
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
            auto info = imageInfoFor(mPipeline, batch.texture);
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
        onRenderToTarget(this, true);
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

        uint32_t slotCapacity(uint32_t required)
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
            const Batch& batch = mBatches[i];
            Slot slot;
            slot.texture = batch.texture;
            const TextureKey key = textureKey(batch.texture);
            slot.textureIdentity = key.first;
            slot.textureRevision = key.second;
            if (i >= mVertPool.size() || mVertPoolCapacities[i] < batch.count)
            {
                const uint32_t capacity = slotCapacity(batch.count);
                auto buffer = vsg::ubyteArray::create(capacity * sizeof(MyGUI::Vertex));
                buffer->properties.dataVariance = vsg::DYNAMIC_DATA;
                if (i >= mVertPool.size())
                {
                    mVertPool.push_back(buffer);
                    mVertPoolCapacities.push_back(capacity);
                }
                else
                {
                    mVertPool[i] = buffer;
                    mVertPoolCapacities[i] = capacity;
                }
            }
            slot.verts = mVertPool[i];
            slot.capacity = mVertPoolCapacities[i];
            const uint32_t count = batch.count;
            if (batch.vertices && count > 0)
                std::memcpy(slot.verts->dataPointer(), batch.vertices->dataPointer(), count * sizeof(MyGUI::Vertex));
            slot.verts->dirty();
            slot.draw = vsg::Draw::create(count, 1, 0, 0);

            auto sg = vsg::StateGroup::create();
            sg->add(mPipeline.bindPipeline);
            sg->add(bindDescriptorSetFor(batch.texture));
            sg->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ slot.verts }));
            sg->addChild(slot.draw);
            root->addChild(sg);
            mSlots.push_back(slot);
        }
        return root;
    }

    vsg::ref_ptr<vsg::BindDescriptorSet> RenderManager::bindDescriptorSetFor(const Texture* texture)
    {
        const TextureKey key = textureKey(texture);
        if (auto it = mDsCache.find(key); it != mDsCache.end())
            return it->second;
        if (key.first != 0)
            std::erase_if(mDsCache,
                [&key](const auto& value) { return value.first.first == key.first && value.first != key; });

        auto info = imageInfoFor(mPipeline, texture);
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
                continue;
            const uint32_t count = mBatches[i].count;
            if (mBatches[i].vertices && count > 0)
            {
                std::memcpy(mSlots[i].verts->dataPointer(), mBatches[i].vertices->dataPointer(),
                    count * sizeof(MyGUI::Vertex));
                mSlots[i].verts->dirty();
            }
            mSlots[i].draw->vertexCount = count;
        }
    }

    bool RenderManager::overlayStructureChanged() const
    {
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
        auto node = rm.buildOverlayNode();
        rm.destroyVertexBuffer(buffer);
        return node;
    }
}