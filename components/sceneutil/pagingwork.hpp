#ifndef OPENMW_SCENEUTIL_PAGINGWORK_H
#define OPENMW_SCENEUTIL_PAGINGWORK_H

#include <atomic>
#include <exception>
#include <cstdint>

namespace osg { class Node; }
namespace SceneUtil
{
    struct ExactPagingWorkCounters
    {
        inline static std::atomic_uint64_t runs{0}, triangles{0}, scoreUpdates{0}, scoreReuses{0};
        inline static std::atomic_uint64_t heapComparisons{0}, unchangedHeapUpdates{0};
        inline static std::atomic_uint64_t scoreTableRuns{0}, scoreTableHits{0};
        inline static std::atomic_uint64_t scoreTableComputations{0}, scoreTableUncached{0};
    };
    // Configured once before paging workers start; independent of triangle
    // rescoring/heap reduction and never enabled by changing a draw policy.
    inline std::atomic_bool reusePagingScoreValues{false};
    class PagingWorkCancelled final : public std::exception
    {
    public:
        const char* what() const noexcept override { return "Obsolete paging preparation cancelled"; }
    };
    // A required private view may be invalidated by a concurrent source/shader
    // recipe change while a loading listener renders. It must be rebuilt in
    // full before publication, rather than treating that epoch change as an
    // actual abort. No other cancellation or exception authorizes a retry.
    inline constexpr unsigned RequiredPagingMaximumAttempts = 3;
    template <class ReadEpoch, class Prepare, class Retried>
    bool prepareFreshRequiredPaging(const std::atomic<bool>& abort, ReadEpoch&& readEpoch,
        Prepare&& prepare, Retried&& retried)
    {
        for (unsigned attempt = 0; attempt < RequiredPagingMaximumAttempts; ++attempt)
        {
            if (abort.load(std::memory_order_acquire)) return false;
            const auto before = readEpoch(); // empty: required world no longer matches
            if (!before) return false;
            try
            {
                prepare(); // caller resets every private view and establishes a fresh required scope
                return !abort.load(std::memory_order_acquire);
            }
            catch (const PagingWorkCancelled&)
            {
                const auto after = readEpoch();
                if (abort.load(std::memory_order_acquire) || !after || *after == *before
                    || attempt + 1 == RequiredPagingMaximumAttempts)
                    return false;
                retried(*before, *after, attempt + 1);
            }
        }
        return false;
    }
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

        struct Context
        {
            const std::atomic<bool>* cancel = nullptr;
            Phase phase = Phase::Full;
        };

        explicit PagingWorkScope(const std::atomic<bool>* cancel, Phase phase = Phase::Full) noexcept
            : PagingWorkScope(Context{ cancel, phase })
        {
        }
        explicit PagingWorkScope(Context context) noexcept
            : mPrevious(sCancel)
            , mPreviousPhase(sPhase)
        {
            sCancel = context.cancel;
            sPhase = context.phase;
        }
        ~PagingWorkScope()
        {
            sCancel = mPrevious;
            sPhase = mPreviousPhase;
        }
        PagingWorkScope(const PagingWorkScope&) = delete;
        PagingWorkScope& operator=(const PagingWorkScope&) = delete;
        static bool active() noexcept { return sCancel != nullptr; }
        static Context capture() noexcept { return Context{ sCancel, sPhase }; }
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
