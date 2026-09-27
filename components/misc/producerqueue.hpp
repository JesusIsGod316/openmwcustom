#ifndef OPENMW_COMPONENTS_MISC_PRODUCERQUEUE_H
#define OPENMW_COMPONENTS_MISC_PRODUCERQUEUE_H

#include <compare>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace Misc
{
    // Coalescing, bounded producer work list. No engine/scene pointer is stored
    // here. Mutation/destruction may signal from another thread, but registration
    // and consumption belong to the engine owner. Notifications allocate nothing.
    class ProducerQueue
    {
        static constexpr std::uint32_t None = std::numeric_limits<std::uint32_t>::max();
    public:
        struct Token
        {
            std::uint32_t slot = None;
            std::uint64_t generation = 0;
            auto operator<=>(const Token&) const = default;
        };
        struct Change { Token token; unsigned reasons = 0; };
    private:
        struct Slot
        {
            std::uint64_t generation = 0;
            std::uint32_t next = None, previous = None;
            unsigned reasons = 0;
            bool live = false, queued = false;
        };
        struct State
        {
            std::mutex mutex;
            std::vector<Slot> slots;
            std::set<std::uint32_t> continuous;
            std::uint32_t first = None, last = None, free = None;
            std::uint64_t generation = 0;
            std::size_t live = 0, pending = 0;
            bool valid(Token token) const noexcept
            { return token.slot < slots.size() && slots[token.slot].live
                && slots[token.slot].generation == token.generation; }
            void enqueue(std::uint32_t index, unsigned reasons) noexcept
            {
                auto& slot = slots[index]; slot.reasons |= reasons;
                if (slot.queued) return;
                slot.queued = true; slot.previous = last; slot.next = None;
                if (last != None) slots[last].next = index; else first = index;
                last = index; ++pending;
            }
            void unlink(std::uint32_t index) noexcept
            {
                auto& slot = slots[index];
                if (!slot.queued) return;
                if (slot.previous != None) slots[slot.previous].next = slot.next; else first = slot.next;
                if (slot.next != None) slots[slot.next].previous = slot.previous; else last = slot.previous;
                slot.queued = false; slot.next = slot.previous = None; --pending;
            }
        };
    public:
        class Ticket
        {
            friend class ProducerQueue;
            Ticket(std::shared_ptr<State> state, Token token) : mState(state), mToken(token) {}
        public:
            ~Ticket() { cancel(); }
            Ticket(const Ticket&) = delete;
            Ticket& operator=(const Ticket&) = delete;
            Token token() const noexcept { return mToken; }
            void notify(unsigned reasons = 1) const noexcept
            {
                if (auto state = mState.lock())
                {
                    std::lock_guard lock(state->mutex);
                    if (state->valid(mToken)) state->enqueue(mToken.slot, reasons);
                }
            }
            void continuous(bool value) const
            {
                if (auto state = mState.lock())
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->valid(mToken)) return;
                    if (value) state->continuous.insert(mToken.slot);
                    else state->continuous.erase(mToken.slot);
                }
            }
            void cancel() const noexcept
            {
                if (auto state = mState.lock())
                {
                    std::lock_guard lock(state->mutex);
                    if (!state->valid(mToken)) return;
                    state->unlink(mToken.slot); state->continuous.erase(mToken.slot);
                    auto& slot = state->slots[mToken.slot];
                    slot.live = false; slot.reasons = 0; slot.next = state->free;
                    state->free = mToken.slot; --state->live;
                }
            }
        private:
            std::weak_ptr<State> mState;
            Token mToken;
        };
        explicit ProducerQueue(std::size_t capacity = 131072)
            : mState(std::make_shared<State>()), mCapacity(capacity) {}
        ProducerQueue(const ProducerQueue&) = delete;
        ProducerQueue& operator=(const ProducerQueue&) = delete;
        std::shared_ptr<Ticket> add()
        {
            std::lock_guard lock(mState->mutex);
            if (mState->live >= mCapacity || mState->generation == std::numeric_limits<std::uint64_t>::max())
                return {}; // caller retains its explicit compatibility work set
            const auto index = mState->free == None
                ? static_cast<std::uint32_t>(mState->slots.size()) : mState->free;
            if (index == None) return {};
            const Token token{index, mState->generation + 1};
            // Allocate first, before committing a registration.
            auto ticket = std::shared_ptr<Ticket>(new Ticket({}, token));
            if (mState->free == None) mState->slots.emplace_back();
            else mState->free = mState->slots[index].next;
            mState->slots[index] = {};
            auto& slot = mState->slots[index];
            slot.live = true; slot.generation = ++mState->generation; ++mState->live;
            mState->enqueue(index, 1);
            ticket->mState = mState;
            return ticket;
        }
        bool valid(Token token) const
        {
            std::lock_guard lock(mState->mutex); return mState->valid(token);
        }
        // Only epoch/configuration changes use this resident inventory walk.
        void invalidateAll(unsigned reasons = 1)
        {
            std::lock_guard lock(mState->mutex);
            for (std::uint32_t i = 0; i < mState->slots.size(); ++i)
                if (mState->slots[i].live) mState->enqueue(i, reasons);
        }
        std::vector<Change> take()
        {
            std::lock_guard lock(mState->mutex);
            std::vector<Change> result;
            result.reserve(mState->continuous.size() + mState->pending);
            for (auto index : mState->continuous)
                result.push_back({{index, mState->slots[index].generation}, mState->slots[index].reasons});
            while (mState->first != None)
            {
                const auto index = mState->first; auto& slot = mState->slots[index];
                if (!mState->continuous.contains(index))
                    result.push_back({{index, slot.generation}, slot.reasons});
                mState->unlink(index); slot.reasons = 0;
            }
            // Clear before callbacks run. Notifications raised while processing
            // this snapshot remain queued for the next drain, never overwritten.
            return result;
        }
        std::size_t size() const
        { std::lock_guard lock(mState->mutex); return mState->live; }
        std::size_t continuousSize() const
        { std::lock_guard lock(mState->mutex); return mState->continuous.size(); }
    private:
        std::shared_ptr<State> mState;
        std::size_t mCapacity;
    };
}
#endif
