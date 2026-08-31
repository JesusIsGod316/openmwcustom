import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.20 focus match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.20 focus patched {rel} ({count} match(es))")


setting_anchor = '''        // V3.19 stable gaming: promoted focus temporal-coherence cadence.
        SettingValue<int> mV319FocusCadence{
            mIndex, "V3", "v3.19 focus cadence", makeClampSanitizerInt(1, 3) };'''
replace_exact(
    "components/settings/categories/cells.hpp",
    setting_anchor,
    setting_anchor
    + '''
        // V3.20: optionally force a focus refresh when the main camera contract changes.
        // Disabled by default so the promoted fixed-cadence P0 path remains exact.
        SettingValue<bool> mV320AdaptiveFocusCadence{
            mIndex, "V3", "v3.20 adaptive focus cadence" };''',
)

default_anchor = "v3.19 focus cadence = 2"
replace_exact(
    "files/settings-default.cfg",
    default_anchor,
    default_anchor
    + '''

# V3.20 optional camera-dirty forcing. False preserves the exact promoted P0
# fixed cadence. When true, view/projection changes force an immediate refresh;
# v3.19 focus cadence remains the hard maximum staleness bound.
v3.20 adaptive focus cadence = false''',
)

include_anchor = "#include <chrono>\n"
replace_exact("apps/openmw/engine.cpp", include_anchor, include_anchor + "#include <cstdint>\n")

old_block = r'''        // V3.19: focus-object GUI refresh can reuse the previous result for a small
        // number of ordinary gameplay frames. Activation/input queries remain untouched.
        // GUI mode always refreshes every frame to preserve mouse/console behavior.
        static const unsigned v319FocusCadence = [] {
            // Stable gaming reads the native setting by default. The environment
            // variable remains an explicit lab override so old benchmark modes keep
            // exact causal control without rewriting the user's settings.cfg.
            const unsigned configured = static_cast<unsigned>(Settings::cells().mV319FocusCadence);
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : configured;
        }();
        if (v319FocusCadence <= 1 || mWindowManager->isGuiMode() || frameNumber % v319FocusCadence == 0)
            mWorld->updateFocusObject();'''

new_block = r'''        // V3.20 preserves the exact V3.19 fixed-cadence path by default. Optional
        // adaptive mode only adds an immediate refresh when the main camera's view or
        // projection contract changes. The configured cadence remains a hard maximum
        // staleness bound for moving world objects while the camera is stationary.
        // GUI mode always refreshes. Activation/input queries remain untouched.
        static const unsigned v319FocusCadence = [] {
            const unsigned configured = static_cast<unsigned>(Settings::cells().mV319FocusCadence);
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : configured;
        }();
        static const bool v320AdaptiveFocusCadence = [] {
            const bool configured = Settings::cells().mV320AdaptiveFocusCadence;
            const char* value = std::getenv("OPENMW_V320_FOCUS_ADAPTIVE");
            return value == nullptr || *value == '\0' ? configured : std::atoi(value) != 0;
        }();

        struct V320FocusCounters
        {
            std::uint64_t mAttempted = 0;
            std::uint64_t mExecuted = 0;
            std::uint64_t mCadenceSkipped = 0;
            std::uint64_t mDirtyForced = 0;
            std::uint64_t mFixedAttempted = 0;
            std::uint64_t mFixedExecuted = 0;
            std::uint64_t mAdaptiveAttempted = 0;
            std::uint64_t mAdaptiveExecuted = 0;
        };
        static V320FocusCounters v320FocusCounters;

        bool cameraDirty = false;
        if (v320AdaptiveFocusCadence)
        {
            static bool initialized = false;
            static osg::Matrixd previousView;
            static osg::Matrixd previousProjection;
            const osg::Camera* const camera = mViewer->getCamera();
            const osg::Matrixd currentView = camera->getViewMatrix();
            const osg::Matrixd currentProjection = camera->getProjectionMatrix();
            cameraDirty = !initialized || currentView != previousView || currentProjection != previousProjection;
            previousView = currentView;
            previousProjection = currentProjection;
            initialized = true;
        }

        const bool guiForced = mWindowManager->isGuiMode();
        const bool cadenceDue = v319FocusCadence <= 1 || frameNumber % v319FocusCadence == 0;
        const bool dirtyForced = v320AdaptiveFocusCadence && cameraDirty && !guiForced && !cadenceDue;
        const bool execute = cadenceDue || guiForced || (v320AdaptiveFocusCadence && cameraDirty);

        ++v320FocusCounters.mAttempted;
        if (v320AdaptiveFocusCadence)
            ++v320FocusCounters.mAdaptiveAttempted;
        else
            ++v320FocusCounters.mFixedAttempted;

        if (execute)
        {
            mWorld->updateFocusObject();
            ++v320FocusCounters.mExecuted;
            if (v320AdaptiveFocusCadence)
                ++v320FocusCounters.mAdaptiveExecuted;
            else
                ++v320FocusCounters.mFixedExecuted;
            if (dirtyForced)
                ++v320FocusCounters.mDirtyForced;
        }
        else
            ++v320FocusCounters.mCadenceSkipped;

        // Aggregate-only attribution. No per-frame file logging is added; values are
        // published only when the existing resource-stat collector is enabled.
        if (stats->collectStats("resource"))
        {
            stats->setAttribute(frameNumber, "V320 Focus Attempted", v320FocusCounters.mAttempted);
            stats->setAttribute(frameNumber, "V320 Focus Executed", v320FocusCounters.mExecuted);
            stats->setAttribute(frameNumber, "V320 Focus CadenceSkipped", v320FocusCounters.mCadenceSkipped);
            stats->setAttribute(frameNumber, "V320 Focus DirtyForced", v320FocusCounters.mDirtyForced);
            stats->setAttribute(frameNumber, "V320 Focus FixedAttempted", v320FocusCounters.mFixedAttempted);
            stats->setAttribute(frameNumber, "V320 Focus FixedExecuted", v320FocusCounters.mFixedExecuted);
            stats->setAttribute(frameNumber, "V320 Focus AdaptiveAttempted", v320FocusCounters.mAdaptiveAttempted);
            stats->setAttribute(frameNumber, "V320 Focus AdaptiveExecuted", v320FocusCounters.mAdaptiveExecuted);
        }'''
replace_exact("apps/openmw/engine.cpp", old_block, new_block)

identity_anchor = (
    "openmw-custom-v3.17 / openmw-custom-v3.18-render-scale-p0 / "
    "openmw-custom-v3.19-cpu-p0 / openmw-custom-v3.19-p0-stable-gaming"
)
replace_exact("apps/openmw/engine.cpp", identity_anchor, identity_anchor + " / openmw-custom-v3.20-cp1-focus")

for rel, required in {
    "components/settings/categories/cells.hpp": (
        "mV320AdaptiveFocusCadence",
        '"V3", "v3.20 adaptive focus cadence"',
    ),
    "files/settings-default.cfg": (
        "v3.19 focus cadence = 2",
        "v3.20 adaptive focus cadence = false",
    ),
    "apps/openmw/engine.cpp": (
        "OPENMW_V320_FOCUS_ADAPTIVE",
        "V320 Focus DirtyForced",
        "currentView != previousView",
        "const bool cadenceDue",
        "Activation/input queries remain untouched.",
        "openmw-custom-v3.20-cp1-focus",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"V3.20 focus source missing {marker!r} in {rel}")

print("V3.20 CP1 adaptive focus forcing and aggregate attribution applied")
