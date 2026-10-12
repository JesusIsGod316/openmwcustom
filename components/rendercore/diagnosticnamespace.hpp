#ifndef OPENMW_RENDERCORE_DIAGNOSTICNAMESPACE_H
#define OPENMW_RENDERCORE_DIAGNOSTICNAMESPACE_H

#include "diagnosticinputborrow.hpp"

#include <atomic>
#include <limits>
#include <memory>

namespace RenderCore::Temporal
{
    // This CPU contract does not inspect GL, SDL or OSG. The actual native
    // context owner must issue it after creation and provide an observation
    // made while that context is current. Observing a familiar handle is not
    // authority to create, refresh or revive a namespace.
    struct DiagnosticNativeObservation
    {
        std::uintptr_t nativeContext = 0;
        unsigned osgContext = 0;
    };
    enum class DiagnosticNamespacePhase { Live, Closing, Destroyed, Expired };
    enum class DiagnosticNamespaceStatus
    {
        Executed, Busy, Closing, Destroyed, Expired, WrongContext, WrongThread,
        InvalidIdentity, StalePublication, MissingResources, MetadataRejected,
        PendingRetirement, InvalidResourceEpoch, NativeDestroyFailed
    };
    struct DiagnosticNamespaceSnapshot
    {
        std::uint64_t epoch = 0;
        std::uintptr_t nativeContext = 0;
        unsigned osgContext = 0;
        DiagnosticNamespacePhase phase = DiagnosticNamespacePhase::Expired;
        bool executing = false, authorityPresent = false, retirementComplete = false;
        std::uint64_t knownBytes = 0, knownObjects = 0, abandonedBytes = 0, abandonedObjects = 0;
        std::uint64_t executions = 0, publications = 0, destructionFailures = 0;
    };
    struct DiagnosticPublicationResult
    {
        DiagnosticNamespaceStatus status = DiagnosticNamespaceStatus::MetadataRejected;
        DiagnosticBorrowStatus metadata = DiagnosticBorrowStatus::MissingPublication;
    };

    namespace DiagnosticNamespaceDetail
    {
        inline std::atomic<std::uint64_t> nextEpoch{1};
        inline std::uint64_t issueEpoch() noexcept
        {
            auto next = nextEpoch.load(std::memory_order_relaxed);
            while (next != std::numeric_limits<std::uint64_t>::max())
            {
                if (nextEpoch.compare_exchange_weak(next, next + 1, std::memory_order_relaxed))
                    return next;
            }
            return 0; // Exhaustion is terminal; never wrap into a reused epoch.
        }
        struct Control
        {
            Control(DiagnosticNativeObservation observed, std::uint64_t issued)
                : native(observed.nativeContext), osg(observed.osgContext), epoch(issued) {}
            const std::uintptr_t native;
            const unsigned osg;
            const std::uint64_t epoch;
            // One atomic word linearizes closure against nonwaiting admission.
            // 0/1 live idle/active; 2/3 closing idle/active; 4 destroyed.
            std::atomic<unsigned> state{0};
            std::atomic<bool> authorityPresent{true}, retirementComplete{true};
            std::atomic<std::uint64_t> bytes{0}, objects{0}, abandonedBytes{0}, abandonedObjects{0};
            std::atomic<std::uint64_t> executions{0}, publications{0}, destructionFailures{0};
            // Access only in the one execution scope, on that scope's thread.
            std::uint64_t executionSerial = 0, expectedResourceEpoch = 0;
            std::optional<DiagnosticDrawIdentity> lastPublication;
            std::uint64_t publisher = 0, historyOwner = 0, lastCapture = 0, largestSourceGroup = 0;
            std::array<std::uint64_t,2> sourceGroups{};
            bool publicationActive = false;
        };
        inline bool matches(const Control& state, DiagnosticNativeObservation observation) noexcept
        {
            return observation.nativeContext && observation.nativeContext == state.native
                && observation.osgContext == state.osg;
        }
        inline DiagnosticNamespaceStatus close(Control& control) noexcept
        {
            auto state = control.state.load(std::memory_order_acquire);
            for (;;)
            {
                if (state == 4) return DiagnosticNamespaceStatus::Destroyed;
                if (state >= 2)
                    return state == 3 ? DiagnosticNamespaceStatus::Busy : DiagnosticNamespaceStatus::Closing;
                if (control.state.compare_exchange_weak(state, state | 2, std::memory_order_acq_rel))
                    return (state & 1) ? DiagnosticNamespaceStatus::Busy : DiagnosticNamespaceStatus::Closing;
            }
        }
        inline DiagnosticNamespaceSnapshot snapshot(const Control& control) noexcept
        {
            const unsigned state = control.state.load(std::memory_order_acquire);
            DiagnosticNamespaceSnapshot result;
            result.epoch = control.epoch;
            result.nativeContext = control.native;
            result.osgContext = control.osg;
            result.phase = state == 4 ? DiagnosticNamespacePhase::Destroyed
                : state >= 2 ? DiagnosticNamespacePhase::Closing : DiagnosticNamespacePhase::Live;
            result.executing = state & 1;
            result.authorityPresent = control.authorityPresent.load(std::memory_order_acquire);
            result.retirementComplete = control.retirementComplete.load(std::memory_order_acquire);
            result.knownBytes = control.bytes.load(std::memory_order_relaxed);
            result.knownObjects = control.objects.load(std::memory_order_relaxed);
            result.abandonedBytes = control.abandonedBytes.load(std::memory_order_relaxed);
            result.abandonedObjects = control.abandonedObjects.load(std::memory_order_relaxed);
            result.executions = control.executions.load(std::memory_order_relaxed);
            result.publications = control.publications.load(std::memory_order_relaxed);
            result.destructionFailures = control.destructionFailures.load(std::memory_order_relaxed);
            return result; // A monitoring snapshot, not a multi-field allocation transaction.
        }
    }

    class DiagnosticNamespace;
    class DiagnosticNamespaceObserver;
    class DiagnosticNamespaceExecution final
    {
    public:
        DiagnosticNamespaceExecution(const DiagnosticNamespaceExecution&) = delete;
        DiagnosticNamespaceExecution(DiagnosticNamespaceExecution&&) = delete;
        DiagnosticNamespaceExecution& operator=(const DiagnosticNamespaceExecution&) = delete;
        DiagnosticNamespaceExecution& operator=(DiagnosticNamespaceExecution&&) = delete;

        bool current(DiagnosticNativeObservation observed) const noexcept
        {
            return onThread() && (mControl.state.load(std::memory_order_acquire) & 1)
                && mSerial == mControl.executionSerial && DiagnosticNamespaceDetail::matches(mControl, observed);
        }
        std::uint64_t namespaceEpoch() const noexcept { return mControl.epoch; }
        bool admissionOpen() const noexcept
        {
            return onThread() && mControl.state.load(std::memory_order_acquire) == 1;
        }
        // The real producer/allocation owner supplies its already issued epoch.
        // This binds checks to that value; it does not certify GL storage or
        // manufacture TemporalMotion's resourceEpoch from a telemetry counter.
        DiagnosticNamespaceStatus bindProducerResourceEpoch(std::uint64_t epoch) const noexcept
        {
            if (!onThread()) return DiagnosticNamespaceStatus::WrongThread;
            if (!admissionOpen()) return DiagnosticNamespaceStatus::Closing;
            if (mControl.publicationActive) return DiagnosticNamespaceStatus::Busy;
            if (!epoch || epoch < mControl.expectedResourceEpoch)
                return DiagnosticNamespaceStatus::InvalidResourceEpoch;
            mControl.expectedResourceEpoch = epoch;
            return DiagnosticNamespaceStatus::Executed;
        }
        // Explicit owner transitions, not observation of a convenient GL name
        // or a telemetry-issued epoch. Keep this metadata contract GL-free.
        // Real producer/History factories supply their already-issued IDs.
        DiagnosticNamespaceStatus registerProducerHistory(std::uint64_t publisher, std::uint64_t history) const noexcept
        {
            if (!onThread()) return DiagnosticNamespaceStatus::WrongThread;
            if (!admissionOpen() || mRetirement) return DiagnosticNamespaceStatus::Closing;
            if (mControl.publicationActive) return DiagnosticNamespaceStatus::Busy;
            if (!publisher || !history || publisher<mControl.publisher || history<mControl.historyOwner)
                return DiagnosticNamespaceStatus::InvalidIdentity;
            if (mControl.lastPublication && !mControl.lastPublication->publisher)
                return DiagnosticNamespaceStatus::InvalidIdentity; // No midstream legacy downgrade/upgrade.
            if (publisher==mControl.publisher && history==mControl.historyOwner)
                return DiagnosticNamespaceStatus::Executed; // Never reset a live watermark by re-registering it.
            if (mControl.publisher && publisher!=mControl.publisher) mControl.sourceGroups={};
            mControl.publisher=publisher; mControl.historyOwner=history;
            mControl.expectedResourceEpoch=0; // The new History must bind its own actual coarse reset epoch.
            // lastCapture and largestSourceGroup survive owner/ticket reset.
            return DiagnosticNamespaceStatus::Executed;
        }
        DiagnosticNamespaceStatus registerSourceGroup(unsigned slot,std::uint64_t generation) const noexcept
        {
            if (!onThread()) return DiagnosticNamespaceStatus::WrongThread;
            if (!admissionOpen() || mRetirement) return DiagnosticNamespaceStatus::Closing;
            if (mControl.publicationActive) return DiagnosticNamespaceStatus::Busy;
            if (!mControl.publisher || !mControl.historyOwner || slot>=mControl.sourceGroups.size() || !generation)
                return DiagnosticNamespaceStatus::InvalidIdentity;
            if (mControl.sourceGroups[slot]==generation) return DiagnosticNamespaceStatus::Executed;
            if (generation<=mControl.largestSourceGroup) return DiagnosticNamespaceStatus::InvalidResourceEpoch;
            mControl.sourceGroups[slot]=generation; mControl.largestSourceGroup=generation;
            return DiagnosticNamespaceStatus::Executed;
        }
        // Exact backend-reported logical charges only. Pending fences/objects
        // must remain charged. These values are neither measured VRAM nor a
        // process memory ceiling, and changing them does not delete resources.
        bool setKnownResources(std::uint64_t bytes, std::uint64_t objects) const noexcept
        {
            if (!onThread() || !(mControl.state.load(std::memory_order_acquire) & 1)) return false;
            mControl.bytes.store(bytes, std::memory_order_relaxed);
            mControl.objects.store(objects, std::memory_order_relaxed);
            mControl.retirementComplete.store(!bytes && !objects, std::memory_order_release);
            return true;
        }
        template <class Consumer>
        DiagnosticPublicationResult withPublication(DiagnosticNativeObservation observed,
            const DiagnosticSources& sources, Consumer&& consumer, DiagnosticBorrowLimits limits = {}) const
        {
            static_assert(std::is_void_v<std::invoke_result_t<Consumer, const DiagnosticInputBorrow&>>,
                "A diagnostic publication callback must not return an image or borrow");
            if (!onThread()) return {DiagnosticNamespaceStatus::WrongThread};
            if (!current(observed)) return {DiagnosticNamespaceStatus::WrongContext};
            if (!admissionOpen() || mRetirement) return {DiagnosticNamespaceStatus::Closing};
            if (mControl.publicationActive) return {DiagnosticNamespaceStatus::Busy};
            const auto& id = sources.identity;
            if (id.nativeContext != mControl.native || id.osgContext != mControl.osg || id.contextEpoch != mControl.epoch)
                return {DiagnosticNamespaceStatus::InvalidIdentity};
            if (!mControl.expectedResourceEpoch) return {DiagnosticNamespaceStatus::MissingResources};
            if (id.resourceEpoch != mControl.expectedResourceEpoch)
                return {DiagnosticNamespaceStatus::InvalidResourceEpoch};
            const bool acquired=mControl.publisher!=0;
            if (acquired)
            {
                if (id.publisher!=mControl.publisher || id.historyOwner!=mControl.historyOwner || !id.capture
                    || id.sourceSlot>=mControl.sourceGroups.size() || !id.sourceGroup
                    || id.sourceGroup!=mControl.sourceGroups[id.sourceSlot])
                    return {DiagnosticNamespaceStatus::InvalidIdentity};
                if (id.capture<=mControl.lastCapture) return {DiagnosticNamespaceStatus::StalePublication};
            }
            else if (id.publisher || id.historyOwner || id.capture || id.sourceGroup || id.sourceSlot)
                return {DiagnosticNamespaceStatus::InvalidIdentity};
            if (mControl.lastPublication && (!acquired || id.historyOwner==mControl.lastPublication->historyOwner))
            {
                const auto& last = *mControl.lastPublication;
                if (id.submission <= last.submission || id.ticket <= last.ticket || id.frame <= last.frame
                    || id.resourceEpoch < last.resourceEpoch || id.motionRevision < last.motionRevision
                    || (id.view != last.view && id.resourceEpoch == last.resourceEpoch))
                    return {DiagnosticNamespaceStatus::StalePublication};
            }
            // The short-lived metadata owner is created on this callback's
            // thread. The persistent watermark above is not reset by handoff,
            // scope retirement, a local owner rebuild or resource replacement.
            DiagnosticInputBorrowOwner metadata(limits);
            const auto published = metadata.publish(sources);
            if (published != DiagnosticBorrowStatus::Observed)
                return {DiagnosticNamespaceStatus::MetadataRejected, published};
            if (!admissionOpen()) return {DiagnosticNamespaceStatus::Closing};
            mControl.publicationActive = true;
            struct EndPublication
            {
                bool& active;
                ~EndPublication() { active = false; }
            } endPublication{mControl.publicationActive};
            mControl.lastPublication = id;
            if (acquired) mControl.lastCapture=id.capture;
            mControl.publications.fetch_add(1, std::memory_order_relaxed);
            const DiagnosticBorrowRequest request{id, sources.images[0].width, sources.images[0].height};
            const auto borrowed = metadata.withBorrow(request, std::forward<Consumer>(consumer));
            return {DiagnosticNamespaceStatus::Executed, borrowed};
        }
    private:
        friend class DiagnosticNamespace;
        friend class DiagnosticNamespaceObserver;
        DiagnosticNamespaceExecution(DiagnosticNamespaceDetail::Control& control, bool retirement)
            : mControl(control), mSerial(++control.executionSerial), mRetirement(retirement) {}
        ~DiagnosticNamespaceExecution() noexcept
        {
            // No GL call, fixed creation-thread requirement or terminate. The
            // scope is valid only inside its synchronous callback; as with the
            // existing borrow, a caller must not capture a raw reference for
            // later use. Closure requested concurrently must remain closed.
            mControl.state.fetch_and(~1u, std::memory_order_release);
        }
        bool onThread() const noexcept { return std::this_thread::get_id() == mThread; }
        DiagnosticNamespaceDetail::Control& mControl;
        const std::uint64_t mSerial;
        const bool mRetirement;
        const std::thread::id mThread = std::this_thread::get_id();
    };

    class DiagnosticNamespaceObserver final
    {
    public:
        DiagnosticNamespaceSnapshot snapshot() const noexcept
        {
            const auto control = mControl.lock();
            return control ? DiagnosticNamespaceDetail::snapshot(*control) : DiagnosticNamespaceSnapshot{};
        }
        template <class Consumer>
        DiagnosticNamespaceStatus withExecution(DiagnosticNativeObservation observed, Consumer&& consumer) const
        {
            const auto control = mControl.lock();
            if (!control) return DiagnosticNamespaceStatus::Expired;
            return execute(*control, observed, false, std::forward<Consumer>(consumer));
        }
    private:
        friend class DiagnosticNamespace;
        explicit DiagnosticNamespaceObserver(const std::shared_ptr<DiagnosticNamespaceDetail::Control>& control)
            : mControl(control) {}
        template <class Consumer>
        static DiagnosticNamespaceStatus execute(DiagnosticNamespaceDetail::Control& control,
            DiagnosticNativeObservation observed, bool retirement, Consumer&& consumer)
        {
            static_assert(std::is_void_v<std::invoke_result_t<Consumer, const DiagnosticNamespaceExecution&>>,
                "An execution callback must not return a guard or resource");
            if (!DiagnosticNamespaceDetail::matches(control, observed)) return DiagnosticNamespaceStatus::WrongContext;
            unsigned idle = retirement ? 2u : 0u;
            if (!control.state.compare_exchange_strong(idle, idle | 1, std::memory_order_acq_rel))
            {
                if (idle == 4) return DiagnosticNamespaceStatus::Destroyed;
                if (idle & 1) return DiagnosticNamespaceStatus::Busy;
                return DiagnosticNamespaceStatus::Closing;
            }
            if (control.executionSerial == std::numeric_limits<std::uint64_t>::max())
            {
                control.state.store(2, std::memory_order_release);
                return DiagnosticNamespaceStatus::Closing;
            }
            const DiagnosticNamespaceExecution scope(control, retirement);
            control.executions.fetch_add(1, std::memory_order_relaxed);
            std::forward<Consumer>(consumer)(scope);
            return DiagnosticNamespaceStatus::Executed;
        }
        std::weak_ptr<DiagnosticNamespaceDetail::Control> mControl;
    };

    // One authority per successfully created native namespace, owned by its
    // SDL/platform owner. Destruction freezes admission but never pretends that
    // a noncurrent C++ destructor performed GL cleanup or destroyed a context.
    class DiagnosticNamespace final
    {
    public:
        static std::optional<DiagnosticNamespace> issue(DiagnosticNativeObservation createdCurrent)
        {
            if (!createdCurrent.nativeContext) return std::nullopt;
            const auto epoch = DiagnosticNamespaceDetail::issueEpoch();
            if (!epoch) return std::nullopt;
            return DiagnosticNamespace(std::make_shared<DiagnosticNamespaceDetail::Control>(createdCurrent, epoch));
        }
        DiagnosticNamespace(const DiagnosticNamespace&) = delete;
        DiagnosticNamespace& operator=(const DiagnosticNamespace&) = delete;
        DiagnosticNamespace(DiagnosticNamespace&& other) noexcept : mControl(std::move(other.mControl)) {}
        DiagnosticNamespace& operator=(DiagnosticNamespace&&) = delete;
        ~DiagnosticNamespace() noexcept
        {
            if (!mControl) return;
            DiagnosticNamespaceDetail::close(*mControl);
            mControl->authorityPresent.store(false, std::memory_order_release);
        }
        DiagnosticNamespaceObserver observer() const noexcept { return DiagnosticNamespaceObserver(mControl); }
        DiagnosticNamespaceSnapshot snapshot() const noexcept { return observer().snapshot(); }
        DiagnosticNamespaceStatus beginClose() noexcept
        {
            return mControl ? DiagnosticNamespaceDetail::close(*mControl) : DiagnosticNamespaceStatus::Expired;
        }
        template <class Consumer>
        DiagnosticNamespaceStatus withRetirement(DiagnosticNativeObservation current, Consumer&& consumer)
        {
            const auto control = mControl;
            if (!control) return DiagnosticNamespaceStatus::Expired;
            if (control->state.load(std::memory_order_acquire) == 0) return DiagnosticNamespaceStatus::Closing;
            return DiagnosticNamespaceObserver::execute(*control, current, true, std::forward<Consumer>(consumer));
        }
        // The real platform destroy runs inside the same exclusive reservation
        // as this state transition, after current-context backend retirement.
        // This prevents a post-hoc receipt racing another retirement scope.
        // A callback's true result must be the actual native API result; this
        // CPU helper does not inspect SDL or establish current GL proof.
        template <class Destroy>
        DiagnosticNamespaceStatus withNativeDestroy(Destroy&& destroy) noexcept
        {
            static_assert(std::is_same_v<std::invoke_result_t<Destroy>, bool>,
                "The native destroy callback must return the platform operation's bool result");
            const auto control = mControl;
            if (!control) return DiagnosticNamespaceStatus::Expired;
            unsigned state = control->state.load(std::memory_order_acquire);
            if (state == 4) return DiagnosticNamespaceStatus::Destroyed;
            if (state & 1) return DiagnosticNamespaceStatus::Busy;
            if (state != 2) return DiagnosticNamespaceStatus::Closing;
            if (!control->state.compare_exchange_strong(state, 3, std::memory_order_acq_rel))
                return state == 4 ? DiagnosticNamespaceStatus::Destroyed : DiagnosticNamespaceStatus::Busy;
            struct EndDestroy
            {
                DiagnosticNamespaceDetail::Control& control;
                ~EndDestroy() { control.state.fetch_and(~1u, std::memory_order_release); }
            } endDestroy{*control};
            if (!control->retirementComplete.load(std::memory_order_acquire))
                return DiagnosticNamespaceStatus::PendingRetirement;
            try
            {
                if (std::forward<Destroy>(destroy)())
                {
                    control->state.store(4, std::memory_order_release);
                    return DiagnosticNamespaceStatus::Destroyed;
                }
            }
            catch (...) {} // Keep the closed, accounted state; no destruction credit.
            control->destructionFailures.fetch_add(1, std::memory_order_relaxed);
            return DiagnosticNamespaceStatus::NativeDestroyFailed;
        }
        DiagnosticNamespaceStatus noteNativeDestroyFailure() noexcept
        {
            if (!mControl) return DiagnosticNamespaceStatus::Expired;
            if (beginClose() == DiagnosticNamespaceStatus::Destroyed) return DiagnosticNamespaceStatus::Destroyed;
            mControl->destructionFailures.fetch_add(1, std::memory_order_relaxed);
            return DiagnosticNamespaceStatus::NativeDestroyFailed;
        }
        // Only for externally confirmed namespace loss/death. This records
        // abandoned logical charges, not deletion, recovered RAM or freed VRAM.
        // The caller must never delete these old numeric names in a replacement.
        DiagnosticNamespaceStatus confirmNativeLost() noexcept
        {
            if (!mControl) return DiagnosticNamespaceStatus::Expired;
            beginClose();
            unsigned state = 2;
            if (!mControl->state.compare_exchange_strong(state, 4, std::memory_order_acq_rel))
                return state == 4 ? DiagnosticNamespaceStatus::Destroyed : DiagnosticNamespaceStatus::Busy;
            mControl->abandonedBytes.fetch_add(mControl->bytes.exchange(0), std::memory_order_relaxed);
            mControl->abandonedObjects.fetch_add(mControl->objects.exchange(0), std::memory_order_relaxed);
            return DiagnosticNamespaceStatus::Destroyed;
        }
    private:
        explicit DiagnosticNamespace(std::shared_ptr<DiagnosticNamespaceDetail::Control> control)
            : mControl(std::move(control)) {}
        std::shared_ptr<DiagnosticNamespaceDetail::Control> mControl;
    };
}
#endif
