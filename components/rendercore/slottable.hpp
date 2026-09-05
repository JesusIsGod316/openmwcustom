#ifndef OPENMW_COMPONENTS_RENDERCORE_SLOTTABLE_H
#define OPENMW_COMPONENTS_RENDERCORE_SLOTTABLE_H

#include "handles.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
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
    //
    // A slot may be reserved before its semantic create operation is published.
    // Reserved identities are not visible to readers until commit(). This lets
    // a single producer establish stable handles while still preserving an
    // immutable RenderWorld read surface for the active render phase.
    template <class HandleT, class Payload>
    class SlotTable
    {
    public:
        using Handle = HandleT;
        using SlotIndex = typename Handle::Slot;
        using Generation = typename Handle::Generation;

        [[nodiscard]] std::optional<Handle> reserve()
        {
            if (mLowestVacant != InvalidHandleSlot)
            {
                const SlotIndex slotIndex = mLowestVacant;
                Slot& slot = mSlots[slotIndex];
                slot.state = State::Reserved;
                refreshLowestVacant(static_cast<std::size_t>(slotIndex) + 1);
                return Handle::fromParts(slotIndex, slot.generation);
            }

            if (mSlots.size() >= static_cast<std::size_t>(InvalidHandleSlot))
                return std::nullopt;

            const SlotIndex slotIndex = static_cast<SlotIndex>(mSlots.size());
            Slot slot;
            slot.generation = 1;
            slot.state = State::Reserved;
            mSlots.push_back(std::move(slot));
            return Handle::fromParts(slotIndex, 1);
        }

        bool commit(Handle handle, Payload payload)
        {
            Slot* slot = resolveState(handle, State::Reserved);
            if (!slot)
                return false;

            slot->payload.emplace(std::move(payload));
            slot->state = State::Live;
            ++mLiveCount;
            return true;
        }

        // Replaces the payload of the same live logical object without changing
        // its slot/generation identity. RenderWorld owns semantic revision checks.
        bool update(Handle handle, Payload payload)
        {
            Slot* slot = resolveState(handle, State::Live);
            if (!slot || !slot->payload)
                return false;

            *slot->payload = std::move(payload);
            return true;
        }

        bool cancel(Handle handle) noexcept
        {
            Slot* slot = resolveState(handle, State::Reserved);
            if (!slot)
                return false;
            release(handle.slot(), *slot);
            return true;
        }

        [[nodiscard]] std::optional<Handle> insert(Payload payload)
        {
            const auto handle = reserve();
            if (!handle)
                return std::nullopt;

            try
            {
                if (!commit(*handle, std::move(payload)))
                {
                    cancel(*handle);
                    return std::nullopt;
                }
            }
            catch (...)
            {
                cancel(*handle);
                throw;
            }
            return handle;
        }

        [[nodiscard]] Payload* get(Handle handle) noexcept
        {
            Slot* slot = resolveState(handle, State::Live);
            return slot && slot->payload ? std::addressof(*slot->payload) : nullptr;
        }

        [[nodiscard]] const Payload* get(Handle handle) const noexcept
        {
            const Slot* slot = resolveState(handle, State::Live);
            return slot && slot->payload ? std::addressof(*slot->payload) : nullptr;
        }

        [[nodiscard]] bool contains(Handle handle) const noexcept { return get(handle) != nullptr; }
        [[nodiscard]] bool isReserved(Handle handle) const noexcept
        {
            return resolveState(handle, State::Reserved) != nullptr;
        }

        // Read-only deterministic live-table walk for validation/dependency checks.
        // Iteration order is slot order and therefore stable for a given operation stream.
        template <class Fn>
        void forEachLive(Fn&& fn) const
        {
            for (std::size_t i = 0; i < mSlots.size(); ++i)
            {
                const Slot& slot = mSlots[i];
                if (slot.state == State::Live && slot.payload)
                    fn(Handle::fromParts(static_cast<SlotIndex>(i), slot.generation), *slot.payload);
            }
        }

        bool retire(Handle handle) noexcept
        {
            Slot* slot = resolveState(handle, State::Live);
            if (!slot || !slot->payload)
                return false;

            slot->payload.reset();
            --mLiveCount;
            release(handle.slot(), *slot);
            return true;
        }

        void retireAll() noexcept
        {
            for (SlotIndex slotIndex = 0; slotIndex < mSlots.size(); ++slotIndex)
            {
                Slot& slot = mSlots[slotIndex];
                if (slot.state == State::Live)
                {
                    slot.payload.reset();
                    --mLiveCount;
                    release(slotIndex, slot);
                }
                else if (slot.state == State::Reserved)
                    release(slotIndex, slot);
            }
        }

        [[nodiscard]] std::size_t liveCount() const noexcept { return mLiveCount; }
        [[nodiscard]] std::size_t slotCount() const noexcept { return mSlots.size(); }

        [[nodiscard]] bool isTombstonedSlot(SlotIndex slotIndex) const noexcept
        {
            return slotIndex < mSlots.size() && mSlots[slotIndex].state == State::Tombstone;
        }

    private:
        enum class State : std::uint8_t
        {
            Vacant,
            Reserved,
            Live,
            Tombstone,
        };

        struct Slot
        {
            Generation generation = 0;
            State state = State::Vacant;
            std::optional<Payload> payload;
        };

        void release(SlotIndex slotIndex, Slot& slot) noexcept
        {
            const auto retired = detail::retireGeneration(slot.generation);
            if (retired.tombstone)
            {
                slot.generation = 0;
                slot.state = State::Tombstone;
                return;
            }

            slot.generation = retired.next;
            slot.state = State::Vacant;
            if (mLowestVacant == InvalidHandleSlot || slotIndex < mLowestVacant)
                mLowestVacant = slotIndex;
        }

        void refreshLowestVacant(std::size_t start) noexcept
        {
            mLowestVacant = InvalidHandleSlot;
            for (std::size_t i = start; i < mSlots.size(); ++i)
            {
                if (mSlots[i].state == State::Vacant)
                {
                    mLowestVacant = static_cast<SlotIndex>(i);
                    return;
                }
            }
        }

        [[nodiscard]] Slot* resolveState(Handle handle, State state) noexcept
        {
            if (!handle.valid() || handle.slot() >= mSlots.size())
                return nullptr;
            Slot& slot = mSlots[handle.slot()];
            if (slot.state != state || slot.generation != handle.generation())
                return nullptr;
            return &slot;
        }

        [[nodiscard]] const Slot* resolveState(Handle handle, State state) const noexcept
        {
            if (!handle.valid() || handle.slot() >= mSlots.size())
                return nullptr;
            const Slot& slot = mSlots[handle.slot()];
            if (slot.state != state || slot.generation != handle.generation())
                return nullptr;
            return &slot;
        }

        std::vector<Slot> mSlots;
        SlotIndex mLowestVacant = InvalidHandleSlot;
        std::size_t mLiveCount = 0;
    };
}

#endif
