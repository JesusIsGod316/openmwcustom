#ifndef OPENMW_COMPONENTS_DEBUG_V35LUALOADTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V35LUALOADTRACE_H

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V35LuaLoadTrace
{
    using Clock = V3Diagnostics::Clock;

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V35_LUA_LOAD_FILE",
            "frame,epoch_ms,container,total_ms,prepare_ms,interfaces_ms,add_scripts_ms,init_load_ms,timers_ms,"
            "heap_ms,tracker_ms,script_count,saved_script_count,timer_count,max_script_ms,max_script_path");
        return writer;
    }

    inline bool enabled()
    {
        return writer().enabled();
    }

    inline double elapsedMs(const Clock::time_point& start)
    {
        return V3Diagnostics::elapsedMs(start);
    }

    inline void record(std::string_view container, double totalMs, double prepareMs, double interfacesMs,
        double addScriptsMs, double initLoadMs, double timersMs, double heapMs, double trackerMs,
        unsigned scriptCount, unsigned savedScriptCount, unsigned timerCount, double maxScriptMs,
        std::string_view maxScriptPath)
    {
        // First materialization is interesting even when individually modest, but keep the stream bounded.
        // The threshold is intentionally below the existing 8 ms Lua slow-phase threshold so we can see
        // which subphase is accumulating across many containers on a grid-crossing frame.
        if (!writer().enabled() || totalMs < 2.0)
            return;

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
            << V3Diagnostics::csvQuote(container) << ',' << std::fixed << std::setprecision(3) << totalMs << ','
            << prepareMs << ',' << interfacesMs << ',' << addScriptsMs << ',' << initLoadMs << ',' << timersMs << ','
            << heapMs << ',' << trackerMs << ',' << scriptCount << ',' << savedScriptCount << ',' << timerCount << ','
            << maxScriptMs << ',' << V3Diagnostics::csvQuote(maxScriptPath);
        writer().writeLine(row.str());
    }
}

#endif
