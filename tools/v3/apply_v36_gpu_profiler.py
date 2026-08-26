import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 GPU-profiler match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 GPU profiler patched {rel} ({count} match(es))")


def write_new(rel, text):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: refusing to overwrite an existing file")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"V3.6 GPU profiler added {rel}")


write_new(
    "components/debug/v36gpuprofiler.hpp",
    r'''#ifndef OPENMW_COMPONENTS_DEBUG_V36GPUPROFILER_H
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
#include <osg/ref_ptr>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36GpuProfiler
{
    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_GPU_PASS_FILE",
            "source_frame,report_frame,epoch_ms,pass,gpu_ms,latency_frames,context");
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
            collect(*gl, context, data);
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
                slot.mFrame = V3HitchTelemetry::currentFrame();
                gl->glQueryCounter(slot.mBeginQuery, GL_TIMESTAMP);
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
            gl->glQueryCounter(slot.mEndQuery, GL_TIMESTAMP);
            slot.mPending = true;
            data.mActive = -1;
            collect(*gl, context, data);
        }

    private:
        struct Slot
        {
            GLuint mBeginQuery = 0;
            GLuint mEndQuery = 0;
            std::uint64_t mFrame = 0;
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

        void collect(osg::GLExtensions& gl, unsigned int contextId, Context& data)
        {
            const std::uint64_t reportFrame = V3HitchTelemetry::currentFrame();
            for (Slot& slot : data.mSlots)
            {
                if (!slot.mPending)
                    continue;
                GLuint beginReady = 0;
                GLuint endReady = 0;
                gl.glGetQueryObjectuiv(slot.mBeginQuery, GL_QUERY_RESULT_AVAILABLE, &beginReady);
                gl.glGetQueryObjectuiv(slot.mEndQuery, GL_QUERY_RESULT_AVAILABLE, &endReady);
                if (!beginReady || !endReady)
                    continue;

                GLuint64 beginTimestamp = 0;
                GLuint64 endTimestamp = 0;
                // GL_QUERY_RESULT is read only after both availability checks succeeded. This never forces completion.
                gl.glGetQueryObjectui64v(slot.mBeginQuery, GL_QUERY_RESULT, &beginTimestamp);
                gl.glGetQueryObjectui64v(slot.mEndQuery, GL_QUERY_RESULT, &endTimestamp);
                slot.mPending = false;
                if (endTimestamp < beginTimestamp)
                    continue;

                std::ostringstream row;
                row << slot.mFrame << ',' << reportFrame << ',' << V3Diagnostics::epochMs() << ','
                    << V3Diagnostics::csvQuote(mName) << ',' << std::fixed << std::setprecision(4)
                    << (static_cast<double>(endTimestamp - beginTimestamp) / 1000000.0) << ','
                    << (reportFrame >= slot.mFrame ? reportFrame - slot.mFrame : 0) << ',' << contextId;
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
''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include <components/debug/v3gpumemory.hpp>''',
    '''#include <components/debug/v3gpumemory.hpp>
#include <components/debug/v36gpuprofiler.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        mViewer->getCamera()->setName(Constants::SceneCamera);''',
    '''        mViewer->getCamera()->setName(Constants::SceneCamera);
        if (Settings::cells().mV36AsyncGpuProfiler)
            Debug::V36GpuProfiler::attachCamera(*mViewer->getCamera(), "main_world");''',
)

replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        void setV33FarCascadeResolutionDivisor(unsigned int divisor);
        unsigned int getV33FarCascadeResolutionDivisor() const { return _v33FarCascadeResolutionDivisor; }''',
    '''        void setV33FarCascadeResolutionDivisor(unsigned int divisor);
        unsigned int getV33FarCascadeResolutionDivisor() const { return _v33FarCascadeResolutionDivisor; }
        void setV36AsyncGpuProfiler(bool enabled) { _v36AsyncGpuProfiler = enabled; }''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.hpp",
    '''        unsigned int                            _v33FarCascadeResolutionDivisor = 1;''',
    '''        unsigned int                            _v33FarCascadeResolutionDivisor = 1;
        bool                                    _v36AsyncGpuProfiler = false;''',
)
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''#include <components/settings/categories/shadows.hpp>''',
    '''#include <components/settings/categories/cells.hpp>
#include <components/settings/categories/shadows.hpp>''',
)
replace_exact(
    "components/sceneutil/shadow.cpp",
    '''        mShadowTechnique->setV33FarCascadeResolutionDivisor(
            static_cast<unsigned>(settings.mV33FarCascadeResolutionDivisor));''',
    '''        mShadowTechnique->setV33FarCascadeResolutionDivisor(
            static_cast<unsigned>(settings.mV33FarCascadeResolutionDivisor));
        mShadowTechnique->setV36AsyncGpuProfiler(Settings::cells().mV36AsyncGpuProfiler);''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v36gpuprofiler.hpp>''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''                sd = new ShadowData(vdd, sm_i, numShadowMapsPerLight);''',
    '''                sd = new ShadowData(vdd, sm_i, numShadowMapsPerLight);
                if (_v36AsyncGpuProfiler)
                    Debug::V36GpuProfiler::attachCamera(
                        *sd->_camera, "shadow_cascade_" + std::to_string(sm_i));''',
)

replace_exact(
    "apps/openmw/mwrender/water.cpp",
    '''#include <components/resource/imagemanager.hpp>''',
    '''#include <components/debug/v36gpuprofiler.hpp>
#include <components/resource/imagemanager.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/water.cpp",
    '''            camera->setName("RefractionCamera");''',
    '''            camera->setName("RefractionCamera");
            if (Settings::cells().mV36AsyncGpuProfiler)
                Debug::V36GpuProfiler::attachCamera(*camera, "water_refraction");''',
)
replace_exact(
    "apps/openmw/mwrender/water.cpp",
    '''            camera->setName("ReflectionCamera");''',
    '''            camera->setName("ReflectionCamera");
            if (Settings::cells().mV36AsyncGpuProfiler)
                Debug::V36GpuProfiler::attachCamera(*camera, "water_reflection");''',
)

replace_exact(
    "apps/openmw/mwrender/postprocessor.cpp",
    '''#include <components/files/conversion.hpp>''',
    '''#include <components/debug/v36gpuprofiler.hpp>
#include <components/files/conversion.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/postprocessor.cpp",
    '''        mHUDCamera->setCullCallback(new HUDCullCallback);''',
    '''        mHUDCamera->setCullCallback(new HUDCullCallback);
        if (Settings::cells().mV36AsyncGpuProfiler)
            Debug::V36GpuProfiler::attachCamera(*mHUDCamera, "postprocess_hud_composite");''',
)

replace_exact(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v36gpuprofiler.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''                auto& v3PostFxWriter = Debug::V3Diagnostics::postFxWriter();
                if (v3PostFxWriter.enabled())''',
    '''                const std::string v36GpuPassName = std::string("postfx/")
                    + (node.mHandle ? node.mHandle->getName() : std::string("unknown")) + "/"
                    + std::to_string(passIndex);
                Debug::V36GpuProfiler::ScopedPass v36GpuPass(renderInfo, v36GpuPassName);
                auto& v3PostFxWriter = Debug::V3Diagnostics::postFxWriter();
                if (v3PostFxWriter.enabled())''',
)

print("V3.6 delayed asynchronous GPU pass profiler source patch completed successfully.")
