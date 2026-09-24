#ifndef OPENMW_COMPONENTS_RENDERCORE_PERSISTENTDRAW_H
#define OPENMW_COMPONENTS_RENDERCORE_PERSISTENTDRAW_H

#include "effectframe.hpp"
#include <array>
#include <atomic>
#include <limits>
#include <stdexcept>

namespace RenderCore
{
    // Asset ownership is independent of placement. Only construction/rebinding
    // copies the material and texture descriptions; transform updates share them.
    class PersistentDrawResource final
    {
    public:
        explicit PersistentDrawResource(const ImmediateEffectDraw& source) : mDraw(source)
        {
            if (!mDraw.meshSnapshot)
                mDraw.meshSnapshot = std::make_shared<const FrozenEffectMesh>(mDraw.mesh);
            mDraw.mesh = {};
            for (auto& texture : mDraw.textures)
                if (texture.texture.pixels)
                    texture.texture.pixels = std::make_shared<const TexturePixels>(*texture.texture.pixels);
            if (!validImmediateEffectDraw(mDraw))
                throw std::invalid_argument("invalid persistent draw resource");
        }
        PersistentDrawResource(const PersistentDrawResource&) = delete;
        PersistentDrawResource& operator=(const PersistentDrawResource&) = delete;
        const ImmediateEffectDraw& draw() const noexcept { return mDraw; }
    private:
        ImmediateEffectDraw mDraw;
    };

    struct PersistentDrawHandle
    {
        std::uint64_t stream = 0, generation = 0;
        std::uint32_t slot = 0;
        bool operator==(const PersistentDrawHandle&) const = default;
    };
    struct PersistentDrawEntry
    {
        PersistentDrawHandle handle;
        std::shared_ptr<const PersistentDrawResource> resource;
        glm::mat4 transform{1.f};
        bool visible = true;
    };

    class PersistentDrawWorld;
    // Sparse copy-on-write pages are recovery state, NOT a per-frame descriptor
    // snapshot. Normal consumption visits changed slots only. A late/new consumer
    // can resynchronize from any frame without retaining an unbounded event log.
    class PersistentDrawFrame final
    {
    public:
        PersistentDrawFrame() = default;
        PersistentDrawFrame(const PersistentDrawFrame&) = delete;
        PersistentDrawFrame& operator=(const PersistentDrawFrame&) = delete;
        static constexpr std::size_t PageSize = 64;
        using Page = std::array<std::shared_ptr<const PersistentDrawEntry>, PageSize>;
        std::uint64_t stream() const noexcept { return mStream; }
        std::uint64_t revision() const noexcept { return mRevision; }
        std::uint64_t baseRevision() const noexcept { return mBaseRevision; }
        const std::vector<std::uint32_t>& changes() const noexcept { return mChanges; }
        const PersistentDrawEntry* get(std::uint32_t slot) const noexcept
        {
            const auto page = slot / PageSize;
            return page < mPages.size() ? (*mPages[page])[slot % PageSize].get() : nullptr;
        }
        template<class F> void forEach(F&& visit) const
        {
            for (const auto& page : mPages)
                for (const auto& value : *page) if (value) visit(*value);
        }
        std::size_t size() const noexcept { return mSize; }
    private:
        friend class PersistentDrawWorld;
        std::uint64_t mStream = 0, mRevision = 0, mBaseRevision = 0;
        std::size_t mSize = 0;
        std::vector<std::shared_ptr<const Page>> mPages;
        std::vector<std::uint32_t> mChanges;
    };

    // Update-thread owned. Handles include stream and generation so unload/reload,
    // world reset and recycled slots cannot accidentally update an older object.
    class PersistentDrawWorld
    {
    public:
        explicit PersistentDrawWorld(std::size_t maximumSlots = 131072) : mMaximumSlots(maximumSlots) {}
        void begin(std::uint64_t epoch)
        {
            if (epoch != mEpoch)
            {
                mEpoch = epoch; mStream = ++sStream; mPages.clear(); mSeen.clear();
                mFree.clear(); mDirty.clear(); mChanges.clear(); mFrame.reset(); mSize = 0;
            }
            ++mCapture;
        }
        const PersistentDrawEntry* get(PersistentDrawHandle handle) const noexcept
        {
            const auto* entry = at(handle.slot);
            return handle.stream == mStream && entry && entry->handle == handle ? entry : nullptr;
        }
        // Returns false at the bounded slot limit, permitting object-local fallback.
        bool update(PersistentDrawHandle& handle, const ImmediateEffectDraw& draw, bool resourceChanged)
        {
            auto* previous = get(handle);
            if (!semantic_detail::finite(draw.worldTransform))
                throw std::invalid_argument("nonfinite persistent placement");
            // Validate/freeze before allocating a slot or replacing its identity.
            // A rejected asset must not consume the bounded slot budget.
            std::shared_ptr<const PersistentDrawResource> resource;
            if (!previous || resourceChanged)
                resource = std::make_shared<const PersistentDrawResource>(draw);
            if (!previous)
            {
                if (mFree.empty() && mSeen.size() >= mMaximumSlots) return false;
                const auto slot = mFree.empty() ? static_cast<std::uint32_t>(mSeen.size()) : mFree.back();
                if (!mFree.empty()) mFree.pop_back();
                else { mSeen.push_back(0); mDirty.push_back(false); }
                handle = {mStream, ++mGeneration, slot};
                resourceChanged = true;
            }
            mSeen[handle.slot] = mCapture;
            if (!resourceChanged && previous->visible && previous->transform == draw.worldTransform) return true;
            auto entry = std::make_shared<PersistentDrawEntry>();
            entry->handle = handle; entry->transform = draw.worldTransform;
            entry->resource = resourceChanged ? std::move(resource) : previous->resource;
            if (!previous) ++mSize;
            assign(handle.slot, std::move(entry));
            return true;
        }
        void hide(PersistentDrawHandle handle)
        {
            const auto* previous = get(handle);
            if (!previous) return;
            mSeen[handle.slot] = mCapture;
            if (!previous->visible) return;
            auto entry = std::make_shared<PersistentDrawEntry>(*previous);
            entry->visible = false;
            assign(handle.slot, std::move(entry));
        }
        void remove(PersistentDrawHandle handle)
        {
            if (!get(handle)) return;
            assign(handle.slot, {}); mFree.push_back(handle.slot); --mSize;
        }
        std::shared_ptr<const PersistentDrawFrame> finish()
        {
            for (std::uint32_t slot = 0; slot < mSeen.size(); ++slot)
                if (const auto* entry = at(slot); entry && mSeen[slot] != mCapture) remove(entry->handle);
            if (mChanges.empty() && mFrame) return mFrame;
            auto frame = std::make_shared<PersistentDrawFrame>();
            frame->mStream = mStream; frame->mSize = mSize;
            frame->mBaseRevision = mFrame ? mFrame->revision() : 0;
            frame->mRevision = frame->mBaseRevision + 1;
            frame->mPages.assign(mPages.begin(), mPages.end());
            frame->mChanges = std::move(mChanges);
            for (auto slot : frame->mChanges) mDirty[slot] = false;
            mChanges.clear(); mFrame = frame; return frame;
        }
    private:
        const PersistentDrawEntry* at(std::uint32_t slot) const noexcept
        { return slot / PersistentDrawFrame::PageSize < mPages.size()
            ? (*mPages[slot / PersistentDrawFrame::PageSize])[slot % PersistentDrawFrame::PageSize].get() : nullptr; }
        void assign(std::uint32_t slot, std::shared_ptr<const PersistentDrawEntry> entry)
        {
            const auto page = slot / PersistentDrawFrame::PageSize;
            while (mPages.size() <= page) mPages.push_back(std::make_shared<PersistentDrawFrame::Page>());
            if (mPages[page].use_count() != 1) mPages[page] = std::make_shared<PersistentDrawFrame::Page>(*mPages[page]);
            (*mPages[page])[slot % PersistentDrawFrame::PageSize] = std::move(entry);
            if (!mDirty[slot]) { mDirty[slot] = true; mChanges.push_back(slot); }
        }
        inline static std::atomic<std::uint64_t> sStream{0};
        std::size_t mMaximumSlots, mSize = 0;
        std::uint64_t mStream = ++sStream, mEpoch = 0, mCapture = 0, mGeneration = 0;
        std::vector<std::shared_ptr<PersistentDrawFrame::Page>> mPages;
        std::vector<std::uint64_t> mSeen;
        std::vector<bool> mDirty;
        std::vector<std::uint32_t> mFree, mChanges;
        std::shared_ptr<const PersistentDrawFrame> mFrame;
    };
}
#endif
