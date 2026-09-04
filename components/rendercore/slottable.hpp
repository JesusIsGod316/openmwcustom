#ifndef OPENMW_COMPONENTS_RENDERCORE_SLOTTABLE_H
#define OPENMW_COMPONENTS_RENDERCORE_SLOTTABLE_H

#include "handles.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace RenderCore
{
    namespace detail
    {
        template <class Generation>
        struct RetiredGeneration
        {
            Generation next = 0;
            bool tombstone = false;
        };

        template <class Generation>
        [[nodiscard]] constexpr RetiredGeneration<Generation> retireGeneration(Generation current) noexcept
        {
            if (current == std::numeric_limits<Generation>::max())
                return { 0, true };
            return { static_cast<Generation>(current + 1), false };
        }
    }

    // Deterministic generation-safe storage for one logical handle family.
    // Reusable slots are selected by lowest slot index; allocation therefore
    // depends only on the ordered semantic operation stream, never pointer or
    // hash iteration order. A generation that would wrap to zero permanently
    // tombstones its slot instead of allowing stale identity to alias.
    template <class HandleT, class Payload>
    class SlotTable
    {
    public:
        using Handle = HandleT;
        using SlotIndex = typename Handle::Slot;
        using Generation = typename Handle::Generation;

        [[nodiscard]] std::optional<Handle> insert(Payload payload)
        {
            if (!mReusable.empty())
            {
                const SlotIndex slotIndex = *mReusable.begin();
                mReusable.erase(mReusable.begin());

                Slot& slot = mSlots[slotIndex];
                slot.live = true;
                slot.payload.emplace(std::move(payload));
                ++mLiveCount;
                return Handle::fromParts(slotIndex, slot.generation);
            }

            if (mSlots.size() >= static_cast<std::size_t>(InvalidHandleSlot))
                return std::nullopt;

            const SlotIndex slotIndex = static_cast<SlotIndex>(mSlots.size());
            Slot slot;
            slot.generation = 1;
            slot.live = true;
            slot.payload.emplace(std::move(payload));
            mSlots.push_back(std::move(slot));
            ++mLiveCount;
            return Handle::fromParts(slotIndex, 1);
        }

        [[nodiscard]] Payload* get(Handle handle) noexcept
        {
            Slot* slot = resolve(handle);
            return slot ? std::addressof(*slot->payload) : nullptr;
        }

        [[nodiscard]] const Payload* get(Handle handle) const noexcept
        {
            const Slot* slot = resolve(handle);
            return slot ? std::addressof(*slot->payload) : nullptr;
        }

        [[nodiscard]] bool contains(Handle handle) const noexcept { return resolve(handle) != nullptr; }

        bool retire(Handle handle)
        {
            Slot* slot = resolve(handle);
            if (!slot)
                return false;

            slot->payload.reset();
            slot->live = false;
            --mLiveCount;

            const auto retired = detail::retireGeneration(slot->generation);
            if (retired.tombstone)
            {
                slot->tombstone = true;
                slot->generation = 0;
                return true;
            }

            slot->generation = retired.next;
            mReusable.insert(handle.slot());
            return true;
        }

        void retireAll()
        {
            for (SlotIndex slotIndex = 0; slotIndex < mSlots.size(); ++slotIndex)
            {
                Slot& slot = mSlots[slotIndex];
                if (!slot.live)
                    continue;
                retire(Handle::fromParts(slotIndex, slot.generation));
            }
        }

        [[nodiscard]] std::size_t liveCount() const noexcept { return mLiveCount; }
        [[nodiscard]] std::size_t slotCount() const noexcept { return mSlots.size(); }

        [[nodiscard]] bool isTombstonedSlot(SlotIndex slotIndex) const noexcept
        {
            return slotIndex < mSlots.size() && mSlots[slotIndex].tombstone;
        }

    private:
        struct Slot
        {
            Generation generation = 0;
            bool live = false;
            bool tombstone = false;
            std::optional<Payload> payload;
        };

        [[nodiscard]] Slot* resolve(Handle handle) noexcept
        {
            if (!handle.valid() || handle.slot() >= mSlots.size())
                return nullptr;
            Slot& slot = mSlots[handle.slot()];
            if (!slot.live || slot.tombstone || slot.generation != handle.generation() || !slot.payload)
                return nullptr;
            return &slot;
        }

        [[nodiscard]] const Slot* resolve(Handle handle) const noexcept
        {
            if (!handle.valid() || handle.slot() >= mSlots.size())
                return nullptr;
            const Slot& slot = mSlots[handle.slot()];
            if (!slot.live || slot.tombstone || slot.generation != handle.generation() || !slot.payload)
                return nullptr;
            return &slot;
        }

        std::vector<Slot> mSlots;
        std::set<SlotIndex> mReusable;
        std::size_t mLiveCount = 0;
    };
}

#endif
