#ifndef OPENMW_SCENEUTIL_PAGINGWORK_H
#define OPENMW_SCENEUTIL_PAGINGWORK_H

#include <atomic>
#include <exception>

namespace osg { class Node; }
namespace SceneUtil
{
    class PagingWorkCancelled final : public std::exception
    {
    public:
        const char* what() const noexcept override { return "Obsolete paging preparation cancelled"; }
    };
    // Scoped to a terrain WORK ITEM. No setting reads or graph metadata on
    // render traversal; a disabled scope preserves the inherited optimizer.
    class PagingWorkScope
    {
    public:
        enum class Phase
        {
            Full,
            RequiredReadiness,
            OptionalOptimization
        };

        explicit PagingWorkScope(const std::atomic<bool>* cancel, Phase phase = Phase::Full) noexcept
            : mPrevious(sCancel)
            , mPreviousPhase(sPhase)
        {
            sCancel = cancel;
            sPhase = phase;
        }
        ~PagingWorkScope()
        {
            sCancel = mPrevious;
            sPhase = mPreviousPhase;
        }
        PagingWorkScope(const PagingWorkScope&) = delete;
        PagingWorkScope& operator=(const PagingWorkScope&) = delete;
        static bool active() noexcept { return sCancel != nullptr; }
        static bool cancelled() noexcept { return sCancel && sCancel->load(std::memory_order_relaxed); }
        static Phase phase() noexcept { return sPhase; }
        static bool requiredReadiness() noexcept { return active() && sPhase == Phase::RequiredReadiness; }
        static bool optionalOptimization() noexcept { return active() && sPhase == Phase::OptionalOptimization; }
        static void checkpoint() { if (cancelled()) throw PagingWorkCancelled{}; }
    private:
        const std::atomic<bool>* mPrevious;
        Phase mPreviousPhase;
        inline static thread_local const std::atomic<bool>* sCancel = nullptr;
        inline static thread_local Phase sPhase = Phase::Full;
    };
    // Only call on privately constructed paging geometry, before publication.
    // Keeps the original vertex arrays/bindings, creates replacement indices.
    void optimizePagingVertices(osg::Node& node);
}
#endif
