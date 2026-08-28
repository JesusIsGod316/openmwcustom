import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.12 spatial match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.12 spatial patched {rel} ({count} match(es))")


# -----------------------------------------------------------------------------
# Optional spatial clustering of the already-proven V3.11 prepared-active merge.
#
# Mode66 uses one mergeGroup per ObjectPaging chunk. That is excellent for draw
# reduction but can create broad child bounds. Spatial mode1 keeps every existing
# compatibility/merge decision and every vertex/material/refnum unchanged, while
# routing mergeable prepared-active instances into four chunk-local quadrants.
# Each quadrant then receives the exact same optimizer/shareState/compile policy
# independently. The goal is to preserve most submission reduction while restoring
# finer frustum/cull granularity. It is deliberately restricted to compile=true
# V3.11 Mode2 prepared active-grid chunks; demand fallback and historical modes are
# byte-for-byte equivalent when this setting is off.
# -----------------------------------------------------------------------------
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<bool> mV312LuaPrecompile{ mIndex, "V3", "v3.12 lua precompile" };''',
    '''        SettingValue<bool> mV312LuaPrecompile{ mIndex, "V3", "v3.12 lua precompile" };
        SettingValue<int> mV312SpatialBatchMode{ mIndex, "V3", "v3.12 spatial batch mode",
            makeClampSanitizerInt(0, 1) };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.12 lua precompile = false

[Cells]''',
    '''v3.12 lua precompile = false
# Spatial prepared-active batching: 0=exact Mode66 single merge group,
# 1=split mergeable prepared active-grid geometry into 2x2 chunk-local clusters.
v3.12 spatial batch mode = 0

[Cells]''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        osg::ref_ptr<osg::Group> group = new osg::Group;
        osg::ref_ptr<osg::Group> mergeGroup = new osg::Group;
        osg::ref_ptr<Resource::TemplateMultiRef> templateRefs = new Resource::TemplateMultiRef;''',
    '''        osg::ref_ptr<osg::Group> group = new osg::Group;
        const int v312SpatialBatchMode = static_cast<int>(Settings::cells().mV312SpatialBatchMode);
        const bool v312SpatialPrepared = v312SpatialBatchMode > 0 && activeGrid && compile
            && static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode) >= 2;
        std::vector<osg::ref_ptr<osg::Group>> v312MergeGroups;
        v312MergeGroups.emplace_back(new osg::Group);
        if (v312SpatialPrepared)
        {
            v312MergeGroups.emplace_back(new osg::Group);
            v312MergeGroups.emplace_back(new osg::Group);
            v312MergeGroups.emplace_back(new osg::Group);
        }
        osg::Group* mergeGroup = v312MergeGroups.front().get();
        osg::ref_ptr<Resource::TemplateMultiRef> templateRefs = new Resource::TemplateMultiRef;''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);''',
    '''                osg::Group* attachTo = group;
                if (merge)
                {
                    if (v312SpatialPrepared)
                    {
                        const unsigned int cluster = (nodePos.x() >= 0.f ? 1u : 0u)
                            | (nodePos.y() >= 0.f ? 2u : 0u);
                        attachTo = v312MergeGroups[cluster].get();
                    }
                    else
                        attachTo = mergeGroup;
                }
                attachTo->addChild(trans);''',
)

# V3.6 structure tracing inspects the pre-optimized merged population. Sum every
# spatial group when enabled so diagnostics retain the same meaning.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)
        {
            v36BeforeStats += Debug::V36StructureTrace::inspect(*group);
            v36BeforeStats += Debug::V36StructureTrace::inspect(*mergeGroup);
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;

        if (mergeGroup->getNumChildren())
        {''',
    '''        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)
        {
            v36BeforeStats += Debug::V36StructureTrace::inspect(*group);
            for (const osg::ref_ptr<osg::Group>& v312MergeGroupRef : v312MergeGroups)
                v36BeforeStats += Debug::V36StructureTrace::inspect(*v312MergeGroupRef);
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;

        for (const osg::ref_ptr<osg::Group>& v312MergeGroupRef : v312MergeGroups)
        {
            osg::Group* const mergeGroup = v312MergeGroupRef.get();
            if (!mergeGroup->getNumChildren())
                continue;
        {''',
)

# Close the additional per-cluster loop after the inherited optimizer block.
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            if (compile)
            {
                stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                mergeGroup->accept(stateToCompile);
            }
        }

        osgUtil::IncrementalCompileOperation* const ico''',
    '''            if (compile)
            {
                stateToCompile._mode = osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS;
                mergeGroup->accept(stateToCompile);
            }
        }
        }

        osgUtil::IncrementalCompileOperation* const ico''',
)

# -----------------------------------------------------------------------------
# Complete the V3.12 runtime matrix.
# 71 adds only spatial clustering to the combined safe candidate.
# 72 enables the existing predictor's second horizon, a longer lead and spatial
# clustering. It is intentionally aggressive and may spend more background work;
# it still preserves the emergency Mode1 demand fallback and performs no GL
# resource eviction.
# -----------------------------------------------------------------------------
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V312LuaPrecompile = 'false'
$RendererProfiling''',
    '''$V312LuaPrecompile = 'false'
$V312SpatialBatchMode = '0'
$RendererProfiling''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 70 = V3.12 combined ETA predictor + Lua precompile (FIRST SAFE CANDIDATE)'
do { $choice = Read-Host 'Enter 1 through 70' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66','67','68','69','70'))''',
    '''Write-Host ' 70 = V3.12 combined ETA predictor + Lua precompile (FIRST SAFE CANDIDATE)'
Write-Host ' 71 = V3.12 combined safe + spatial prepared-active batching'
Write-Host ' 72 = V3.12 aggressive two-horizon + spatial batching'
do { $choice = Read-Host 'Enter 1 through 72' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66','67','68','69','70','71','72'))''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '70' { $Experiment = 'v312-combined-safe'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true' }
}''',
    '''    '70' { $Experiment = 'v312-combined-safe'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true' }
    '71' { $Experiment = 'v312-spatial-batching'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1' }
    '72' { $Experiment = 'v312-aggressive-horizon'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '2'; $V312PredictorLeadSeconds = '4.0'; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1' }
}''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v312_lua_precompile=$V312LuaPrecompile",
    "shadow_distance=$ShadowDistance",''',
    '''    "v312_lua_precompile=$V312LuaPrecompile",
    "v312_spatial_batch_mode=$V312SpatialBatchMode",
    "shadow_distance=$ShadowDistance",''',
)

replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'V3' 'v3.12 lua precompile' $V312LuaPrecompile
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
    '''    Set-IniValue $SettingsPath 'V3' 'v3.12 lua precompile' $V312LuaPrecompile
    Set-IniValue $SettingsPath 'V3' 'v3.12 spatial batch mode' $V312SpatialBatchMode
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath''',
)

print("V3.12 spatial prepared-active batching and complete runtime matrix patched successfully.")
