import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP1 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP1 patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# V3.21 CP1 fixed completed-work admission governor.
#
# Mode 0 is an exact V3.20 control. Mode 1 does NOT throttle async workers or
# terrain/object preparation. It only:
#   1. bounds GL objects drained by OSG's IncrementalCompileOperation per frame;
#   2. bounds completed CompileSets admitted to Viewer::updateTraversal() for
#      main-thread merge/install, with FIFO ordering and bounded-age extra
#      progress to prevent indefinite starvation.
# -----------------------------------------------------------------------------
setting_anchor = '''        // V3.20: optionally force a focus refresh when the main camera contract changes.
        // Disabled by default so the promoted fixed-cadence P0 path remains exact.
        SettingValue<bool> mV320AdaptiveFocusCadence{
            mIndex, "V3", "v3.20 adaptive focus cadence" };'''
replace_exact(
    "components/settings/categories/cells.hpp",
    setting_anchor,
    setting_anchor
    + '''
        // V3.21 CP1: downstream completed-work admission only. Worker preparation
        // remains unthrottled. Mode 0=off/exact V3.20, 1=fixed bounded governor.
        SettingValue<int> mV321CompletionGovernorMode{
            mIndex, "V3", "v3.21 completion governor mode", makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV321CompileObjectsPerFrame{
            mIndex, "V3", "v3.21 compile objects per frame", makeClampSanitizerInt(1, 20) };
        SettingValue<int> mV321MergeSetsPerFrame{
            mIndex, "V3", "v3.21 merge sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321MaxDeferredFrames{
            mIndex, "V3", "v3.21 max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321ForcedMergeSets{
            mIndex, "V3", "v3.21 forced merge sets", makeClampSanitizerInt(0, 8) };
        SettingValue<float> mV321CompileMinimumMilliseconds{
            mIndex, "V3", "v3.21 compile minimum milliseconds", makeClampSanitizerFloat(0.1, 4.0) };
        SettingValue<float> mV321CompileConservativeRatio{
            mIndex, "V3", "v3.21 compile conservative ratio", makeClampSanitizerFloat(0.1, 1.0) };''',
)

default_anchor = "v3.20 adaptive focus cadence = false"
replace_exact(
    "files/settings-default.cfg",
    default_anchor,
    default_anchor
    + '''

# V3.21 CP1 completed-work admission governor. Default off preserves the exact
# V3.20 foundation. Fixed mode keeps async preload/paging workers running and
# bounds only ICO GL compile drain plus completed CompileSet main-thread merges.
v3.21 completion governor mode = 0
v3.21 compile objects per frame = 4
v3.21 merge sets per frame = 2
v3.21 max deferred frames = 4
v3.21 forced merge sets = 2
v3.21 compile minimum milliseconds = 1.0
v3.21 compile conservative ratio = 0.25''',
)

ico_anchor = '''            ico->setTargetFrameRate(compileTarget);
            if (static_cast<int>(Settings::cells().mV38CompilePacingMode) > 0)
            {
                ico->setMaximumNumOfObjectsToCompilePerFrame(maxObjectsPerFrame);
                ico->setConservativeTimeRatio(conservativeRatio);
            }
            mViewer->setIncrementalCompileOperation(ico);'''
ico_replacement = '''            const int v321CompletionGovernorMode = [] {
                const int configured = static_cast<int>(Settings::cells().mV321CompletionGovernorMode);
                const char* value = std::getenv("OPENMW_V321_COMPLETION_GOVERNOR");
                if (value == nullptr || *value == '\\0')
                    return configured;
                const int parsed = std::atoi(value);
                return parsed >= 0 && parsed <= 1 ? parsed : configured;
            }();

            if (v321CompletionGovernorMode == 1)
            {
                // V3.21 CP1 deliberately undoes V3.8's aggressive 36 Hz spare-time
                // target for this mode. At an already-bound frame, ICO therefore
                // falls back to its bounded minimum instead of consuming several ms.
                compileTarget = configuredTarget;
                maxObjectsPerFrame
                    = static_cast<unsigned int>(Settings::cells().mV321CompileObjectsPerFrame);
                conservativeRatio
                    = static_cast<double>(Settings::cells().mV321CompileConservativeRatio);
                ico->setMinimumTimeAvailableForGLCompileAndDeletePerFrame(
                    static_cast<double>(Settings::cells().mV321CompileMinimumMilliseconds) / 1000.0);
            }

            ico->setTargetFrameRate(compileTarget);
            if (static_cast<int>(Settings::cells().mV38CompilePacingMode) > 0 || v321CompletionGovernorMode > 0)
            {
                ico->setMaximumNumOfObjectsToCompilePerFrame(maxObjectsPerFrame);
                ico->setConservativeTimeRatio(conservativeRatio);
            }
            mViewer->setIncrementalCompileOperation(ico);'''
replace_exact("apps/openmw/mwrender/renderingmanager.cpp", ico_anchor, ico_replacement)

include_anchor = "#include <cstdint>\n"
replace_exact(
    "apps/openmw/engine.cpp",
    include_anchor,
    include_anchor + "#include <deque>\n#include <mutex>\n",
)

traversal_anchor = '''    mViewer->eventTraversal();
    mViewer->updateTraversal();'''
traversal_replacement = r'''    mViewer->eventTraversal();

    // V3.21 CP1: OSG normally merges every fully compiled CompileSet during
    // Viewer::updateTraversal(). Keep the producer and GL compile paths active,
    // but hold completed sets in a FIFO and expose only a bounded slice to the
    // main-thread merge each frame. This is downstream admission, not WorkQueue
    // throttling. A small bounded-age supplement prevents indefinite starvation.
    static const int v321CompletionGovernorMode = [] {
        const int configured = static_cast<int>(Settings::cells().mV321CompletionGovernorMode);
        const char* value = std::getenv("OPENMW_V321_COMPLETION_GOVERNOR");
        if (value == nullptr || *value == '\0')
            return configured;
        const int parsed = std::atoi(value);
        return parsed >= 0 && parsed <= 1 ? parsed : configured;
    }();

    if (v321CompletionGovernorMode == 1)
    {
        osgUtil::IncrementalCompileOperation* const ico = mViewer->getIncrementalCompileOperation();
        if (ico != nullptr)
        {
            struct V321DeferredCompileSet
            {
                osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> mSet;
                unsigned int mFirstDeferredFrame = 0;
            };
            struct V321CompletionCounters
            {
                std::uint64_t mCompletedSeen = 0;
                std::uint64_t mAdmitted = 0;
                std::uint64_t mForced = 0;
                std::uint64_t mPeakDeferred = 0;
            };

            static std::deque<V321DeferredCompileSet> deferred;
            static V321CompletionCounters counters;

            const unsigned int baseBudget
                = static_cast<unsigned int>(Settings::cells().mV321MergeSetsPerFrame);
            const unsigned int maxDeferredFrames
                = static_cast<unsigned int>(Settings::cells().mV321MaxDeferredFrames);
            const unsigned int forcedBudget
                = static_cast<unsigned int>(Settings::cells().mV321ForcedMergeSets);

            unsigned int completedThisFrame = 0;
            unsigned int admittedThisFrame = 0;
            unsigned int forcedThisFrame = 0;
            unsigned int oldestAge = 0;

            {
                std::lock_guard<OpenThreads::Mutex> lock(*ico->getCompiledMutex());
                osgUtil::IncrementalCompileOperation::CompileSets& completed = ico->getCompiled();

                while (!completed.empty())
                {
                    deferred.push_back(V321DeferredCompileSet{ completed.front(), frameNumber });
                    completed.pop_front();
                    ++completedThisFrame;
                    ++counters.mCompletedSeen;
                }

                if (!deferred.empty())
                    oldestAge = frameNumber - deferred.front().mFirstDeferredFrame;

                unsigned int admissionBudget = baseBudget;
                if (deferred.size() > baseBudget && oldestAge >= maxDeferredFrames)
                    admissionBudget += forcedBudget;

                while (admittedThisFrame < admissionBudget && !deferred.empty())
                {
                    completed.push_back(deferred.front().mSet);
                    deferred.pop_front();
                    ++admittedThisFrame;
                }

                if (admittedThisFrame > baseBudget)
                    forcedThisFrame = admittedThisFrame - baseBudget;

                counters.mAdmitted += admittedThisFrame;
                counters.mForced += forcedThisFrame;
                if (deferred.size() > counters.mPeakDeferred)
                    counters.mPeakDeferred = deferred.size();
            }

            if (reportResource)
            {
                stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);
                stats->setAttribute(frameNumber, "V321 Completion Admitted", counters.mAdmitted);
                stats->setAttribute(frameNumber, "V321 Completion Forced", counters.mForced);
                stats->setAttribute(frameNumber, "V321 Completion PeakDeferred", counters.mPeakDeferred);
                stats->setAttribute(frameNumber, "V321 Completion CompletedThisFrame", completedThisFrame);
                stats->setAttribute(frameNumber, "V321 Completion AdmittedThisFrame", admittedThisFrame);
                stats->setAttribute(frameNumber, "V321 Completion Deferred",
                    static_cast<double>(deferred.size()));
                stats->setAttribute(frameNumber, "V321 Completion OldestAge", oldestAge);
            }
        }
    }

    mViewer->updateTraversal();'''
replace_exact("apps/openmw/engine.cpp", traversal_anchor, traversal_replacement)

identity_anchor = "openmw-custom-v3.20-cp1-focus"
replace_exact(
    "apps/openmw/engine.cpp",
    identity_anchor,
    identity_anchor + " / openmw-custom-v3.21-cp1-completion-governor",
)

for rel, required in {
    "components/settings/categories/cells.hpp": (
        "mV321CompletionGovernorMode",
        "mV321CompileObjectsPerFrame",
        "mV321MergeSetsPerFrame",
        "mV321MaxDeferredFrames",
    ),
    "files/settings-default.cfg": (
        "v3.21 completion governor mode = 0",
        "v3.21 compile objects per frame = 4",
        "v3.21 merge sets per frame = 2",
    ),
    "apps/openmw/mwrender/renderingmanager.cpp": (
        "OPENMW_V321_COMPLETION_GOVERNOR",
        "mV321CompileMinimumMilliseconds",
        "mV321CompileConservativeRatio",
    ),
    "apps/openmw/engine.cpp": (
        "openmw-custom-v3.21-cp1-completion-governor",
        "V321 Completion Deferred",
        "getCompiledMutex",
        "mV321ForcedMergeSets",
    ),
}.items():
    text = (ROOT / rel).read_text(encoding="utf-8")
    for marker in required:
        if marker not in text:
            raise RuntimeError(f"V3.21 CP1 generated source missing {marker!r} in {rel}")

print("V3.21 CP1 fixed completed-work admission governor applied")
