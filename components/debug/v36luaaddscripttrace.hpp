#ifndef OPENMW_COMPONENTS_DEBUG_V36LUAADDSCRIPTTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36LUAADDSCRIPTTRACE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36LuaAddScriptTrace
{
    enum class Phase : std::size_t
    {
        HiddenSetup,
        CachedChunkLoad,
        Environment,
        CommonPackages,
        ContainerPackages,
        RequireSetup,
        ModuleLoad,
        ScriptBody,
        HandlerExtraction,
        InterfaceRegistration,
        Count,
    };

    inline constexpr std::size_t PhaseCount = static_cast<std::size_t>(Phase::Count);

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_LUA_ADDSCRIPT_FILE",
            "frame,epoch_ms,container,script,total_ms,hidden_setup_ms,cached_chunk_load_ms,environment_ms,"
            "common_packages_ms,container_packages_ms,require_setup_ms,module_load_ms,script_body_ms,"
            "handler_extraction_ms,interface_registration_ms,other_ms");
        return writer;
    }

    class Recorder
    {
    public:
        void begin(std::string_view container, std::string_view script)
        {
            mContainer.assign(container);
            mScript.assign(script);
            mStart = V3Diagnostics::Clock::now();
        }

        void add(Phase phase, double milliseconds)
        {
            mPhases[static_cast<std::size_t>(phase)] += milliseconds;
        }

        void finish()
        {
            const double total = V3Diagnostics::elapsedMs(mStart);
            if (total < 0.25)
                return;
            // Module loads occur inside the top-level script call. Keep the CSV phases mutually exclusive.
            const std::size_t moduleIndex = static_cast<std::size_t>(Phase::ModuleLoad);
            const std::size_t bodyIndex = static_cast<std::size_t>(Phase::ScriptBody);
            mPhases[bodyIndex] = std::max(0.0, mPhases[bodyIndex] - mPhases[moduleIndex]);
            double accounted = 0.0;
            for (double value : mPhases)
                accounted += value;
            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
                << V3Diagnostics::csvQuote(mContainer) << ',' << V3Diagnostics::csvQuote(mScript) << ','
                << std::fixed << std::setprecision(4) << total;
            for (double value : mPhases)
                row << ',' << value;
            row << ',' << std::max(0.0, total - accounted);
            writer().writeLine(row.str());
        }

    private:
        std::string mContainer;
        std::string mScript;
        V3Diagnostics::Clock::time_point mStart{};
        std::array<double, PhaseCount> mPhases{};
    };

    inline thread_local Recorder* sRecorder = nullptr;

    class ScriptScope
    {
    public:
        ScriptScope(std::string_view container, std::string_view script)
            : mPrevious(sRecorder)
            , mActive(writer().enabled())
        {
            if (mActive)
            {
                sRecorder = &mRecorder;
                mRecorder.begin(container, script);
            }
        }

        ~ScriptScope()
        {
            if (mActive)
            {
                mRecorder.finish();
                sRecorder = mPrevious;
            }
        }

        ScriptScope(const ScriptScope&) = delete;
        ScriptScope& operator=(const ScriptScope&) = delete;

    private:
        Recorder mRecorder;
        Recorder* mPrevious;
        bool mActive;
    };

    class PhaseScope
    {
    public:
        explicit PhaseScope(Phase phase)
            : mRecorder(sRecorder)
            , mPhase(phase)
            , mStart(mRecorder ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }

        ~PhaseScope()
        {
            if (mRecorder)
                mRecorder->add(mPhase, V3Diagnostics::elapsedMs(mStart));
        }

        PhaseScope(const PhaseScope&) = delete;
        PhaseScope& operator=(const PhaseScope&) = delete;

    private:
        Recorder* mRecorder;
        Phase mPhase;
        V3Diagnostics::Clock::time_point mStart;
    };

    inline void add(Phase phase, V3Diagnostics::Clock::time_point start)
    {
        if (sRecorder)
            sRecorder->add(phase, V3Diagnostics::elapsedMs(start));
    }
}

#endif
