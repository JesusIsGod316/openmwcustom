import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.16 resource-sweep match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 idle resource sweep patched {rel} ({count} match(es))")


# Independent default-off control so Mode86/87 preserve the existing V3.15 path.
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };''',
    '''        SettingValue<int> mV315AdaptiveCompileGovernor{ mIndex, "V3", "v3.15 adaptive compile governor",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV316IdleResourceSweep{ mIndex, "V3", "v3.16 idle resource sweep" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''# 0=off/inherited V3.8 pacing, 1=balanced, 2=aggressive tail protection.
v3.15 adaptive compile governor = 0

[Cells]''',
    '''# 0=off/inherited V3.8 pacing, 1=balanced, 2=aggressive tail protection.
v3.15 adaptive compile governor = 0

# V3.16: move periodic ResourceSystem cache maintenance off the shared preload
# work queue onto one dedicated idle-priority worker. This keeps the same cache
# expiry semantics while reducing contention with gameplay-critical preloading.
v3.16 idle resource sweep = false

[Cells]''',
)

# A dedicated queue is important: lowering the priority of a shared WorkQueue
# thread would persist and could accidentally throttle later paging/preload work.
replace_exact(
    "apps/openmw/mwworld/cellpreloader.hpp",
    '''        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        double mExpiryDelay;''',
    '''        osg::ref_ptr<SceneUtil::WorkQueue> mWorkQueue;
        osg::ref_ptr<SceneUtil::WorkQueue> mV316ResourceSweepQueue;
        double mExpiryDelay;''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>''',
    '''#include <components/misc/pathhelpers.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/misc/thread.hpp>
#include <components/misc/strings/algorithm.hpp>''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        UpdateCacheItem(Resource::ResourceSystem* resourceSystem, double referenceTime)
            : mReferenceTime(referenceTime)
            , mResourceSystem(resourceSystem)
        {
        }

        void doWork() override { mResourceSystem->updateCache(mReferenceTime); }

    private:
        double mReferenceTime;
        Resource::ResourceSystem* mResourceSystem;''',
    '''        UpdateCacheItem(Resource::ResourceSystem* resourceSystem, double referenceTime, bool idlePriority)
            : mReferenceTime(referenceTime)
            , mResourceSystem(resourceSystem)
            , mIdlePriority(idlePriority)
        {
        }

        void doWork() override
        {
            if (mIdlePriority)
                Misc::setCurrentThreadIdlePriority();
            mResourceSystem->updateCache(mReferenceTime);
        }

    private:
        double mReferenceTime;
        Resource::ResourceSystem* mResourceSystem;
        bool mIdlePriority;''',
)

replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        , mLoadedTerrainTimestamp(0.0)
    {
    }''',
    '''        , mLoadedTerrainTimestamp(0.0)
    {
        if (static_cast<bool>(Settings::cells().mV316IdleResourceSweep))
            mV316ResourceSweepQueue = new SceneUtil::WorkQueue(1);
    }''',
)

# V3.7 already made the cadence pressure-aware. Preserve that exact cadence and
# expiry behavior; only change where the maintenance work executes.
replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    '''        if (timestamp - mLastResourceCacheUpdate > v37ResourceSweepSeconds
            && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {
            // the resource cache is cleared from the worker thread so that we're not holding up the main thread with
            // delete operations
            mUpdateCacheItem = new UpdateCacheItem(mResourceSystem, timestamp);
            mWorkQueue->addWorkItem(mUpdateCacheItem, true);
            mLastResourceCacheUpdate = timestamp;
        }''',
    '''        if (timestamp - mLastResourceCacheUpdate > v37ResourceSweepSeconds
            && (!mUpdateCacheItem || mUpdateCacheItem->isDone()))
        {
            // V3.16 balanced/aggressive modes isolate periodic cache maintenance
            // from the shared preload queue. The dedicated thread is permanently
            // idle-priority and handles no paging-critical work.
            const bool v316IdleSweep = static_cast<bool>(Settings::cells().mV316IdleResourceSweep)
                && mV316ResourceSweepQueue;
            mUpdateCacheItem = new UpdateCacheItem(mResourceSystem, timestamp, v316IdleSweep);
            if (v316IdleSweep)
                mV316ResourceSweepQueue->addWorkItem(mUpdateCacheItem);
            else
                mWorkQueue->addWorkItem(mUpdateCacheItem, true);
            mLastResourceCacheUpdate = timestamp;
        }''',
)

launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")
# The SFX metadata-frontload layer is applied before this one. Anchor on that
# exact composed state so changes in patch ordering fail closed instead of
# silently dropping one of the V3.16 controls.
old_default = "$V316SfxPredecodeWorkers = '0'\n$V316SfxMetadataFrontload = 'false'\n$RendererProfiling"
new_default = "$V316SfxPredecodeWorkers = '0'\n$V316SfxMetadataFrontload = 'false'\n$V316IdleResourceSweep = 'false'\n$RendererProfiling"
if text.count(old_default) != 1:
    raise RuntimeError("V3.16 idle resource sweep launcher default anchor mismatch")
text = text.replace(old_default, new_default, 1)

lines = text.splitlines()
for mode in ("88", "89"):
    matches = [i for i, line in enumerate(lines) if line.startswith(f"        '{mode}' {{")]
    if len(matches) != 1:
        raise RuntimeError(f"V3.16 idle resource sweep expected one Mode{mode} line, found {len(matches)}")
    i = matches[0]
    line = lines[i]
    if not line.rstrip().endswith("}"):
        raise RuntimeError(f"V3.16 Mode{mode} launcher line has unexpected layout")
    lines[i] = line.rstrip()[:-1].rstrip() + "; $V316IdleResourceSweep = 'true' }"
text = "\n".join(lines) + "\n"

setting_anchor = "Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' $V315AdaptiveCompileGovernor"
if text.count(setting_anchor) != 1:
    raise RuntimeError("V3.16 idle resource sweep launcher setting anchor mismatch")
text = text.replace(
    setting_anchor,
    setting_anchor + "\n        Set-IniValue $SettingsPath 'V3' 'v3.16 idle resource sweep' $V316IdleResourceSweep",
    1,
)

text = text.replace(
    "Write-Host ' 88 = V3.16 balanced hitch: audio64 + 256/384MB decoded SFX retention'",
    "Write-Host ' 88 = V3.16 balanced: audio/SFX retention + idle resource maintenance'",
    1,
)
text = text.replace(
    "Write-Host ' 89 = V3.16 aggressive: audio128 + SFX retention + 384MB idle predecode'",
    "Write-Host ' 89 = V3.16 aggressive: balanced + 384MB idle SFX predecode'",
    1,
)
launcher.write_text(text, encoding="utf-8", newline="\n")

marker = ROOT / "V3.16-HITCH-LAYER.txt"
with marker.open("a", encoding="utf-8", newline="\n") as f:
    f.write("mode88_idle_resource_sweep=1\n")
    f.write("mode89_idle_resource_sweep=1\n")
    f.write("resource_sweep_dedicated_workers=1_idle_priority\n")
    f.write("resource_sweep_expiry_semantics=unchanged\n")

print("V3.16 dedicated idle-priority resource maintenance applied")
