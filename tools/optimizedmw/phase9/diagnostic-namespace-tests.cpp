#include "../../../components/rendercore/diagnosticnamespace.hpp"

#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace RenderCore::Temporal;
namespace
{
    unsigned checks = 0, cases = 0;
    void require(bool passed, const char* message)
    {
        ++checks;
        if (!passed) throw std::runtime_error(message);
    }
    void test(const char* name, const auto& run)
    {
        run();
        ++cases;
        std::cout << "PASS " << name << '\n';
    }
    DiagnosticSources publication(std::uint64_t namespaceEpoch, std::uint64_t submission = 1,
        std::uint64_t ticket = 71, std::uint64_t frame = 0, std::uint64_t resource = 12)
    {
        DiagnosticSources result;
        result.identity = {submission, ticket, frame, 51, 0x4567, namespaceEpoch, resource, 9, 0};
        for (unsigned i = 0; i < 3; ++i)
        {
            result.images[i] = {101 + i, 34 + i, frame, 51, 0x4567, namespaceEpoch, 64, 32,
                i == 0 ? DiagnosticFormat::Rgba8 : i == 1 ? DiagnosticFormat::Depth32Float : DiagnosticFormat::Rg16Float};
        }
        result.colorStage = DiagnosticColorStage::SceneBeforePostFx;
        result.coverage = DiagnosticMotionCoverage::PartialOpaque;
        result.motionSpace = DiagnosticMotionSpace::CurrentToPreviousRenderPixels;
        result.depthRange = DiagnosticDepthRange::ZeroToOne;
        result.producerSubmitted = true;
        return result;
    }
    const DiagnosticNativeObservation correct{0x4567, 0};
    const DiagnosticNativeObservation wrongNative{0x9876, 0}, wrongOsg{0x4567, 3};
    auto create() { return DiagnosticNamespace::issue(correct); }
    auto emptyConsumer = [](const DiagnosticNamespaceExecution&) {};
    auto ignoreBorrow = [](const DiagnosticInputBorrow& borrow)
    {
        require(borrow.sources() != nullptr, "Synchronous draw sees its metadata borrow");
    };
}

int main()
{
    try
    {
        test("owner-issued identities and missing native namespace", []
        {
            require(!DiagnosticNamespace::issue({0, 0}), "Zero native handle cannot issue a namespace");
            auto first = create(), next = create();
            require(first && next, "Real owner issuance succeeds");
            require(first->snapshot().epoch && next->snapshot().epoch > first->snapshot().epoch,
                "Numeric native and OSG handle reuse receives a distinct monotonic epoch");
            require(first->snapshot().osgContext == 0, "OSG context zero remains valid");
            require(first->snapshot().phase == DiagnosticNamespacePhase::Live,
                "Issued namespace initially live");
            require(!first->snapshot().executing && first->snapshot().authorityPresent,
                "Issued namespace has authority but no permanent creation-thread lease");
        });
        test("native and OSG observation must match before callback", []
        {
            auto owner = create();
            unsigned called = 0;
            auto consumer = [&](const DiagnosticNamespaceExecution&) { ++called; };
            require(owner->observer().withExecution(wrongNative, consumer) == DiagnosticNamespaceStatus::WrongContext,
                "Wrong native handle fails closed");
            require(owner->observer().withExecution(wrongOsg, consumer) == DiagnosticNamespaceStatus::WrongContext,
                "Wrong OSG association fails closed");
            require(owner->observer().withExecution({0, 0}, consumer) == DiagnosticNamespaceStatus::WrongContext,
                "Missing current native context fails closed");
            require(!called && !owner->snapshot().executions, "Wrong observation cannot enter callback");
        });
        test("scoped current proof and nonwaiting reentrant admission", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            require(observer.withExecution(correct, [&](const auto& execution)
            {
                require(execution.current(correct), "Current execution accepts its exact observation");
                require(!execution.current(wrongNative) && !execution.current(wrongOsg),
                    "Context switch cannot reuse scoped current proof");
                require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                    "Reentrant admission returns busy without waiting");
                require(owner->snapshot().executing, "Monitoring exposes active execution");
            }) == DiagnosticNamespaceStatus::Executed, "Successful current scope runs callback");
            require(!owner->snapshot().executing, "Scope completion releases exclusive admission");
            require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Executed,
                "Next serialized callback enters");
        });
        test("wrong thread within scope and controlled thread handoff", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            const auto epoch = owner->snapshot().epoch;
            bool badCurrent = true, badCharge = true;
            DiagnosticNamespaceStatus badBind = DiagnosticNamespaceStatus::Executed;
            DiagnosticPublicationResult badPublish;
            observer.withExecution(correct, [&](const auto& execution)
            {
                std::thread misuse([&]
                {
                    badCurrent = execution.current(correct);
                    badCharge = execution.setKnownResources(999, 9);
                    badBind = execution.bindProducerResourceEpoch(12);
                    badPublish = execution.withPublication(correct, publication(epoch), ignoreBorrow);
                });
                misuse.join();
                require(!badCurrent && !badCharge, "A current lease cannot be used by a foreign thread");
                require(badBind == DiagnosticNamespaceStatus::WrongThread
                    && badPublish.status == DiagnosticNamespaceStatus::WrongThread,
                    "Foreign thread fails before mutable publication access");
            });
            DiagnosticNamespaceStatus draw = DiagnosticNamespaceStatus::WrongThread;
            std::thread firstDraw([&]
            {
                draw = observer.withExecution(correct, [&](const auto& execution)
                {
                    require(execution.current(correct), "New draw thread proves scoped current context");
                    require(execution.bindProducerResourceEpoch(12) == DiagnosticNamespaceStatus::Executed,
                        "Actual resource epoch binds on draw owner");
                    require(execution.withPublication(correct, publication(epoch), ignoreBorrow).status
                        == DiagnosticNamespaceStatus::Executed, "Draw thread publishes real metadata");
                });
            });
            firstDraw.join();
            require(draw == DiagnosticNamespaceStatus::Executed, "Creation thread is not a permanent draw owner");
            require(observer.withExecution(correct, [&](const auto& execution)
            {
                require(execution.withPublication(correct, publication(epoch), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::StalePublication, "Main thread cannot refresh an already used descriptor");
            }) == DiagnosticNamespaceStatus::Executed, "Joined draw thread hands current execution back to main");
            std::thread restartedDraw([&]
            {
                draw = observer.withExecution(correct, [&](const auto& execution)
                {
                    require(execution.withPublication(correct, publication(epoch, 2, 72, 1), ignoreBorrow).status
                        == DiagnosticNamespaceStatus::Executed, "Replacement draw thread preserves monotonic history");
                });
            });
            restartedDraw.join();
            require(draw == DiagnosticNamespaceStatus::Executed && owner->snapshot().publications == 2,
                "Restarted thread enters same live namespace without reconstructing lifetime");
        });
        test("persistent publication watermark, real fields, stale and malformed inputs", []
        {
            auto owner = create();
            const auto epoch = owner->snapshot().epoch;
            owner->observer().withExecution(correct, [&](const auto& execution)
            {
                const auto first = publication(epoch);
                require(execution.withPublication(correct, first, ignoreBorrow).status
                    == DiagnosticNamespaceStatus::MissingResources, "No observed actual producer epoch is not ready");
                require(execution.bindProducerResourceEpoch(0) == DiagnosticNamespaceStatus::InvalidResourceEpoch,
                    "Zero epoch cannot establish a producer");
                require(execution.bindProducerResourceEpoch(12) == DiagnosticNamespaceStatus::Executed,
                    "Bind actual producer resource epoch");
                require(execution.withPublication(correct, first, [&](const auto& borrow)
                {
                    const auto* sources = borrow.sources();
                    require(sources && sources->identity.ticket == 71 && sources->identity.frame == 0
                        && sources->identity.submission == 1 && sources->identity.resourceEpoch == 12
                        && sources->identity.view == 51, "Ticket, frame, capture serial, resource epoch and view stay distinct");
                    require(execution.withPublication(correct, publication(epoch, 2, 72, 1), ignoreBorrow).status
                        == DiagnosticNamespaceStatus::Busy, "Same-scope recursive publication cannot open a second metadata borrow");
                }).status == DiagnosticNamespaceStatus::Executed, "Valid acquired draw publication succeeds");
                for (unsigned field = 0; field < 6; ++field)
                {
                    auto stale = publication(epoch, 2, 72, 1);
                    if (field == 0) stale.identity.submission = 1;
                    if (field == 1) stale.identity.ticket = 71;
                    if (field == 2) stale.identity.frame = 0;
                    if (field == 3) stale.identity.motionRevision = 8;
                    if (field == 4) stale.identity.resourceEpoch = 11;
                    if (field == 5) stale.identity.view = 52;
                    require(execution.withPublication(correct, stale, ignoreBorrow).status
                        == (field == 4 ? DiagnosticNamespaceStatus::InvalidResourceEpoch
                                      : DiagnosticNamespaceStatus::StalePublication),
                        "Stale submission/ticket/frame/revision/resource/view refuses without consumer");
                }
                for (unsigned field = 0; field < 4; ++field)
                {
                    auto wrong = publication(epoch, 2, 72, 1);
                    if (field == 0) ++wrong.identity.contextEpoch;
                    if (field == 1) ++wrong.identity.nativeContext;
                    if (field == 2) ++wrong.identity.osgContext;
                    if (field == 3) wrong.images[1].id = 0;
                    require(execution.withPublication(correct, wrong, ignoreBorrow).status
                        == (field == 3 ? DiagnosticNamespaceStatus::MetadataRejected
                                      : DiagnosticNamespaceStatus::InvalidIdentity),
                        "Wrong issued namespace or missing source refuses");
                }
                require(owner->snapshot().publications == 1, "Rejected descriptors consume no publication counter");
                require(execution.withPublication(correct, publication(epoch, 2, 72, 1), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::Executed, "Rejected inputs do not poison the valid next draw");
            });
            owner->observer().withExecution(correct, [&](const auto& execution)
            {
                require(execution.bindProducerResourceEpoch(11) == DiagnosticNamespaceStatus::InvalidResourceEpoch,
                    "Producer epoch cannot move backward");
                require(execution.bindProducerResourceEpoch(13) == DiagnosticNamespaceStatus::Executed,
                    "Real resource replacement advances expected epoch");
                require(execution.withPublication(correct, publication(epoch, 3, 73, 2), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::InvalidResourceEpoch, "Old slot resource generation cannot be refreshed");
                require(execution.withPublication(correct, publication(epoch, 1, 71, 0, 13), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::StalePublication, "Resource replacement cannot reset capture/history watermark");
                require(execution.withPublication(correct, publication(epoch, 3, 73, 2, 13), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::Executed, "Matching new resource generation proceeds");
            });
        });
        test("consumer exception releases scope but never refreshes consumed publication", []
        {
            auto owner = create();
            const auto epoch = owner->snapshot().epoch;
            bool caught = false;
            try
            {
                owner->observer().withExecution(correct, [&](const auto& execution)
                {
                    execution.bindProducerResourceEpoch(12);
                    execution.withPublication(correct, publication(epoch), [](const auto&)
                    {
                        throw std::runtime_error("controlled diagnostic consumer failure");
                    });
                });
            }
            catch (const std::runtime_error&) { caught = true; }
            require(caught && !owner->snapshot().executing, "Unwinding releases borrow and exclusive scope");
            owner->observer().withExecution(correct, [&](const auto& execution)
            {
                require(execution.withPublication(correct, publication(epoch), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::StalePublication, "Failed backend cannot resubmit consumed history");
                require(execution.withPublication(correct, publication(epoch, 2, 72, 1), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::Executed, "Next proper draw can recover without context recreation");
            });
        });
        test("concurrent close freezes new admission while current execution finishes", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            const auto epoch = owner->snapshot().epoch;
            std::mutex mutex;
            std::condition_variable cv;
            bool admitted = false, released = false;
            DiagnosticNamespaceStatus result = DiagnosticNamespaceStatus::WrongContext;
            std::thread draw([&]
            {
                result = observer.withExecution(correct, [&](const auto& execution)
                {
                    execution.setKnownResources(1200, 3);
                    std::unique_lock lock(mutex);
                    admitted = true;
                    cv.notify_one();
                    cv.wait(lock, [&] { return released; });
                    require(execution.current(correct), "Admitted execution stays current while orderly close waits for its end");
                    require(!execution.admissionOpen(), "Closing disables further publication admission");
                    require(execution.withPublication(correct, publication(epoch), ignoreBorrow).status
                        == DiagnosticNamespaceStatus::Closing, "No new diagnostic publication starts in closing scope");
                    require(execution.setKnownResources(1800, 4), "Existing work remains explicitly charged until retirement");
                });
            });
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return admitted; });
            }
            require(owner->beginClose() == DiagnosticNamespaceStatus::Busy, "Close freezes admission without blocking on active work");
            require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::Busy,
                "Native destroy cannot be credited before admitted scope ends");
            require(owner->confirmNativeLost() == DiagnosticNamespaceStatus::Busy,
                "Even external loss confirmation cannot mutate active callback state");
            require(owner->withRetirement(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                "Cleanup cannot overlap existing draw ownership");
            require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                "Second draw scope refuses while first is active");
            {
                std::lock_guard lock(mutex);
                released = true;
            }
            cv.notify_one();
            draw.join();
            require(result == DiagnosticNamespaceStatus::Executed && !owner->snapshot().executing,
                "Admitted work ends normally without fixed-thread teardown termination");
            require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Closing,
                "No draw may enter the now idle closing namespace");
            require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::PendingRetirement,
                "Outstanding backend charges forbid normal destruction credit");
            require(owner->snapshot().knownBytes == 1800 && owner->snapshot().knownObjects == 4,
                "Closure retains all known owned resource charges");
        });
        test("main-thread current retirement after draw-thread join and duplicate close", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            std::thread draw([&]
            {
                observer.withExecution(correct, [&](const auto& execution) { execution.setKnownResources(4096, 7); });
            });
            draw.join();
            require(owner->withRetirement(correct, emptyConsumer) == DiagnosticNamespaceStatus::Closing,
                "A live namespace cannot bypass orderly beginClose");
            owner->beginClose();
            require(owner->withRetirement(wrongNative, emptyConsumer) == DiagnosticNamespaceStatus::WrongContext,
                "Noncurrent cleanup cannot credit deletion");
            require(owner->withRetirement(correct, [&](const auto& execution)
            {
                require(execution.current(correct), "Cleanup has exact current namespace on main thread");
                require(!execution.admissionOpen(), "Cleanup does not reopen draw admission");
                require(execution.setKnownResources(0, 0), "Explicit successful backend cleanup closes logical charges");
                require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::Busy,
                    "Platform death receipt cannot precede cleanup scope end");
            }) == DiagnosticNamespaceStatus::Executed, "Thread-neutral owner supports main-thread cleanup");
            require(owner->snapshot().retirementComplete, "Only zero remaining charges complete orderly retirement");
            require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::Destroyed,
                "Successful native destroy witness closes namespace");
            require(owner->beginClose() == DiagnosticNamespaceStatus::Destroyed
                && owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::Destroyed,
                "Duplicate close and release remain idempotent");
            require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Destroyed,
                "Old observer cannot revive a destroyed namespace");
            require(owner->withRetirement(correct, emptyConsumer) == DiagnosticNamespaceStatus::Destroyed,
                "Duplicate cleanup cannot run against destroyed names");
        });
        test("actual destroy callback reservation and failed operations stay closed", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            unsigned called = 0;
            require(owner->withNativeDestroy([&] { ++called; return true; }) == DiagnosticNamespaceStatus::Closing
                && !called, "Native callback does not run before closure freezes admission");
            observer.withExecution(correct, [&](const auto& execution) { execution.setKnownResources(6400, 8); });
            owner->beginClose();
            require(owner->withNativeDestroy([&] { ++called; return true; })
                == DiagnosticNamespaceStatus::PendingRetirement && !called,
                "Pending owned storage prevents the actual platform operation, not just its success receipt");
            require(owner->snapshot().knownBytes == 6400 && owner->snapshot().knownObjects == 8,
                "Refused destruction cannot erase pending charges");
            bool caughtDeleteFailure = false;
            try
            {
                owner->withRetirement(correct, [&](const auto&)
                {
                    throw std::runtime_error("controlled bind/delete failure");
                });
            }
            catch (const std::runtime_error&) { caughtDeleteFailure = true; }
            require(caughtDeleteFailure && !owner->snapshot().executing
                && owner->snapshot().knownBytes == 6400 && !owner->snapshot().retirementComplete,
                "Cleanup failure releases the reservation and retains all unfinished debt");
            owner->withRetirement(correct, [&](const auto& execution) { execution.setKnownResources(0, 0); });
            require(owner->withNativeDestroy([&]
            {
                ++called;
                require(owner->snapshot().executing, "Native operation holds an exclusive closing reservation");
                require(owner->withRetirement(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                    "Retirement cannot overlap the actual native destroy operation");
                require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                    "Draw cannot overlap actual native destroy operation");
                require(owner->confirmNativeLost() == DiagnosticNamespaceStatus::Busy,
                    "Separate loss receipt cannot race the reserved native operation");
                require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::Busy,
                    "Reentrant native destruction refuses before a second platform operation");
                return false;
            }) == DiagnosticNamespaceStatus::NativeDestroyFailed, "False native result never receives death credit");
            require(owner->snapshot().phase == DiagnosticNamespacePhase::Closing && !owner->snapshot().executing
                && owner->snapshot().destructionFailures == 1 && !owner->snapshot().knownBytes,
                "Native failure keeps the namespace closed after completed backend retirement");
            require(owner->withNativeDestroy([]() -> bool { throw std::runtime_error("controlled destroy failure"); })
                == DiagnosticNamespaceStatus::NativeDestroyFailed,
                "Native exception is contained without successful destruction credit");
            require(owner->snapshot().phase == DiagnosticNamespacePhase::Closing
                && !owner->snapshot().executing && owner->snapshot().destructionFailures == 2,
                "Exception remains an accounted refused state");
            require(owner->withNativeDestroy([&] { ++called; return true; }) == DiagnosticNamespaceStatus::Destroyed,
                "A later confirmed successful platform operation completes the closed namespace");
            const auto finalCalled = called;
            require(owner->withNativeDestroy([&] { ++called; return true; }) == DiagnosticNamespaceStatus::Destroyed
                && called == finalCalled, "Duplicate release never calls the native API twice");
            require(owner->noteNativeDestroyFailure() == DiagnosticNamespaceStatus::Destroyed
                && owner->snapshot().destructionFailures == 2, "Destroyed namespace cannot acquire a spurious new failure receipt");
        });
        test("failed native destruction and confirmed loss retain honest accounting", []
        {
            auto owner = create();
            owner->observer().withExecution(correct, [&](const auto& execution) { execution.setKnownResources(12288, 8); });
            require(owner->noteNativeDestroyFailure() == DiagnosticNamespaceStatus::NativeDestroyFailed,
                "Platform destroy failure is explicit");
            require(owner->snapshot().phase == DiagnosticNamespacePhase::Closing
                && owner->snapshot().destructionFailures == 1 && owner->snapshot().knownBytes == 12288,
                "Failure refuses new work and retains known live debt");
            require(owner->withNativeDestroy([] { return true; }) == DiagnosticNamespaceStatus::PendingRetirement,
                "Failed destroy is not a normal successful retirement");
            require(owner->confirmNativeLost() == DiagnosticNamespaceStatus::Destroyed,
                "Separate externally confirmed death may abandon old namespace names");
            require(!owner->snapshot().knownBytes && !owner->snapshot().knownObjects
                && owner->snapshot().abandonedBytes == 12288 && owner->snapshot().abandonedObjects == 8
                && !owner->snapshot().retirementComplete,
                "Abandoned logical bytes are separate from explicitly deleted/recovered resources");
            owner->confirmNativeLost();
            require(owner->snapshot().abandonedBytes == 12288, "Duplicate loss never double-credits abandoned bytes");
        });
        test("old descriptors and observers cannot refresh into reused numeric handles", []
        {
            auto old = create();
            const auto oldObserver = old->observer();
            const auto descriptor = publication(old->snapshot().epoch);
            old->beginClose();
            old->withNativeDestroy([] { return true; });
            auto replacement = create();
            require(replacement->snapshot().epoch > descriptor.identity.contextEpoch,
                "Same numeric handles get independently issued new namespace identity");
            replacement->observer().withExecution(correct, [&](const auto& execution)
            {
                execution.bindProducerResourceEpoch(12);
                require(execution.withPublication(correct, descriptor, ignoreBorrow).status
                    == DiagnosticNamespaceStatus::InvalidIdentity, "Retained old publication cannot submit to recreated namespace");
                require(execution.withPublication(correct, publication(replacement->snapshot().epoch), ignoreBorrow).status
                    == DiagnosticNamespaceStatus::Executed, "New owner-issued producer can publish normally");
            });
            require(oldObserver.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Destroyed,
                "Old observer stays bound to its actual issued lifetime");
            old.reset();
            require(oldObserver.snapshot().phase == DiagnosticNamespacePhase::Expired
                && oldObserver.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Expired,
                "Expired observer cannot reacquire authority from replacement handle address");
        });
        test("noncurrent owner destruction and owner expiry during active scope", []
        {
            auto owner = create();
            const auto observer = owner->observer();
            owner->observer().withExecution(correct, [&](const auto& execution)
            {
                execution.setKnownResources(5000, 5);
                owner.reset();
                require(observer.snapshot().phase == DiagnosticNamespacePhase::Closing
                    && !observer.snapshot().authorityPresent && observer.snapshot().knownBytes == 5000,
                    "Destructor freezes admission without claiming native destruction or GL deletion");
                require(execution.current(correct), "Scope pins CPU control long enough to finish admitted work");
                require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Busy,
                    "No replacement scope can enter after owner expiry");
            });
            require(observer.snapshot().phase == DiagnosticNamespacePhase::Expired,
                "Scope release permits CPU control expiry without a thread-bound destructor");
            require(observer.withExecution(correct, emptyConsumer) == DiagnosticNamespaceStatus::Expired,
                "Observer is weak and keeps neither manager nor platform owner alive");
        });
        std::cout << "PASS diagnostic namespace CPU lifecycle: " << cases << " scenarios, " << checks
                  << " assertions; no SDL/GL/game/NGX execution\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL after " << cases << " scenarios / " << checks << " assertions: " << error.what() << '\n';
        return 1;
    }
}
