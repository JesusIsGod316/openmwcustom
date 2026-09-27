#ifndef OPENMW_SCENEUTIL_RENDERMUTATION_H
#define OPENMW_SCENEUTIL_RENDERMUTATION_H

#include <atomic>
#include <functional>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <vector>

namespace SceneUtil
{
    // Opt-in contract for engine-owned callbacks. Unknown callbacks MUST remain
    // on the evaluated compatibility path. A supported callback may only mutate
    // the declared rendering fields, never topology or unreported mesh arrays.
    enum RenderMutation : unsigned
    {
        RenderUntracked = 1u,
        RenderTransform = 2u,
        RenderVisibility = 4u,
        RenderMaterial = 8u,
        RenderTexCoords = 16u,
    };

    class RenderMutationSource
    {
    public:
        // Update-thread subscription. No scene pointers escape to rendering workers.
        // A destroyed source invalidates bindings before they can be dereferenced.
        struct Subscription
        {
            std::atomic_bool changed{true}, invalidated{false};
            // Bound once before subscribing; only enqueues an owner token. It
            // never inspects nodes or invokes rendering during a mutation.
            std::function<void()> wake;
            void notify(bool invalid = false) noexcept
            {
                changed.store(true, std::memory_order_release);
                if (invalid) invalidated.store(true, std::memory_order_release);
                if (wake) wake();
            }
        };
        virtual ~RenderMutationSource()
        {
            for (auto& weak : mSubscriptions)
                if (auto observer = weak.lock()) observer->notify(true);
        }
        void subscribeRenderMutations(const std::shared_ptr<Subscription>& observer)
        {
            std::erase_if(mSubscriptions, [](const auto& weak) { return weak.expired(); });
            for (const auto& weak : mSubscriptions)
                if (weak.lock() == observer) return;
            mSubscriptions.push_back(observer);
            mWatched = true;
        }
        virtual unsigned renderMutationMask() const noexcept { return RenderUntracked; }
        void watchRenderMutations() noexcept { mWatched = true; }
        std::uint64_t renderMutationRevision() const noexcept { return mRevision; }
    protected:
        // Only subscribed engine objects pay even the increment. Copied
        // callbacks have a fresh subscription; no observer or frame is shared.
        RenderMutationSource() = default;
        RenderMutationSource(const RenderMutationSource&) {}
        void publishRenderMutation() noexcept
        {
            if (!mWatched) return;
            ++mRevision;
            for (auto& weak : mSubscriptions)
                if (auto observer = weak.lock())
                {
                    observer->notify((renderMutationMask() & RenderUntracked) != 0);
                }
        }
        void invalidateRenderMutationBindings() noexcept
        {
            for (auto& weak : mSubscriptions)
                if (auto observer = weak.lock()) observer->notify(true);
        }
    private:
        bool mWatched = false;
        std::uint64_t mRevision = 0;
        std::vector<std::weak_ptr<Subscription>> mSubscriptions;
    };
}
#endif
