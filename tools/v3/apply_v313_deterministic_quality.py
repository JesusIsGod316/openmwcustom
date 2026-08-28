import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.13 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.13 patched {rel} ({count} match(es))")


def replace_region(rel, start, end, replacement):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    a = text.find(start)
    if a < 0:
        raise RuntimeError(f"{rel}: V3.13 start marker not found")
    b = text.find(end, a)
    if b < 0:
        raise RuntimeError(f"{rel}: V3.13 end marker not found")
    path.write_text(text[:a] + replacement + text[b:], encoding="utf-8", newline="\n")
    print(f"V3.13 replaced region in {rel}")


# -----------------------------------------------------------------------------
# V3.13 deterministic ObjectPaging quality repair
#
# V3.11/V3.12 validation exposed a first-writer quality alias: ChunkId contains
# only (center,size,activeGrid), while compile=false demand fallback and the strong
# compile=true prepared path populate that same key. A weak demand node can therefore
# satisfy a later strong preload lookup forever, preventing POSTTRANSFORM/shared-state
# preparation from reaching the live cache. V3.13 records preparation quality per
# ChunkId and allows the existing preload worker to rebuild/promote a stronger node.
#
# No demand call waits for a repair. Existing GenericObjectCache replacement is
# mutex-protected and stores osg::ref_ptr, so replacing the cache entry is safe:
# existing readers retain the old node while future lookups get the promoted node.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV312SpatialBatchMode{ mIndex, "V3", "v3.12 spatial batch mode",
            makeClampSanitizerInt(0, 1) };''',
    '''        SettingValue<int> mV312SpatialBatchMode{ mIndex, "V3", "v3.12 spatial batch mode",
            makeClampSanitizerInt(0, 1) };
        SettingValue<int> mV313ChunkQualityMode{ mIndex, "V3", "v3.13 chunk quality mode",
            makeClampSanitizerInt(0, 2) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.12 spatial batch mode = 0

[Cells]''',
    '''v3.12 spatial batch mode = 0

# V3.13 deterministic ObjectPaging cache quality repair.
# 0=exact inherited first-writer behavior; 1=repair weak/lower-quality cached active
# chunks on strong compile=true preload; 2=mode1 + require matching V3.12 spatial
# prepared signature. Demand fallback remains nonblocking in every mode.
v3.13 chunk quality mode = 0

[Cells]''',
)

# Explicit includes make the V3.13 metadata independent of transitive headers.
replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''#include <mutex>''',
    '''#include <atomic>
#include <map>
#include <mutex>
#include <set>''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        std::atomic_uint64_t mV311DemandFallbacks{ 0 };

        std::mutex mRefTrackerMutex;''',
    '''        std::atomic_uint64_t mV311DemandFallbacks{ 0 };

        struct V313ChunkQuality
        {
            unsigned char mPrepareMode = 0;
            unsigned char mSpatialMode = 0;
        };
        mutable std::mutex mV313ChunkQualityMutex;
        std::map<ChunkId, V313ChunkQuality> mV313ChunkQualities;
        std::set<ChunkId> mV313StrongUpgradeInFlight;
        std::atomic_uint64_t mV313WeakCacheHitOnStrongPrepare{ 0 };
        std::atomic_uint64_t mV313UpgradeBuilt{ 0 };
        std::atomic_uint64_t mV313UpgradeInstalled{ 0 };
        std::atomic_uint64_t mV313UpgradeCoalesced{ 0 };

        std::mutex mRefTrackerMutex;''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.hpp",
    '''        void clear();

        /// Must be called after clear() before rendering starts.''',
    '''        void clear();
        void clearCache() override;

        /// Must be called after clear() before rendering starts.''',
)

# Replace the generated V3.11/V3.12 getChunk implementation as one unit. This keeps
# all inherited diagnostics but fixes the cache-quality alias and install race.
getchunk_start = '''    osg::ref_ptr<osg::Node> ObjectPaging::getChunk(float size, const osg::Vec2f& center, unsigned char /*lod*/,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {'''
getchunk_end = '''    namespace
    {
        class CanOptimizeCallback'''
getchunk_replacement = r'''    osg::ref_ptr<osg::Node> ObjectPaging::getChunk(float size, const osg::Vec2f& center, unsigned char /*lod*/,
        unsigned int lodFlags, bool activeGrid, const osg::Vec3f& viewPoint, bool compile)
    {
        if (activeGrid && !mActiveGrid)
            return nullptr;

        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);
        const int v313QualityMode = static_cast<int>(Settings::cells().mV313ChunkQualityMode);

        // Mode0 is the inherited V3.12/V3.11 first-writer path. Keep it isolated
        // so all historical modes remain a valid behavioral/performance control.
        if (v313QualityMode == 0)
        {
            if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            {
                if (v311PrepareMode > 0 && activeGrid && !compile)
                {
                    std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                    if (mV311PreparedActiveChunks.contains(id))
                        mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
                }
                return static_cast<osg::Node*>(obj.get());
            }

            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.erase(id);
            }

            const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
                activeGrid ? "active_grid" : "distant", 0.25);
            osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
            mCache->addEntryToObjectCache(id, node.get());

            if (v311PrepareMode > 0 && activeGrid && compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.insert(id);
                mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
            }
            return node;
        }

        const unsigned char v313RequestedPrepareMode
            = activeGrid && compile && v311PrepareMode > 0 ? static_cast<unsigned char>(v311PrepareMode) : 0;
        const unsigned char v313RequestedSpatialMode
            = activeGrid && compile && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0 ? 1 : 0;
        const V313ChunkQuality v313RequestedQuality{ v313RequestedPrepareMode, v313RequestedSpatialMode };

        const auto v313QualitySatisfies = [&](const V313ChunkQuality& have, const V313ChunkQuality& need) {
            if (have.mPrepareMode < need.mPrepareMode)
                return false;
            if (v313QualityMode >= 2 && need.mPrepareMode > 0 && have.mSpatialMode != need.mSpatialMode)
                return false;
            return true;
        };

        osg::ref_ptr<osg::Object> cached = mCache->getRefFromObjectCache(id);
        bool v313RepairBuild = false;
        if (cached)
        {
            bool v313CachedSatisfies = true;
            if (v313QualityMode > 0 && v313RequestedPrepareMode > 0)
            {
                std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
                const auto it = mV313ChunkQualities.find(id);
                const V313ChunkQuality have = it != mV313ChunkQualities.end() ? it->second : V313ChunkQuality{};
                v313CachedSatisfies = v313QualitySatisfies(have, v313RequestedQuality);
                if (!v313CachedSatisfies)
                {
                    mV313WeakCacheHitOnStrongPrepare.fetch_add(1, std::memory_order_relaxed);
                    if (!mV313StrongUpgradeInFlight.insert(id).second)
                    {
                        mV313UpgradeCoalesced.fetch_add(1, std::memory_order_relaxed);
                        return static_cast<osg::Node*>(cached.get());
                    }
                    v313RepairBuild = true;
                }
            }

            if (v313CachedSatisfies)
            {
                if (v311PrepareMode > 0 && activeGrid && !compile)
                {
                    std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                    if (mV311PreparedActiveChunks.contains(id))
                        mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
                }
                return static_cast<osg::Node*>(cached.get());
            }
        }
        else if (v313QualityMode > 0)
        {
            // Generic cache expiry/removal does not know about V3.13's side table.
            // A real cache miss is authoritative and makes any old quality record stale.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            mV313ChunkQualities.erase(id);
            mV313StrongUpgradeInFlight.erase(id);
        }

        if (v311PrepareMode > 0 && activeGrid && !compile)
        {
            mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.erase(id);
        }

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(),
            v313RepairBuild ? "object_chunk_quality_upgrade" : "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);

        const V313ChunkQuality v313BuiltQuality{
            activeGrid && compile && v311PrepareMode > 0 ? static_cast<unsigned char>(v311PrepareMode) : 0,
            activeGrid && compile && static_cast<int>(Settings::cells().mV312SpatialBatchMode) > 0 ? 1 : 0 };
        if (v313RepairBuild)
            mV313UpgradeBuilt.fetch_add(1, std::memory_order_relaxed);

        if (v313QualityMode > 0)
        {
            // Strong-wins installation. A cheap demand miss may have started before a
            // strong worker finished; it must never overwrite the stronger live node.
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            const osg::ref_ptr<osg::Object> current = mCache->getRefFromObjectCache(id);
            const auto currentIt = mV313ChunkQualities.find(id);
            const V313ChunkQuality currentQuality
                = current && currentIt != mV313ChunkQualities.end() ? currentIt->second : V313ChunkQuality{};

            if (current && v313QualitySatisfies(currentQuality, v313BuiltQuality)
                && (currentQuality.mPrepareMode > v313BuiltQuality.mPrepareMode
                    || (currentQuality.mPrepareMode == v313BuiltQuality.mPrepareMode
                        && (v313QualityMode < 2 || currentQuality.mSpatialMode == v313BuiltQuality.mSpatialMode))))
            {
                // Preserve the already-installed equal-or-stronger node. This is the
                // race that prevents a late compile=false build from downgrading cache quality.
                if (currentQuality.mPrepareMode > 0 && activeGrid)
                {
                    std::lock_guard<std::mutex> preparedLock(mV311PreparedActiveMutex);
                    mV311PreparedActiveChunks.insert(id);
                }
                mV313StrongUpgradeInFlight.erase(id);
                return static_cast<osg::Node*>(current.get());
            }

            mCache->addEntryToObjectCache(id, node.get());
            mV313ChunkQualities[id] = v313BuiltQuality;
            mV313StrongUpgradeInFlight.erase(id);
            if (v313RepairBuild)
                mV313UpgradeInstalled.fetch_add(1, std::memory_order_relaxed);
        }
        else
            mCache->addEntryToObjectCache(id, node.get());

        if (v311PrepareMode > 0 && activeGrid && compile)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;
    }

'''
replace_region("apps/openmw/mwrender/objectpaging.cpp", getchunk_start, getchunk_end, getchunk_replacement)

# Full cache clears must also clear side metadata; otherwise a later cache miss can
# carry stale quality/"prepared" identity. Individual cache expiry/removal is already
# repaired lazily by the authoritative miss path above.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''    bool ObjectPaging::unlockCache()
    {
        if (!mRefTrackerLocked)
            return false;
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            mRefTrackerLocked = false;
            if (mRefTracker == mRefTrackerNew)
                return false;
            else
                mRefTracker = mRefTrackerNew;
        }
        mCache->clear();
        return true;
    }''',
    '''    void ObjectPaging::clearCache()
    {
        mCache->clear();
        if (static_cast<int>(Settings::cells().mV313ChunkQualityMode) > 0)
        {
            {
                std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
                mV313ChunkQualities.clear();
                mV313StrongUpgradeInFlight.clear();
            }
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                mV311PreparedActiveChunks.clear();
            }
        }
    }

    bool ObjectPaging::unlockCache()
    {
        if (!mRefTrackerLocked)
            return false;
        {
            std::lock_guard<std::mutex> lock(mRefTrackerMutex);
            mRefTrackerLocked = false;
            if (mRefTracker == mRefTrackerNew)
                return false;
            else
                mRefTracker = mRefTrackerNew;
        }
        clearCache();
        return true;
    }''',
)

# Add compact proof counters to the existing V3.11 ObjectPaging stats block.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            stats->setAttribute(frameNumber, "V3.11 Prepared Active Resident",
                static_cast<double>(mV311PreparedActiveChunks.size()));
        }
    }''',
    '''            stats->setAttribute(frameNumber, "V3.11 Prepared Active Resident",
                static_cast<double>(mV311PreparedActiveChunks.size()));
        }
        stats->setAttribute(frameNumber, "V3.13 Weak Cache Hit On Strong Prepare",
            static_cast<double>(mV313WeakCacheHitOnStrongPrepare.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Built",
            static_cast<double>(mV313UpgradeBuilt.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Installed",
            static_cast<double>(mV313UpgradeInstalled.load(std::memory_order_relaxed)));
        stats->setAttribute(frameNumber, "V3.13 Upgrade Coalesced",
            static_cast<double>(mV313UpgradeCoalesced.load(std::memory_order_relaxed)));
        {
            std::lock_guard<std::mutex> lock(mV313ChunkQualityMutex);
            stats->setAttribute(frameNumber, "V3.13 Quality Entries",
                static_cast<double>(mV313ChunkQualities.size()));
            stats->setAttribute(frameNumber, "V3.13 Upgrade In Flight",
                static_cast<double>(mV313StrongUpgradeInFlight.size()));
        }
    }''',
)

# -----------------------------------------------------------------------------
# Runtime matrix. 73 repeats the promoted Mode66 foundation without V3.12 extras;
# 74 isolates quality repair; 75 adds the repeatedly validated Lua bytecode cache;
# 76 is an aggressive strict-signature + spatial experiment. ETA predictor remains
# available in inherited modes but is intentionally OFF in all V3.13 candidates.
# -----------------------------------------------------------------------------
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old = "$V312SpatialBatchMode = '0'\n$RendererProfiling"
new = "$V312SpatialBatchMode = '0'\n$V313ChunkQualityMode = '0'\n$RendererProfiling"
if text.count(old) != 1:
    raise RuntimeError("V3.13 launcher defaults anchor mismatch")
text = text.replace(old, new, 1)

old_menu = "Write-Host ' 72 = V3.12 aggressive two-horizon + spatial batching'"
new_menu = """Write-Host ' 72 = V3.12 aggressive two-horizon + spatial batching'
Write-Host ' 73 = V3.13 exact Mode66 foundation control'
Write-Host ' 74 = V3.13 deterministic ObjectPaging quality repair'
Write-Host ' 75 = V3.13 quality repair + Lua precompile (RECOMMENDED)'
Write-Host ' 76 = V3.13 strict quality signature + Lua + spatial experiment'"""
if text.count(old_menu) != 1:
    raise RuntimeError("V3.13 launcher menu anchor mismatch")
text = text.replace(old_menu, new_menu, 1)

# Extend the prompt/range without copying the enormous inherited choice list.
text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 72' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 76' } until ($choice -in @(" + m.group(1) + ",'73','74','75','76'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.13 launcher choice-range anchor mismatch")

mode72 = re.search(r"(?m)^    '72' \{[^\n]+\}\n\}", text)
if not mode72:
    raise RuntimeError("V3.13 launcher Mode72 anchor not found")
foundation = "$V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'"
addition = f"""{mode72.group(0)[:-2]}
    '73' {{ $Experiment = 'v313-mode66-control'; {foundation} }}
    '74' {{ $Experiment = 'v313-quality-repair'; {foundation}; $V313ChunkQualityMode = '1' }}
    '75' {{ $Experiment = 'v313-quality-lua'; {foundation}; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1' }}
    '76' {{ $Experiment = 'v313-strict-spatial'; {foundation}; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1'; $V313ChunkQualityMode = '2' }}
}}"""
text = text[:mode72.start()] + addition + text[mode72.end():]

old = '    "v312_spatial_batch_mode=$V312SpatialBatchMode",\n    "shadow_distance=$ShadowDistance",'
new = '    "v312_spatial_batch_mode=$V312SpatialBatchMode",\n    "v313_chunk_quality_mode=$V313ChunkQualityMode",\n    "shadow_distance=$ShadowDistance",'
if text.count(old) != 1:
    raise RuntimeError("V3.13 launcher run-metadata anchor mismatch")
text = text.replace(old, new, 1)

old = "    Set-IniValue $SettingsPath 'V3' 'v3.12 spatial batch mode' $V312SpatialBatchMode\n    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"
new = "    Set-IniValue $SettingsPath 'V3' 'v3.12 spatial batch mode' $V312SpatialBatchMode\n    Set-IniValue $SettingsPath 'V3' 'v3.13 chunk quality mode' $V313ChunkQualityMode\n    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"
if text.count(old) != 1:
    raise RuntimeError("V3.13 launcher settings-write anchor mismatch")
text = text.replace(old, new, 1)
launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.13 launcher matrix 73-76 patched successfully")

print("V3.13 deterministic ObjectPaging quality layer completed successfully.")
