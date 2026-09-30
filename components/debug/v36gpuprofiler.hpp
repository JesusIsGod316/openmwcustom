#ifndef OPENMW_COMPONENTS_DEBUG_V36GPUPROFILER_H
#define OPENMW_COMPONENTS_DEBUG_V36GPUPROFILER_H

#include <array>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <osg/Camera>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>
#include <osg/FrameStamp>
#include <osg/ref_ptr>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36GpuProfiler
{
    // Windows' core OpenGL 1.1 header omits these standard query enums. Keep
    // header-local names so including this tracer never depends on SDL order.
    inline constexpr GLenum QueryTimestamp = 0x8e28;
    inline constexpr GLenum QueryResultAvailable = 0x8867;
    inline constexpr GLenum QueryResult = 0x8866;
    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_GPU_PASS_FILE",
            "source_frame,report_frame,epoch_ms,pass,gpu_ms,latency_frames,context,dropped_total,camera,writer_dropped_total");
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
            if (!writer().enabled())
                return;
            osg::State* state = renderInfo.getState();
            if (!state)
                return;
            osg::GLExtensions* gl = state->get<osg::GLExtensions>();
            if (!supported(gl))
                return;

            const unsigned int context = state->getContextID();
            std::lock_guard<std::mutex> lock(mMutex);
            Context& data = mContexts[context];
            const auto* stamp = state->getFrameStamp();
            if (!stamp) return;
            const auto frame = stamp->getFrameNumber();
            collect(*gl, context, data, frame);
            if (data.mActive >= 0)
                return;

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
                if (slot.mBeginQuery == 0 || slot.mEndQuery == 0)
                    return;
                slot.mFrame = frame;
                slot.mCamera = reinterpret_cast<std::uintptr_t>(renderInfo.getCurrentCamera());
                gl->glQueryCounter(slot.mBeginQuery, QueryTimestamp);
                data.mActive = static_cast<int>(index);
                data.mNext = (index + 1) % data.mSlots.size();
                return;
            }
            ++data.mDropped;
        }

        void end(osg::RenderInfo& renderInfo)
        {
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
            collect(*gl, context, data, state->getFrameStamp() ? state->getFrameStamp()->getFrameNumber() : slot.mFrame);
        }

    private:
        struct Slot
        {
            GLuint mBeginQuery = 0;
            GLuint mEndQuery = 0;
            std::uint64_t mFrame = 0;
            std::uintptr_t mCamera = 0;
            bool mPending = false;
        };

        struct Context
        {
            std::array<Slot, 16> mSlots{};
            std::size_t mNext = 0;
            int mActive = -1;
            std::uint64_t mDropped = 0;
        };

        static bool supported(const osg::GLExtensions* gl)
        {
            return gl && gl->glGenQueries && gl->glQueryCounter && gl->glGetQueryObjectuiv
                && gl->glGetQueryObjectui64v;
        }

        void collect(osg::GLExtensions& gl, unsigned int contextId, Context& data, std::uint64_t reportFrame)
        {
            for (Slot& slot : data.mSlots)
            {
                if (!slot.mPending)
                    continue;
                GLuint beginReady = 0;
                GLuint endReady = 0;
                gl.glGetQueryObjectuiv(slot.mBeginQuery, QueryResultAvailable, &beginReady);
                gl.glGetQueryObjectuiv(slot.mEndQuery, QueryResultAvailable, &endReady);
                if (!beginReady || !endReady)
                    continue;

                GLuint64 beginTimestamp = 0;
                GLuint64 endTimestamp = 0;
                // GL_QUERY_RESULT is read only after both availability checks succeeded. This never forces completion.
                gl.glGetQueryObjectui64v(slot.mBeginQuery, QueryResult, &beginTimestamp);
                gl.glGetQueryObjectui64v(slot.mEndQuery, QueryResult, &endTimestamp);
                slot.mPending = false;
                if (endTimestamp < beginTimestamp)
                    continue;

                std::ostringstream row;
                row << slot.mFrame << ',' << reportFrame << ',' << V3Diagnostics::epochMs() << ','
                    << V3Diagnostics::csvQuote(mName) << ',' << std::fixed << std::setprecision(4)
                    << (static_cast<double>(endTimestamp - beginTimestamp) / 1000000.0) << ','
                    << (reportFrame >= slot.mFrame ? reportFrame - slot.mFrame : 0) << ',' << contextId << ','
                    << data.mDropped << ',' << slot.mCamera << ',' << writer().droppedLines();
                writer().writeLine(row.str());
            }
        }

        std::string mName;
        std::mutex mMutex;
        std::map<unsigned int, Context> mContexts;
    };

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
