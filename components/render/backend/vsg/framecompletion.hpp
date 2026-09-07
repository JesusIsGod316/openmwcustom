#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMECOMPLETION_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_FRAMECOMPLETION_H

#include <components/rendercore/resources.hpp>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace RenderVsg
{
    // Backend-local submission/completion timeline. It deliberately models
    // completion separately from logical FrameId publication: retirement is
    // legal only after the backend has observed the corresponding fence/timeline
    // completion, never merely because a newer CPU frame was produced.
    class FrameCompletionTracker
    {
    public:
        explicit FrameCompletionTracker(std::size_t maximumFramesInFlight = 3)
            : mMaximumFramesInFlight(maximumFramesInFlight)
        {
        }

        [[nodiscard]] bool submit(RenderCore::FrameId frame)
        {
            if (!frame.valid() || mMaximumFramesInFlight == 0 || atCapacity()
                || (mLastSubmitted && frame <= *mLastSubmitted))
                return false;
            mSubmitted.push_back(frame);
            mLastSubmitted = frame;
            return true;
        }

        [[nodiscard]] bool completeThrough(RenderCore::FrameId frame)
        {
            if (!frame.valid() || !mLastSubmitted || frame > *mLastSubmitted || (mLastCompleted && frame < *mLastCompleted))
                return false;
            while (!mSubmitted.empty() && mSubmitted.front() <= frame)
                mSubmitted.pop_front();
            mLastCompleted = frame;
            return true;
        }

        [[nodiscard]] bool atCapacity() const noexcept { return mSubmitted.size() >= mMaximumFramesInFlight; }
        [[nodiscard]] std::size_t inFlightCount() const noexcept { return mSubmitted.size(); }
        [[nodiscard]] std::optional<RenderCore::FrameId> lastSubmitted() const noexcept { return mLastSubmitted; }
        [[nodiscard]] std::optional<RenderCore::FrameId> lastCompleted() const noexcept { return mLastCompleted; }

    private:
        std::size_t mMaximumFramesInFlight = 0;
        std::deque<RenderCore::FrameId> mSubmitted;
        std::optional<RenderCore::FrameId> mLastSubmitted;
        std::optional<RenderCore::FrameId> mLastCompleted;
    };

    template <class Object>
    class FrameRetirementQueue
    {
    public:
        [[nodiscard]] bool queue(RenderCore::FrameId lastUseFrame, Object object)
        {
            if (!lastUseFrame.valid())
                return false;
            mEntries.push_back(Entry{ lastUseFrame, std::move(object) });
            return true;
        }

        // Returning retired objects lets the owner perform any required final
        // backend bookkeeping before their destructors release VSG/Vulkan state.
        [[nodiscard]] std::vector<Object> collect(RenderCore::FrameId completedFrame)
        {
            std::vector<Object> retired;
            if (!completedFrame.valid())
                return retired;
            for (auto it = mEntries.begin(); it != mEntries.end();)
            {
                if (it->lastUseFrame > completedFrame)
                {
                    ++it;
                    continue;
                }
                retired.push_back(std::move(it->object));
                it = mEntries.erase(it);
            }
            return retired;
        }

        [[nodiscard]] std::size_t size() const noexcept { return mEntries.size(); }

    private:
        struct Entry
        {
            RenderCore::FrameId lastUseFrame;
            Object object;
        };
        std::vector<Entry> mEntries;
    };
}

#endif
