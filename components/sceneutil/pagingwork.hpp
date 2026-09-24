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
        explicit PagingWorkScope(const std::atomic<bool>* cancel) noexcept
            : mPrevious(sCancel) { sCancel = cancel; }
        ~PagingWorkScope() { sCancel = mPrevious; }
        PagingWorkScope(const PagingWorkScope&) = delete;
        PagingWorkScope& operator=(const PagingWorkScope&) = delete;
        static bool active() noexcept { return sCancel != nullptr; }
        static bool cancelled() noexcept { return sCancel && sCancel->load(std::memory_order_relaxed); }
        static void checkpoint() { if (cancelled()) throw PagingWorkCancelled{}; }
    private:
        const std::atomic<bool>* mPrevious;
        inline static thread_local const std::atomic<bool>* sCancel = nullptr;
    };
    // Only call on privately constructed paging geometry, before publication.
    // Keeps the original vertex arrays/bindings, creates replacement indices.
    void optimizePagingVertices(osg::Node& node);
}
#endif
