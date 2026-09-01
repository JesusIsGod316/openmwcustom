import os
import subprocess
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel: str, old: str, new: str, expected: int = 1) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.21 CP2 fairness match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.21 CP2 fairness patched {rel} ({count} match(es))")


# CP2 repair: classification exists only on transient ICO jobs. The shared mode
# flag is initialized once during ICO setup and is immutable for the process.
# No renderable scene object receives class userdata.
helper_path = ROOT / "components/resource/v321classifiedcompileset.hpp"
if helper_path.exists():
    raise RuntimeError("components/resource/v321classifiedcompileset.hpp unexpectedly already exists")
helper_path.write_text(
    r'''#pragma once

#include <atomic>
#include <mutex>

#include <osg/Node>
#include <osgUtil/IncrementalCompileOperation>

namespace Resource
{
    enum class V321CompileClass : unsigned char
    {
        Unknown = 0,
        ObjectPaging,
        Terrain,
        GenericModel,
    };

    inline std::atomic_bool& v321CP2FairnessFlag()
    {
        static std::atomic_bool enabled{ false };
        return enabled;
    }

    inline void initializeV321CP2Fairness(bool enabled)
    {
        static std::once_flag once;
        std::call_once(once, [enabled] { v321CP2FairnessFlag().store(enabled, std::memory_order_release); });
    }

    inline bool v321CP2FairnessEnabled()
    {
        return v321CP2FairnessFlag().load(std::memory_order_acquire);
    }

    class V321ClassifiedCompileSet final : public osgUtil::IncrementalCompileOperation::CompileSet
    {
    public:
        V321ClassifiedCompileSet(osg::Node* subgraph, V321CompileClass compileClass)
            : CompileSet(subgraph)
            , mCompileClass(compileClass)
        {
        }

        V321CompileClass compileClass() const { return mCompileClass; }

    protected:
        ~V321ClassifiedCompileSet() override = default;

    private:
        V321CompileClass mCompileClass;
    };

    inline V321CompileClass getV321CompileClass(
        const osgUtil::IncrementalCompileOperation::CompileSet* compileSet)
    {
        const auto* classified = dynamic_cast<const V321ClassifiedCompileSet*>(compileSet);
        return classified ? classified->compileClass() : V321CompileClass::Unknown;
    }
}
''',
    encoding="utf-8",
    newline="\n",
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    "#include <components/resource/resourcesystem.hpp>\n",
    "#include <components/resource/resourcesystem.hpp>\n"
    "#include <components/resource/v321classifiedcompileset.hpp>\n",
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    "#include <components/resource/scenemanager.hpp>\n",
    "#include <components/resource/scenemanager.hpp>\n"
    "#include <components/resource/v321classifiedcompileset.hpp>\n",
)
replace_exact(
    "components/terrain/chunkmanager.cpp",
    "#include <components/resource/scenemanager.hpp>\n",
    "#include <components/resource/scenemanager.hpp>\n"
    "#include <components/resource/v321classifiedcompileset.hpp>\n",
)
replace_exact(
    "components/resource/scenemanager.hpp",
    '#include "resourcemanager.hpp"\n',
    '#include "resourcemanager.hpp"\n#include "v321classifiedcompileset.hpp"\n',
)

# Resolve the mode exactly once at ICO setup. Every producer and the engine read
# this same immutable flag; no site re-parses the environment independently.
replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''        mObjects = std::make_unique<Objects>(mResourceSystem, sceneRoot, unrefQueue);

        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)''',
    '''        mObjects = std::make_unique<Objects>(mResourceSystem, sceneRoot, unrefQueue);

        const int v321CP2FairnessMode = [] {
            const int configured = static_cast<int>(Settings::cells().mV321CP2FairnessMode);
            const char* value = std::getenv("OPENMW_V321_CP2_FAIRNESS");
            if (value == nullptr || *value == '\\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 0 && parsed <= 1 ? parsed : configured;
        }();
        Resource::initializeV321CP2Fairness(v321CP2FairnessMode == 1);

        if (getenv("OPENMW_DONT_PRECOMPILE") == nullptr)''',
)

# ObjectPaging already builds a specialized StateToCompile map. Mode125 takes
# the literal original CP1 statements; Mode129 changes only the job type.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);
            compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
            ico->add(compileSet, false);''',
    '''            if (Resource::v321CP2FairnessEnabled())
            {
                auto compileSet = new Resource::V321ClassifiedCompileSet(
                    group, Resource::V321CompileClass::ObjectPaging);
                compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                ico->add(compileSet, false);
            }
            else
            {
                auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);
                compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                ico->add(compileSet, false);
            }''',
)

# Terrain's off branch is the original CP1 add(geometry). The classified path
# uses ICO's public add(CompileSet*) overload, which builds the normal map.
replace_exact(
    "components/terrain/chunkmanager.cpp",
    "            mSceneManager->getIncrementalCompileOperation()->add(geometry);",
    '''            if (Resource::v321CP2FairnessEnabled())
            {
                auto compileSet = new Resource::V321ClassifiedCompileSet(
                    geometry, Resource::V321CompileClass::Terrain);
                mSceneManager->getIncrementalCompileOperation()->add(compileSet);
            }
            else
                mSceneManager->getIncrementalCompileOperation()->add(geometry);''',
)

# GenericModel identity is passed only by the cell-preload producer. Other
# callers remain Unknown, and Mode125 still calls getTemplate(mesh) exactly.
replace_exact(
    "components/resource/scenemanager.hpp",
    "        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path, bool compile = true);",
    '''        osg::ref_ptr<const osg::Node> getTemplate(VFS::Path::NormalizedView path, bool compile = true,
            V321CompileClass compileClass = V321CompileClass::Unknown);''',
)
replace_exact(
    "components/resource/scenemanager.cpp",
    "    osg::ref_ptr<const osg::Node> SceneManager::getTemplate(VFS::Path::NormalizedView path, bool compile)",
    '''    osg::ref_ptr<const osg::Node> SceneManager::getTemplate(
        VFS::Path::NormalizedView path, bool compile, V321CompileClass compileClass)''',
)
replace_exact(
    "components/resource/scenemanager.cpp",
    '''            if (compile && mIncrementalCompileOperation)
                mIncrementalCompileOperation->add(loaded);
            else
                loaded->getBound();''',
    '''            if (compile && mIncrementalCompileOperation)
            {
                if (v321CP2FairnessEnabled() && compileClass != V321CompileClass::Unknown)
                {
                    auto compileSet = new V321ClassifiedCompileSet(loaded, compileClass);
                    mIncrementalCompileOperation->add(compileSet);
                }
                else
                    mIncrementalCompileOperation->add(loaded);
            }
            else
                loaded->getBound();''',
)
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    "                    mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));",
    '''                    if (Resource::v321CP2FairnessEnabled())
                        mPreloadedObjects.insert(mSceneManager->getTemplate(
                            mesh, true, Resource::V321CompileClass::GenericModel));
                    else
                        mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));''',
)

settings_anchor = '''        SettingValue<int> mV321AdaptiveDebtRepayPerFrame{
            mIndex, "V3", "v3.21 adaptive debt repay per frame", makeClampSanitizerInt(0, 4) };'''
replace_exact(
    "components/settings/categories/cells.hpp",
    settings_anchor,
    settings_anchor
    + '''
        // V3.21 CP2 class-aware completion fairness/dephasing. Default off.
        SettingValue<int> mV321CP2FairnessMode{
            mIndex, "V3", "v3.21 CP2 fairness mode", makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV321CP2ServiceSetsPerFrame{
            mIndex, "V3", "v3.21 CP2 service sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2ClassBurstSetsPerFrame{
            mIndex, "V3", "v3.21 CP2 class burst sets per frame", makeClampSanitizerInt(1, 16) };
        SettingValue<int> mV321CP2MaxDeferredFrames{
            mIndex, "V3", "v3.21 CP2 max deferred frames", makeClampSanitizerInt(1, 120) };
        SettingValue<int> mV321CP2ForcedSets{
            mIndex, "V3", "v3.21 CP2 forced sets", makeClampSanitizerInt(0, 8) };
        SettingValue<int> mV321CP2DeficitCap{
            mIndex, "V3", "v3.21 CP2 deficit cap", makeClampSanitizerInt(1, 32) };''',
)

replace_exact(
    "files/settings-default.cfg",
    "v3.21 adaptive debt repay per frame = 1",
    '''v3.21 adaptive debt repay per frame = 1

# V3.21 CP2 fairness/dephasing. Mode 0 preserves CP1/V3.20 behavior.
# Mode 1 stages only fully completed ICO sets; WorkQueue preparation is untouched.
v3.21 CP2 fairness mode = 0
v3.21 CP2 service sets per frame = 6
v3.21 CP2 class burst sets per frame = 3
v3.21 CP2 max deferred frames = 6
v3.21 CP2 forced sets = 2
v3.21 CP2 deficit cap = 12''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    "#include <deque>\n#include <mutex>\n",
    "#include <array>\n#include <deque>\n#include <mutex>\n"
    "#include <components/resource/v321classifiedcompileset.hpp>\n",
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''        if (v321CompletionGovernorMode > 0)
        {
            osgUtil::IncrementalCompileOperation* const ico = mViewer->getIncrementalCompileOperation();''',
    '''        const bool v321CP2FairnessMode = Resource::v321CP2FairnessEnabled();

        if (v321CompletionGovernorMode > 0 || v321CP2FairnessMode)
        {
            osgUtil::IncrementalCompileOperation* const ico = mViewer->getIncrementalCompileOperation();''',
)

replace_exact(
    "apps/openmw/engine.cpp",
    '''                static std::deque<V321DeferredCompileSet> deferred;
                static V321CompletionCounters counters;

                const unsigned int baseBudget''',
    '''                static std::deque<V321DeferredCompileSet> deferred;
                static V321CompletionCounters counters;

                // Separate queues are used only by CP2 Mode129. CP1 modes keep
                // their original single FIFO and admission behavior unchanged.
                static std::array<std::deque<V321DeferredCompileSet>, 4> cp2Deferred;
                static std::array<unsigned int, 4> cp2Deficit = { 0, 0, 0, 0 };
                static std::array<std::uint64_t, 4> cp2Seen = { 0, 0, 0, 0 };
                static std::array<std::uint64_t, 4> cp2Admitted = { 0, 0, 0, 0 };
                static unsigned int cp2Cursor = 0;

                const unsigned int baseBudget''',
)

engine_path = ROOT / "apps/openmw/engine.cpp"
engine_text = engine_path.read_text(encoding="utf-8")
start_marker = "                unsigned int completedThisFrame = 0;"
end_marker = '\n                if (stats->collectStats("resource"))\n                {\n                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);'
if engine_text.count(start_marker) != 1 or engine_text.count(end_marker) != 1:
    raise RuntimeError("V3.21 CP2 fairness could not locate unique CP1 admission/stat boundary")
start = engine_text.index(start_marker)
end = engine_text.index(end_marker, start)

new_admission = r'''                unsigned int completedThisFrame = 0;
                unsigned int admittedThisFrame = 0;
                unsigned int forcedThisFrame = 0;
                unsigned int oldestAge = 0;
                unsigned int v321DeferredDepthForStats = 0;
                unsigned int cp2ActiveClasses = 0;
                std::array<unsigned int, 4> cp2AdmittedThisFrame = { 0, 0, 0, 0 };

                {
                    std::lock_guard<OpenThreads::Mutex> lock(*ico->getCompiledMutex());
                    osgUtil::IncrementalCompileOperation::CompileSets& completed = ico->getCompiled();

                    auto cp2ClassIndex = [](Resource::V321CompileClass value) -> unsigned int {
                        switch (value)
                        {
                            case Resource::V321CompileClass::ObjectPaging: return 0;
                            case Resource::V321CompileClass::Terrain: return 1;
                            case Resource::V321CompileClass::GenericModel: return 2;
                            case Resource::V321CompileClass::Unknown:
                            default: return 3;
                        }
                    };

                    while (!completed.empty())
                    {
                        osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> set = completed.front();
                        completed.pop_front();
                        if (v321CP2FairnessMode)
                        {
                            const unsigned int index
                                = cp2ClassIndex(Resource::getV321CompileClass(set.get()));
                            cp2Deferred[index].push_back(V321DeferredCompileSet{ set, frameNumber });
                            ++cp2Seen[index];
                        }
                        else
                            deferred.push_back(V321DeferredCompileSet{ set, frameNumber });
                        ++completedThisFrame;
                        ++counters.mCompletedSeen;
                    }

                    if (v321CP2FairnessMode)
                    {
                        const unsigned int serviceBudget
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ServiceSetsPerFrame);
                        const unsigned int configuredClassBurst
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ClassBurstSetsPerFrame);
                        const unsigned int maxDeferredFrames
                            = static_cast<unsigned int>(Settings::cells().mV321CP2MaxDeferredFrames);
                        const unsigned int forcedBudget
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ForcedSets);
                        const unsigned int deficitCap
                            = static_cast<unsigned int>(Settings::cells().mV321CP2DeficitCap);
                        constexpr std::array<unsigned int, 4> quantum = { 2, 2, 1, 1 };

                        auto totalDeferred = [&]() -> unsigned int {
                            unsigned int total = 0;
                            for (const auto& queue : cp2Deferred)
                                total += static_cast<unsigned int>(queue.size());
                            return total;
                        };
                        auto recomputeOldestAge = [&]() -> unsigned int {
                            unsigned int age = 0;
                            for (const auto& queue : cp2Deferred)
                                if (!queue.empty())
                                    age = std::max(age, frameNumber - queue.front().mFirstDeferredFrame);
                            return age;
                        };
                        auto admitClass = [&](unsigned int index, bool forced) {
                            completed.push_back(cp2Deferred[index].front().mSet);
                            cp2Deferred[index].pop_front();
                            ++admittedThisFrame;
                            ++cp2AdmittedThisFrame[index];
                            ++cp2Admitted[index];
                            if (forced)
                                ++forcedThisFrame;
                        };

                        for (unsigned int i = 0; i < 4; ++i)
                        {
                            if (!cp2Deferred[i].empty())
                                ++cp2ActiveClasses;
                            cp2Deficit[i] = std::min(deficitCap, cp2Deficit[i] + quantum[i]);
                        }

                        if (cp2ActiveClasses == 1)
                        {
                            // A single active class gets the full budget: fairness
                            // must not become an artificial producer throttle.
                            for (unsigned int i = 0; i < 4; ++i)
                            {
                                if (cp2Deferred[i].empty())
                                    continue;
                                const unsigned int count
                                    = std::min(serviceBudget, static_cast<unsigned int>(cp2Deferred[i].size()));
                                for (unsigned int n = 0; n < count; ++n)
                                    admitClass(i, false);
                                cp2Deficit[i] = cp2Deficit[i] > count ? cp2Deficit[i] - count : 0;
                                cp2Cursor = (i + 1) % 4;
                                break;
                            }
                        }
                        else if (cp2ActiveClasses > 1)
                        {
                            const unsigned int classBurst = std::min(configuredClassBurst, serviceBudget);
                            unsigned int refills = 0;
                            while (admittedThisFrame < serviceBudget && totalDeferred() > 0)
                            {
                                int selected = -1;
                                for (unsigned int offset = 0; offset < 4; ++offset)
                                {
                                    const unsigned int index = (cp2Cursor + offset) % 4;
                                    if (!cp2Deferred[index].empty()
                                        && cp2AdmittedThisFrame[index] < classBurst
                                        && cp2Deficit[index] > 0)
                                    {
                                        selected = static_cast<int>(index);
                                        break;
                                    }
                                }
                                if (selected < 0)
                                {
                                    bool classCapBlocksAll = true;
                                    for (unsigned int i = 0; i < 4; ++i)
                                        if (!cp2Deferred[i].empty() && cp2AdmittedThisFrame[i] < classBurst)
                                        {
                                            classCapBlocksAll = false;
                                            break;
                                        }
                                    if (classCapBlocksAll || refills >= 4)
                                        break;
                                    for (unsigned int i = 0; i < 4; ++i)
                                        cp2Deficit[i] = std::min(deficitCap, cp2Deficit[i] + quantum[i]);
                                    ++refills;
                                    continue;
                                }
                                const unsigned int index = static_cast<unsigned int>(selected);
                                admitClass(index, false);
                                --cp2Deficit[index];
                                cp2Cursor = (index + 1) % 4;
                            }
                        }

                        // Global age escape ignores class cap/deficit so no class
                        // can starve indefinitely. Extra service itself is bounded.
                        for (unsigned int forced = 0; forced < forcedBudget; ++forced)
                        {
                            int oldestClass = -1;
                            unsigned int oldestClassAge = 0;
                            for (unsigned int i = 0; i < 4; ++i)
                            {
                                if (cp2Deferred[i].empty())
                                    continue;
                                const unsigned int age = frameNumber - cp2Deferred[i].front().mFirstDeferredFrame;
                                if (age >= maxDeferredFrames && (oldestClass < 0 || age > oldestClassAge))
                                {
                                    oldestClass = static_cast<int>(i);
                                    oldestClassAge = age;
                                }
                            }
                            if (oldestClass < 0)
                                break;
                            admitClass(static_cast<unsigned int>(oldestClass), true);
                        }

                        oldestAge = recomputeOldestAge();
                        v321DeferredDepthForStats = totalDeferred();
                        counters.mAdmitted += admittedThisFrame;
                        counters.mForced += forcedThisFrame;
                        counters.mPeakDeferred = std::max<std::uint64_t>(
                            counters.mPeakDeferred, v321DeferredDepthForStats);
                    }
                    else
                    {
                        if (!deferred.empty())
                            oldestAge = frameNumber - deferred.front().mFirstDeferredFrame;

                        unsigned int admissionBudget = baseBudget;
                        unsigned int adaptiveDebtRepaid = 0;
                        static double adaptiveFrameEmaMs = 0.0;
                        static unsigned int adaptiveDebt = 0;
                        const double previousFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();

                        if (v321CompletionGovernorMode == 2)
                        {
                            const double targetMs
                                = static_cast<double>(Settings::cells().mV321AdaptiveTargetMilliseconds);
                            const double alpha
                                = static_cast<double>(Settings::cells().mV321AdaptiveFrameEmaAlpha);
                            const unsigned int minBudget
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMin);
                            const unsigned int configuredMax
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMax);
                            const unsigned int maxBudget = std::max(minBudget, configuredMax);
                            const unsigned int debtCap
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtCap);
                            const unsigned int repayCap
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtRepayPerFrame);
                            if (previousFrameMs > 0.0)
                                adaptiveFrameEmaMs = adaptiveFrameEmaMs <= 0.0
                                    ? previousFrameMs
                                    : adaptiveFrameEmaMs * (1.0 - alpha) + previousFrameMs * alpha;
                            if (previousFrameMs >= targetMs + 6.0)
                                admissionBudget = minBudget;
                            else if (previousFrameMs >= targetMs + 2.0)
                                admissionBudget = std::max(minBudget, baseBudget > 1 ? baseBudget - 1 : 1u);
                            else if (previousFrameMs > 0.0 && previousFrameMs <= targetMs - 3.0
                                && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                                admissionBudget = maxBudget;
                            else
                                admissionBudget = std::min(maxBudget, std::max(minBudget, baseBudget));
                            if (!deferred.empty() && admissionBudget < baseBudget && adaptiveDebt < debtCap)
                            {
                                const unsigned int withheld = baseBudget - admissionBudget;
                                adaptiveDebt += std::min(withheld, debtCap - adaptiveDebt);
                            }
                            if (!deferred.empty() && adaptiveDebt > 0 && previousFrameMs > 0.0
                                && previousFrameMs <= targetMs - 2.0
                                && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                            {
                                const unsigned int room
                                    = maxBudget > admissionBudget ? maxBudget - admissionBudget : 0;
                                adaptiveDebtRepaid = std::min(repayCap, std::min(adaptiveDebt, room));
                                admissionBudget += adaptiveDebtRepaid;
                                adaptiveDebt -= adaptiveDebtRepaid;
                            }
                        }

                        const unsigned int adaptiveBudgetBeforeForced = admissionBudget;
                        const unsigned int maxDeferredFrames
                            = static_cast<unsigned int>(Settings::cells().mV321MaxDeferredFrames);
                        const unsigned int forcedBudget
                            = static_cast<unsigned int>(Settings::cells().mV321ForcedMergeSets);
                        if (deferred.size() > admissionBudget && oldestAge >= maxDeferredFrames)
                            admissionBudget += forcedBudget;
                        while (admittedThisFrame < admissionBudget && !deferred.empty())
                        {
                            completed.push_back(deferred.front().mSet);
                            deferred.pop_front();
                            ++admittedThisFrame;
                        }
                        if (admittedThisFrame > adaptiveBudgetBeforeForced)
                            forcedThisFrame = admittedThisFrame - adaptiveBudgetBeforeForced;
                        counters.mAdmitted += admittedThisFrame;
                        counters.mForced += forcedThisFrame;
                        if (deferred.size() > counters.mPeakDeferred)
                            counters.mPeakDeferred = deferred.size();
                        v321DeferredDepthForStats = static_cast<unsigned int>(deferred.size());

                        if (stats->collectStats("resource") && v321CompletionGovernorMode == 2)
                        {
                            stats->setAttribute(frameNumber, "V321 Adaptive PreviousFrameMs", previousFrameMs);
                            stats->setAttribute(frameNumber, "V321 Adaptive FrameEmaMs", adaptiveFrameEmaMs);
                            stats->setAttribute(frameNumber, "V321 Adaptive MergeBudget", adaptiveBudgetBeforeForced);
                            stats->setAttribute(frameNumber, "V321 Adaptive Debt", adaptiveDebt);
                            stats->setAttribute(frameNumber, "V321 Adaptive DebtRepaid", adaptiveDebtRepaid);
                        }
                    }
                }
'''
engine_text = engine_text[:start] + new_admission + engine_text[end:]
engine_text = engine_text.replace(
    'frameNumber, "V321 Completion Deferred", static_cast<double>(deferred.size()));',
    'frameNumber, "V321 Completion Deferred", static_cast<double>(v321DeferredDepthForStats));',
    1,
)
stats_anchor = '                    stats->setAttribute(frameNumber, "V321 Completion OldestAge", oldestAge);'
if engine_text.count(stats_anchor) != 1:
    raise RuntimeError("V3.21 CP2 completion stats anchor drifted")
engine_text = engine_text.replace(
    stats_anchor,
    stats_anchor
    + r'''
                    if (v321CP2FairnessMode)
                    {
                        stats->setAttribute(frameNumber, "V321 CP2 ActiveClasses", cp2ActiveClasses);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Deferred", cp2Deferred[0].size());
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Deferred", cp2Deferred[1].size());
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Deferred", cp2Deferred[2].size());
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Deferred", cp2Deferred[3].size());
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging AdmittedFrame", cp2AdmittedThisFrame[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain AdmittedFrame", cp2AdmittedThisFrame[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel AdmittedFrame", cp2AdmittedThisFrame[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown AdmittedFrame", cp2AdmittedThisFrame[3]);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Seen", cp2Seen[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Seen", cp2Seen[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Seen", cp2Seen[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Seen", cp2Seen[3]);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Admitted", cp2Admitted[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Admitted", cp2Admitted[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Admitted", cp2Admitted[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Admitted", cp2Admitted[3]);
                    }''',
    1,
)
replace_id = "openmw-custom-v3.21-cp1-adaptive-governor"
if engine_text.count(replace_id) != 1:
    raise RuntimeError("V3.21 CP2 engine identity anchor drifted")
engine_text = engine_text.replace(
    replace_id, replace_id + " / openmw-custom-v3.21-cp2-fairness-dephasing", 1
)
engine_path.write_text(engine_text, encoding="utf-8", newline="\n")

launcher_path = ROOT / "tools/v3/launchers/V3_Lab.ps1"
launcher = launcher_path.read_text(encoding="utf-8")
menu127 = next((line for line in launcher.splitlines() if line.startswith("Write-Host '127 = V3.21 CP1 adaptive")), None)
if not menu127:
    raise RuntimeError("V3.21 CP2 launcher lost Mode127 menu anchor")
launcher = launcher.replace(
    menu127,
    menu127 + "\nWrite-Host '128 = reserved; no CP1 combination implemented'"
    + "\nWrite-Host '129 = V3.21 CP2 class-aware completion fairness/dephasing'",
    1,
)
choice_line = next((line for line in launcher.splitlines() if "Read-Host 'Enter 1 through 127'" in line), None)
if not choice_line or ",'125','126','127'))" not in choice_line:
    raise RuntimeError("V3.21 CP2 launcher choice anchor drifted")
new_choice = choice_line.replace(
    "Read-Host 'Enter 1 through 127'", "Read-Host 'Enter a listed mode (1-127 or 129)'", 1
).replace(",'125','126','127'))", ",'125','126','127','129'))", 1)
launcher = launcher.replace(choice_line, new_choice, 1)
replace_default = "$V321CompletionGovernor = '0'\n$V320EngineLuaFastPaths = '0'"
if launcher.count(replace_default) != 1:
    raise RuntimeError("V3.21 CP2 launcher default anchor drifted")
launcher = launcher.replace(
    replace_default,
    "$V321CompletionGovernor = '0'\n$V321CP2Fairness = '0'\n$V320EngineLuaFastPaths = '0'",
    1,
)
line125 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'125'"))
line127 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'127'"))
control_body = line125[line125.index("{") + 1 : line125.rindex("}")].strip()
if "v321-cp1-v320-control" not in control_body or "$V321CompletionGovernor = '0'" not in control_body:
    raise RuntimeError("V3.21 CP2 Mode125 control body drifted")
mode129_body = control_body.replace("v321-cp1-v320-control", "v321-cp2-fairness-dephasing", 1)
mode129 = "        '129' { " + mode129_body + "; $V321CP2Fairness = '1' }"
launcher = launcher.replace(line127 + "\n", line127 + "\n" + mode129 + "\n", 1)
manifest_anchor = '    "v321_completion_governor=$V321CompletionGovernor",'
launcher = launcher.replace(
    manifest_anchor, manifest_anchor + '\n    "v321_cp2_fairness=$V321CP2Fairness",', 1
)
env_anchor = "    $env:OPENMW_V321_COMPLETION_GOVERNOR = $V321CompletionGovernor"
launcher = launcher.replace(
    env_anchor, env_anchor + "\n    $env:OPENMW_V321_CP2_FAIRNESS = $V321CP2Fairness", 1
)
cleanup_anchor = "    Remove-Item Env:OPENMW_V321_COMPLETION_GOVERNOR -ErrorAction SilentlyContinue"
launcher = launcher.replace(
    cleanup_anchor,
    "    Remove-Item Env:OPENMW_V321_CP2_FAIRNESS -ErrorAction SilentlyContinue\n" + cleanup_anchor,
    1,
)
launcher_path.write_text(launcher, encoding="utf-8", newline="\n")

readme_path = ROOT / "V3-LAB-README.txt"
readme = readme_path.read_text(encoding="utf-8")
readme += r'''

V3.21 CP2 — class-aware completion fairness/dephasing
=====================================================

Mode 129 keeps the exact Mode 125 V3.20 foundation and leaves the CP1 completion
governor OFF. It independently stages only fully completed ICO CompileSets and
services ObjectPaging, Terrain, GenericModel, and Unknown source classes through
bounded class-aware queues. WorkQueue threads, cell/terrain/model preload work,
prediction, object construction, and ICO compile production remain unthrottled.
Class identity lives only on transient derived CompileSet jobs. Mode 125 creates
ordinary OSG CompileSets through the original CP1 submission branches; neither
mode writes CP2 class userdata into cached or renderable scene objects.

When multiple source classes compete, CP2 uses bounded deficit round-robin with
nonzero quanta (ObjectPaging=2, Terrain=2, GenericModel=1, Unknown=1) and a
per-class burst cap to dephase same-class completion storms. When only one class
has completed work, that class may use the full service budget so fairness does
not become an artificial throttle. A separate global maximum-age escape admits
overdue work regardless of deficit or class burst cap, with bounded forced
service. Default settings use service=6 sets/frame, mixed-class burst=3,
max-age=6 frames, forced escape=2, and deficit cap=12.

Groundcover is intentionally not invented as an ICO class because its current
path does not submit CompileSets through ICO. Mode 128 remains unimplemented.
CP2 is orthogonal to the periodic ~24-26 ms other_ms investigation and to CP1
adaptive Mode 127. Resource stats expose class queue depth, seen/admitted totals,
per-frame service, active-class count, global deferred depth, and oldest age.
'''
readme_path.write_text(readme, encoding="utf-8", newline="\n")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

engine = engine_path.read_text(encoding="utf-8")
cells = (ROOT / "components/settings/categories/cells.hpp").read_text(encoding="utf-8")
settings = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
launcher = launcher_path.read_text(encoding="utf-8")
readme = readme_path.read_text(encoding="utf-8")
for marker in (
    "openmw-custom-v3.21-cp2-fairness-dephasing",
    "v321CP2FairnessEnabled",
    "V321 CP2 ObjectPaging Deferred",
    "V321 CP2 Terrain Deferred",
    "V321 CP2 GenericModel Deferred",
    "V321 CP2 Unknown Deferred",
    "cp2ClassIndex",
    "cp2Deficit",
    "v321CompletionGovernorMode > 0 || v321CP2FairnessMode",
):
    if marker not in engine:
        raise RuntimeError(f"V3.21 CP2 engine missing marker: {marker}")
for marker in (
    "mV321CP2FairnessMode",
    "mV321CP2ServiceSetsPerFrame",
    "mV321CP2ClassBurstSetsPerFrame",
    "mV321CP2MaxDeferredFrames",
    "mV321CP2ForcedSets",
    "mV321CP2DeficitCap",
):
    if marker not in cells:
        raise RuntimeError(f"V3.21 CP2 settings registration missing marker: {marker}")
for marker in (
    "v3.21 CP2 fairness mode = 0",
    "v3.21 CP2 service sets per frame = 6",
    "v3.21 CP2 class burst sets per frame = 3",
    "v3.21 CP2 max deferred frames = 6",
):
    if marker not in settings:
        raise RuntimeError(f"V3.21 CP2 default settings missing marker: {marker}")
for marker in (
    "129 = V3.21 CP2 class-aware completion fairness/dephasing",
    "v321-cp2-fairness-dephasing",
    "v321_cp2_fairness=$V321CP2Fairness",
    "OPENMW_V321_CP2_FAIRNESS",
    "Enter a listed mode (1-127 or 129)",
):
    if marker not in launcher:
        raise RuntimeError(f"V3.21 CP2 launcher missing marker: {marker}")
line129 = next(line for line in launcher.splitlines() if line.lstrip().startswith("'129'"))
if "$V321CompletionGovernor = '0'" not in line129 or "$V321CP2Fairness = '1'" not in line129:
    raise RuntimeError("V3.21 Mode129 is not orthogonal to CP1 or did not enable CP2")
if any(line.lstrip().startswith("'128'") for line in launcher.splitlines()):
    raise RuntimeError("V3.21 Mode128 was accidentally implemented")
for marker in (
    "Mode 129 keeps the exact Mode 125 V3.20 foundation",
    "bounded deficit round-robin",
    "full service budget",
    "global maximum-age escape",
    "Mode 128 remains unimplemented",
    "transient derived CompileSet jobs",
    "CP2 class userdata",
):
    if marker not in readme:
        raise RuntimeError(f"V3.21 CP2 README missing marker: {marker}")

helper = helper_path.read_text(encoding="utf-8")
object_paging = (ROOT / "apps/openmw/mwrender/objectpaging.cpp").read_text(encoding="utf-8")
terrain = (ROOT / "components/terrain/chunkmanager.cpp").read_text(encoding="utf-8")
scene_manager = (ROOT / "components/resource/scenemanager.cpp").read_text(encoding="utf-8")
preloader = (ROOT / "apps/openmw/mwworld/cellpreloader.cpp").read_text(encoding="utf-8")
rendering_manager = (ROOT / "apps/openmw/mwrender/renderingmanager.cpp").read_text(encoding="utf-8")

for marker in (
    "class V321ClassifiedCompileSet",
    "initializeV321CP2Fairness",
    "v321CP2FairnessEnabled",
    "dynamic_cast<const V321ClassifiedCompileSet*>",
):
    if marker not in helper:
        raise RuntimeError(f"V3.21 CP2 transient CompileSet helper missing marker: {marker}")
if "Resource::initializeV321CP2Fairness(v321CP2FairnessMode == 1);" not in rendering_manager:
    raise RuntimeError("V3.21 CP2 shared fairness mode was not initialized at ICO setup")
if object_paging.count("auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);") != 1:
    raise RuntimeError("V3.21 CP2 Mode125 ObjectPaging path lost the original CP1 CompileSet allocation")
if object_paging.count("ico->add(compileSet, false);") != 2:
    raise RuntimeError("V3.21 CP2 ObjectPaging classified/control add paths are incomplete")
if terrain.count("mSceneManager->getIncrementalCompileOperation()->add(geometry);") != 1:
    raise RuntimeError("V3.21 CP2 Mode125 Terrain path lost the original CP1 ICO add statement")
if scene_manager.count("mIncrementalCompileOperation->add(loaded);") != 1:
    raise RuntimeError("V3.21 CP2 Mode125 GenericModel path lost the original CP1 ICO add statement")
if preloader.count("mPreloadedObjects.insert(mSceneManager->getTemplate(mesh));") != 1:
    raise RuntimeError("V3.21 CP2 Mode125 preloader path lost the original CP1 getTemplate call")

for forbidden in (
    "markV321CompileClass",
    "OpenMW.V321CompileClass.",
    "osg::DummyObject",
):
    for rel, text in (
        ("helper", helper),
        ("objectpaging", object_paging),
        ("terrain", terrain),
        ("scenemanager", scene_manager),
        ("cellpreloader", preloader),
        ("engine", engine),
    ):
        if forbidden in text:
            raise RuntimeError(f"V3.21 CP2 forbidden scene-graph classification marker {forbidden!r} in {rel}")

print("V3.21 CP2 repaired transient class-aware fairness/dephasing scheduler applied")
