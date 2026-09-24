#ifndef OPENMW_SCENEUTIL_RENDERMUTATION_H
#define OPENMW_SCENEUTIL_RENDERMUTATION_H

#include <cstdint>

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
        virtual ~RenderMutationSource() = default;
        virtual unsigned renderMutationMask() const noexcept { return RenderUntracked; }
        void watchRenderMutations() noexcept { mWatched = true; }
        std::uint64_t renderMutationRevision() const noexcept { return mRevision; }
    protected:
        // Only subscribed engine objects pay even the increment. Copied
        // callbacks have a fresh subscription; no observer or frame is shared.
        RenderMutationSource() = default;
        RenderMutationSource(const RenderMutationSource&) {}
        void publishRenderMutation() noexcept { if (mWatched) ++mRevision; }
    private:
        bool mWatched = false;
        std::uint64_t mRevision = 0;
    };
}
#endif
