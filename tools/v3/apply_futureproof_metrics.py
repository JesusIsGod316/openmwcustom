from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"futureproof metrics patched {rel}")


# ---------------------------------------------------------------------------
# Optional every-frame telemetry. Hitch telemetry remains sparse/lightweight;
# OPENMW_V3_FRAME_FILE provides statistically valid frametime percentiles and
# variance without requiring the very large OSG text log.
# ---------------------------------------------------------------------------
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        void ensureStream()
        {
            if (mStreamAttempted)''',
    '''        void ensureAllFrameStream()
        {
            if (mAllFrameStreamAttempted)
                return;
            mAllFrameStreamAttempted = true;

            const char* raw = std::getenv("OPENMW_V3_FRAME_FILE");
            if (!raw)
                return;
            const std::string_view setting(raw);
            if (setting.empty() || setting == "0" || setting == "off" || setting == "false")
                return;

            mAllFrameStream.open(std::filesystem::u8path(std::string(setting)), std::ios::out | std::ios::trunc);
            if (!mAllFrameStream.is_open())
                return;
            mAllFrameStream
                << "frame,epoch_ms,wall_ms,input_ms,sound_ms,lua_sync_ms,state_ms,script_ms,mechanics_ms,physics_ms,"
                   "world_ms,gui_ms,focus_ms,accounted_ms,other_ms\\n";
        }

        void emitAllFrame(double wallMs, double accountedMs, double otherMs)
        {
            ensureAllFrameStream();
            if (!mAllFrameStream.is_open())
                return;

            const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                mFrameStartSystem.time_since_epoch()).count();
            mAllFrameStream << mFrame << ',' << epochMs << ',' << std::fixed << std::setprecision(3) << wallMs;
            for (double value : mStageMs)
                mAllFrameStream << ',' << value;
            mAllFrameStream << ',' << accountedMs << ',' << otherMs << '\\n';
            if (++mAllFrameLinesSinceFlush >= FlushInterval)
            {
                mAllFrameStream.flush();
                mAllFrameLinesSinceFlush = 0;
            }
        }

        void ensureStream()
        {
            if (mStreamAttempted)''',
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());

            const bool hitch = wallMs >= HitchThresholdMs;''',
    '''            const double largestStage = *std::max_element(mStageMs.begin(), mStageMs.end());

            emitAllFrame(wallMs, accountedMs, otherMs);

            const bool hitch = wallMs >= HitchThresholdMs;''',
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        bool mStarted = false;
        bool mStreamAttempted = false;
        unsigned mFrame = 0;
        unsigned mLinesSinceFlush = 0;''',
    '''        bool mStarted = false;
        bool mStreamAttempted = false;
        bool mAllFrameStreamAttempted = false;
        unsigned mFrame = 0;
        unsigned mLinesSinceFlush = 0;
        unsigned mAllFrameLinesSinceFlush = 0;''',
)
replace_once(
    "components/debug/v3hitchtelemetry.hpp",
    '''        std::string mPath;
        std::ofstream mStream;''',
    '''        std::string mPath;
        std::ofstream mStream;
        std::ofstream mAllFrameStream;''',
)

# ---------------------------------------------------------------------------
# Bridge major synchronous paths into the nested cross-thread trace. Existing
# subsystem CSVs remain unchanged; the trace answers what was on the critical
# path and what worker activity overlapped it.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_interior", cellName);''',
    '''        Debug::V3Diagnostics::writeEvent("change_to_interior", cellName);
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_to_interior", cellName, 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_interior", cellName);''',
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Debug::V3Diagnostics::writeEvent("change_to_exterior");
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_exterior", "exterior");''',
    '''        Debug::V3Diagnostics::writeEvent("change_to_exterior");
        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_to_exterior", "exterior", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_to_exterior", "exterior");''',
)
replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");
        const int halfGridSize''',
    '''        Debug::V3Diagnostics::TraceScope v3Trace("transition", "change_cell_grid", "exterior_grid", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer v3TransitionTimer(
            Debug::V3Diagnostics::transitionWriter(), "change_cell_grid", "exterior_grid");
        const int halfGridSize''',
)
replace_once(
    "components/detournavigator/navigatorimpl.cpp",
    '''        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::navWriter(), "navigator_wait", "", 0.1);
        mNavMeshManager.wait(waitConditionType, listener);''',
    '''        Debug::V3Diagnostics::TraceScope trace("nav", "navigator_wait", "", 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::navWriter(), "navigator_wait", "", 0.1);
        mNavMeshManager.wait(waitConditionType, listener);''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::resourceWriter(), "resource_cache_update", "all_managers", 0.5);''',
    '''        Debug::V3Diagnostics::TraceScope trace("resource", "resource_cache_update", "all_managers", 0.5);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::resourceWriter(), "resource_cache_update", "all_managers", 0.5);''',
)

print("V3 Future-Proof Metrics source patch completed successfully.")
