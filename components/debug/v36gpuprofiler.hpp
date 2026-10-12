#ifndef OPENMW_COMPONENTS_DEBUG_V36GPUPROFILER_H
#define OPENMW_COMPONENTS_DEBUG_V36GPUPROFILER_H

#include <array>
#include <cstdint>
#include <functional>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <osg/Camera>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/FrameStamp>
#include <osg/ref_ptr>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"
#include "compileattribution.hpp"

namespace Debug::V36GpuProfiler
{
    // Windows' core OpenGL 1.1 header omits these standard query enums. Keep
    // header-local names so including this tracer never depends on SDL order.
    inline constexpr GLenum QueryTimestamp = 0x8e28;
    inline constexpr GLenum QueryResultAvailable = 0x8867;
    inline constexpr GLenum QueryResult = 0x8866;
    inline bool deferredCollectionRequested()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("OPENMW_P9_GPU_QUERY_DEFER");
            return value && std::string_view(value) == "1";
        }();
        return enabled;
    }
    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_GPU_PASS_FILE",
            "source_frame,report_frame,epoch_ms,pass,gpu_ms,latency_frames,context,dropped_total,camera,writer_dropped_total,report_frame_known,collection_policy");
        return writer;
    }

    class PassTracker : public osg::Referenced
    {
    public:
        explicit PassTracker(std::string name)
            : mName(std::move(name))
        {
        }

        void begin(osg::RenderInfo& renderInfo)
        {
            if (!writer().enabled() || registry().finished.load(std::memory_order_acquire))
                return;
            std::lock_guard<std::mutex> lock(mMutex);
            registerSelf();
            ++mBeginCalls;
            osg::State* state = renderInfo.getState();
            if (!state) {
                ++mMissingState;
                return;
            }
            osg::GLExtensions* gl = state->get<osg::GLExtensions>();
            const unsigned int context = state->getContextID();
            Context& data = mContexts[context];
            if (!supported(gl)) {
                ++data.mUnsupported;
                return;
            }
            const auto* stamp = state->getFrameStamp();
            if (!stamp) { ++data.mMissingStamp; return; }
            const auto frame = stamp->getFrameNumber();
            collect(*gl, context, data, frame, true);
            if (data.mActive >= 0) {
                ++data.mNestedBegin;
                return;
            }

            for (std::size_t offset = 0; offset < data.mSlots.size(); ++offset)
            {
                const std::size_t index = (data.mNext + offset) % data.mSlots.size();
                Slot& slot = data.mSlots[index];
                if (slot.mPending)
                    continue;
                if (slot.mBeginQuery == 0)
                {
                    gl->glGenQueries(1, &slot.mBeginQuery);
                    gl->glGenQueries(1, &slot.mEndQuery);
                }
                if (slot.mBeginQuery == 0 || slot.mEndQuery == 0) {
                    ++data.mQueryAllocationFailures;
                    return;
                }
                slot.mFrame = frame;
                slot.mNextPollFrame = frame + 2;
                slot.mUnavailablePolls = 0;
                slot.mBackoffStep = 0;
                slot.mCamera = reinterpret_cast<std::uintptr_t>(renderInfo.getCurrentCamera());
                gl->glQueryCounter(slot.mBeginQuery, QueryTimestamp);
                ++data.mIssued;
                data.mActive = static_cast<int>(index);
                data.mNext = (index + 1) % data.mSlots.size();
                return;
            }
            ++data.mDropped;
        }

        void end(osg::RenderInfo& renderInfo)
        {
            if (!writer().enabled() || registry().finished.load(std::memory_order_acquire)) return;
            osg::State* state = renderInfo.getState();
            if (!state)
                return;
            osg::GLExtensions* gl = state->get<osg::GLExtensions>();
            if (!supported(gl))
                return;

            const unsigned int context = state->getContextID();
            std::lock_guard<std::mutex> lock(mMutex);
            auto found = mContexts.find(context);
            if (found == mContexts.end() || found->second.mActive < 0)
                return;
            Context& data = found->second;
            Slot& slot = data.mSlots[static_cast<std::size_t>(data.mActive)];
            gl->glQueryCounter(slot.mEndQuery, QueryTimestamp);
            slot.mPending = true;
            data.mActive = -1;
            const auto* stamp = state->getFrameStamp();
            if (!deferredCollectionRequested())
                collect(*gl, context, data, stamp ? stamp->getFrameNumber() : slot.mFrame, stamp != nullptr);
        }

        // Engine shutdown calls only after graphics/resource/viewer owners are
        // joined. Never issue GL calls here: final pending queries stay pending,
        // rather than forcing completion or silently claiming complete results.
        static void finish(bool producerThreadsJoined) noexcept
        {
            if (!writer().enabled() || registry().finished.exchange(true)) return;
            try
            {
                const char* path = std::getenv("OPENMW_V36_GPU_PASS_FILE");
                if (!path || !*path) return;
                std::vector<osg::ref_ptr<PassTracker>> trackers;
                { std::lock_guard<std::mutex> lock(registry().mutex); trackers = registry().trackers; }
                std::ofstream out(std::string(path) + ".queries.csv");
                std::ofstream unresolved(std::string(path) + ".pending.csv");
                unresolved << "schema,tracker,pass,context,source_frame,camera,begin_query,end_query,status,next_poll_frame,unavailable_polls\n";
                out << "schema,tracker,pass,context,issued,collected,invalid,pending,active,admission_dropped,"
                    "unavailable_polls,deferred_skips,frame_regressions,unsupported,missing_stamp,query_allocation_failures,nested_begin\n";
                std::uint64_t issued=0, collected=0, invalid=0, pending=0, active=0, dropped=0;
                std::uint64_t unavailable=0, deferred=0, regressions=0, unsupported=0, missing=0, failures=0, nested=0;
                std::uint64_t beginCalls=0, missingState=0;
                bool coherent = true;
                for (const auto& tracker : trackers)
                {
                    std::lock_guard<std::mutex> lock(tracker->mMutex);
                    beginCalls += tracker->mBeginCalls; missingState += tracker->mMissingState;
                    for (const auto& [contextId, data] : tracker->mContexts)
                    {
                        std::uint64_t slotPending = 0;
                        for (std::size_t index=0; index<data.mSlots.size(); ++index)
                        {
                            const auto& slot = data.mSlots[index];
                            slotPending += slot.mPending;
                            if (slot.mPending || data.mActive == static_cast<int>(index))
                                unresolved << "1," << reinterpret_cast<std::uintptr_t>(tracker.get()) << ','
                                    << V3Diagnostics::csvQuote(tracker->mName) << ',' << contextId << ',' << slot.mFrame
                                    << ',' << slot.mCamera << ',' << slot.mBeginQuery << ',' << slot.mEndQuery << ','
                                    << (slot.mPending ? "pending" : "active") << ',' << slot.mNextPollFrame
                                    << ',' << slot.mUnavailablePolls << '\n';
                        }
                        const unsigned slotActive = data.mActive >= 0 ? 1 : 0;
                        coherent = coherent && data.mIssued == data.mCollected + data.mInvalid + slotPending + slotActive;
                        out << "1," << reinterpret_cast<std::uintptr_t>(tracker.get()) << ',' << V3Diagnostics::csvQuote(tracker->mName)
                            << ',' << contextId << ',' << data.mIssued << ',' << data.mCollected << ',' << data.mInvalid
                            << ',' << slotPending << ',' << slotActive << ',' << data.mDropped << ',' << data.mUnavailablePolls
                            << ',' << data.mDeferredSkips << ',' << data.mFrameRegressions << ',' << data.mUnsupported
                            << ',' << data.mMissingStamp << ',' << data.mQueryAllocationFailures << ',' << data.mNestedBegin << '\n';
                        issued += data.mIssued; collected += data.mCollected; invalid += data.mInvalid;
                        pending += slotPending; active += slotActive; dropped += data.mDropped;
                        unavailable += data.mUnavailablePolls; deferred += data.mDeferredSkips; regressions += data.mFrameRegressions;
                        unsupported += data.mUnsupported; missing += data.mMissingStamp; failures += data.mQueryAllocationFailures;
                        nested += data.mNestedBegin;
                    }
                }
                out.flush(); const bool flushed = bool(out); out.close();
                unresolved.flush(); const bool unresolvedFlushed = bool(unresolved); unresolved.close();
                const bool outputOk = flushed && !out.fail() && unresolvedFlushed && !unresolved.fail();
                const auto registrationFailures = registry().registrationFailures.load();
                const bool accounting = outputOk && producerThreadsJoined && coherent && !registrationFailures;
                const bool complete = accounting && issued && !pending && !active && !invalid && !dropped && !unsupported
                    && !missing && !missingState && !failures && !nested && !regressions && !writer().droppedLines();
                std::ofstream status(std::string(path) + ".query-status.txt");
                status << "schema=1\nnormal_finish=1\noutput_ok=" << outputOk << "\nproducer_threads_joined=" << producerThreadsJoined
                    << "\naccounting_complete=" << accounting << "\nquery_result_coverage_complete=" << complete
                    << "\nwriter_finalization=pending_external_status"
                    << "\npolicy=" << (deferredCollectionRequested() ? "deferred_begin_min2_backoff8" : "legacy_begin_end")
                    << "\nregistered_trackers=" << trackers.size() << "\nregistration_failures=" << registrationFailures
                    << "\nbegin_calls=" << beginCalls << "\nissued=" << issued << "\ncollected=" << collected
                    << "\ninvalid=" << invalid << "\npending=" << pending << "\nactive=" << active
                    << "\nadmission_dropped=" << dropped << "\nunavailable_polls=" << unavailable
                    << "\ndeferred_skips=" << deferred << "\nframe_regressions=" << regressions
                    << "\nunsupported=" << unsupported << "\nmissing_state=" << missingState << "\nmissing_stamp=" << missing
                    << "\nquery_allocation_failures=" << failures << "\nnested_begin=" << nested
                    << "\nwriter_dropped=" << writer().droppedLines()
                    << "\nlimitation=No shutdown GL drain. Pending/active are missing results, not zero GPU time. Timestamp intervals may include submission gaps/idle; no GPU-active proof.\n"
                    << "status_complete=1\n";
                status.flush(); status.close();
            }
            catch (...) { /* Missing final status fails its independent qualification. */ }
        }

    private:
        struct Slot
        {
            GLuint mBeginQuery = 0;
            GLuint mEndQuery = 0;
            std::uint64_t mFrame = 0;
            std::uintptr_t mCamera = 0;
            std::uint64_t mNextPollFrame = 0;
            std::uint64_t mUnavailablePolls = 0;
            unsigned mBackoffStep = 0;
            bool mPending = false;
        };

        struct Context
        {
            std::array<Slot, 16> mSlots{};
            std::size_t mNext = 0;
            int mActive = -1;
            std::uint64_t mDropped = 0;
            std::uint64_t mIssued=0, mCollected=0, mInvalid=0, mUnavailablePolls=0, mDeferredSkips=0;
            std::uint64_t mFrameRegressions=0, mUnsupported=0, mMissingStamp=0, mQueryAllocationFailures=0, mNestedBegin=0;
            std::optional<std::uint64_t> mLastCollectionFrame;
        };

        struct Registry
        {
            std::mutex mutex;
            std::vector<osg::ref_ptr<PassTracker>> trackers;
            std::atomic<bool> finished{false};
            std::atomic<std::uint64_t> registrationFailures{0};
        };
        static Registry& registry() { static Registry value; return value; }
        void registerSelf()
        {
            if (mRegistered) return;
            mRegistered = true;
            try {
                std::lock_guard<std::mutex> lock(registry().mutex);
                registry().trackers.emplace_back(this);
            } catch (const std::bad_alloc&) { registry().registrationFailures.fetch_add(1); }
        }

        static bool supported(const osg::GLExtensions* gl)
        {
            return gl && gl->glGenQueries && gl->glQueryCounter && gl->glGetQueryObjectuiv
                && gl->glGetQueryObjectui64v;
        }

        void collect(osg::GLExtensions& gl, unsigned int contextId, Context& data, std::uint64_t reportFrame,
            bool reportFrameKnown)
        {
            const bool deferred = deferredCollectionRequested();
            if (!reportFrameKnown) ++data.mMissingStamp;
            if (deferred)
            {
                if (!reportFrameKnown) return;
                if (data.mLastCollectionFrame && *data.mLastCollectionFrame == reportFrame) return;
                data.mLastCollectionFrame = reportFrame;
            }
            std::optional<HitchTrace::Scope> collectScope;
            if (CompileAttribution::requested())
            {
                collectScope.emplace("diagnostic.gpu_query_collect", mName, "", static_cast<unsigned>(reportFrame),
                    HitchTrace::CpuTiming::WallEnvelope);
                collectScope->setMetric(0, "context", contextId);
                collectScope->setMetric(1, "frame_known", reportFrameKnown ? 1 : 0);
            }
            for (Slot& slot : data.mSlots)
            {
                if (!slot.mPending)
                    continue;
                if (reportFrameKnown && reportFrame < slot.mFrame)
                { ++data.mFrameRegressions; if (deferred) continue; }
                if (deferred && reportFrame < slot.mNextPollFrame) { ++data.mDeferredSkips; continue; }
                GLuint beginReady = 0;
                GLuint endReady = 0;
                gl.glGetQueryObjectuiv(slot.mBeginQuery, QueryResultAvailable, &beginReady);
                gl.glGetQueryObjectuiv(slot.mEndQuery, QueryResultAvailable, &endReady);
                if (!beginReady || !endReady) {
                    ++data.mUnavailablePolls;
                    ++slot.mUnavailablePolls;
                    if (deferred)
                    {
                        slot.mBackoffStep = (std::min)(slot.mBackoffStep + 1, 3u);
                        slot.mNextPollFrame = reportFrame + (std::uint64_t{1} << slot.mBackoffStep);
                    }
                    continue;
                }

                GLuint64 beginTimestamp = 0;
                GLuint64 endTimestamp = 0;
                // Read only completed results. Availability/result API calls
                // can still have CPU cost inside the driver; this is no fence.
                gl.glGetQueryObjectui64v(slot.mBeginQuery, QueryResult, &beginTimestamp);
                gl.glGetQueryObjectui64v(slot.mEndQuery, QueryResult, &endTimestamp);
                slot.mPending = false;
                if (endTimestamp < beginTimestamp) {
                    ++data.mInvalid;
                    continue;
                }
                ++data.mCollected;

                std::optional<HitchTrace::Scope> formatScope;
                if (CompileAttribution::requested())
                {
                    formatScope.emplace("diagnostic.gpu_csv_format", mName, "", static_cast<unsigned>(reportFrame),
                        HitchTrace::CpuTiming::WallEnvelope);
                    formatScope->setMetric(0, "context", contextId);
                    formatScope->setMetric(1, "frame_known", reportFrameKnown ? 1 : 0);
                }
                std::ostringstream row;
                row << slot.mFrame << ',' << reportFrame << ',' << V3Diagnostics::epochMs() << ','
                    << V3Diagnostics::csvQuote(mName) << ',' << std::fixed << std::setprecision(4)
                    << (static_cast<double>(endTimestamp - beginTimestamp) / 1000000.0) << ','
                    << (reportFrame >= slot.mFrame ? reportFrame - slot.mFrame : 0) << ',' << contextId << ','
                    << data.mDropped << ',' << slot.mCamera << ',' << writer().droppedLines() << ',' << reportFrameKnown
                    << ',' << (deferred ? "deferred_begin_min2_backoff8" : "legacy_begin_end");
                const std::string line = row.str();
                formatScope.reset();
                std::optional<HitchTrace::Scope> enqueueScope;
                if (CompileAttribution::requested())
                {
                    enqueueScope.emplace("diagnostic.gpu_csv_enqueue", mName, "", static_cast<unsigned>(reportFrame),
                        HitchTrace::CpuTiming::WallEnvelope);
                    enqueueScope->setMetric(0, "context", contextId);
                    enqueueScope->setMetric(1, "frame_known", reportFrameKnown ? 1 : 0);
                }
                writer().writeLine(line);
            }
        }

        std::string mName;
        std::mutex mMutex;
        std::map<unsigned int, Context> mContexts;
        std::uint64_t mBeginCalls = 0, mMissingState = 0;
        bool mRegistered = false;
    };

    inline void finish(bool producerThreadsJoined) noexcept { PassTracker::finish(producerThreadsJoined); }

    class CameraCallback : public osg::Camera::DrawCallback
    {
    public:
        CameraCallback(PassTracker* tracker, bool begin)
            : mTracker(tracker)
            , mBegin(begin)
        {
        }

        void operator()(osg::RenderInfo& renderInfo) const override
        {
            if (mBegin)
                mTracker->begin(renderInfo);
            else
                mTracker->end(renderInfo);
        }

    private:
        osg::ref_ptr<PassTracker> mTracker;
        bool mBegin;
    };

    inline void attachCamera(osg::Camera& camera, std::string name)
    {
        osg::ref_ptr<PassTracker> tracker = new PassTracker(std::move(name));
        camera.addInitialDrawCallback(new CameraCallback(tracker, true));
        camera.addFinalDrawCallback(new CameraCallback(tracker, false));
    }

    inline PassTracker& tracker(std::string_view name)
    {
        static std::mutex mutex;
        static std::map<std::string, osg::ref_ptr<PassTracker>, std::less<>> trackers;
        std::lock_guard<std::mutex> lock(mutex);
        auto [it, inserted] = trackers.try_emplace(std::string(name));
        if (inserted)
            it->second = new PassTracker(it->first);
        return *it->second;
    }

    class ScopedPass
    {
    public:
        ScopedPass(osg::RenderInfo& renderInfo, std::string name)
            : mRenderInfo(renderInfo)
            , mTracker(writer().enabled() ? &tracker(name) : nullptr)
        {
            if (mTracker)
                mTracker->begin(mRenderInfo);
        }

        ~ScopedPass()
        {
            if (mTracker)
                mTracker->end(mRenderInfo);
        }

        ScopedPass(const ScopedPass&) = delete;
        ScopedPass& operator=(const ScopedPass&) = delete;

    private:
        osg::RenderInfo& mRenderInfo;
        PassTracker* mTracker;
    };
}

#endif
