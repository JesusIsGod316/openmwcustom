import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.4 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.4 patched {rel} ({count} match(es))")


# V3.4 broadened MSOC coverage. Disabled by default.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<int> mOcclusionMaxTriangles{ mIndex, "Camera", "occlusion max triangles",
            makeClampSanitizerInt(0, 500000) };''',
    '''        SettingValue<int> mOcclusionMaxTriangles{ mIndex, "Camera", "occlusion max triangles",
            makeClampSanitizerInt(0, 500000) };
        SettingValue<bool> mV34BroadenOcclusion{ mIndex, "Camera", "v3.4 broaden occlusion" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''occlusion max triangles = 30000

[Cells]''',
    '''occlusion max triangles = 30000

# V3.4: allow large objects that were not admitted as occluders to be rejected by the completed full MSOC buffer.
# The launcher can pair this with a lower occluder radius / longer occluder range. Disabled by default.
v3.4 broaden occlusion = false

[Cells]''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            unsigned int maxTriangles, OcclusionStorage* storage = nullptr);''',
    '''            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage = nullptr);''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        bool mEnableStaticOccluders;
        unsigned int mMaxTriangles;''',
    '''        bool mEnableStaticOccluders;
        bool mV34BroadenOcclusion;
        unsigned int mMaxTriangles;''',
)

replace_exact(
    "apps/openmw/mwrender/objects.hpp",
    '''            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            unsigned int maxTriangles, OcclusionStorage* storage = nullptr);''',
    '''            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage = nullptr);''',
)
replace_exact(
    "apps/openmw/mwrender/objects.hpp",
    '''        bool mEnableStaticOccluders = true;
        unsigned int mMaxTriangles = 30000;''',
    '''        bool mEnableStaticOccluders = true;
        bool mV34BroadenOcclusion = false;
        unsigned int mMaxTriangles = 30000;''',
)

# There are two cell-root construction sites. Also wire the persistent storage pointer that Objects already retained.
replace_exact(
    "apps/openmw/mwrender/objects.cpp",
    '''                    mOccluderInsideThreshold, mOccluderMaxDistance, mEnableStaticOccluders, mMaxTriangles));''',
    '''                    mOccluderInsideThreshold, mOccluderMaxDistance, mEnableStaticOccluders,
                    mV34BroadenOcclusion, mMaxTriangles, mOcclusionStorage));''',
    expected=2,
)
replace_exact(
    "apps/openmw/mwrender/objects.cpp",
    '''        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        unsigned int maxTriangles, OcclusionStorage* storage)''',
    '''        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage)''',
)
replace_exact(
    "apps/openmw/mwrender/objects.cpp",
    '''        mEnableStaticOccluders = enableStaticOccluders;
        mMaxTriangles = maxTriangles;''',
    '''        mEnableStaticOccluders = enableStaticOccluders;
        mV34BroadenOcclusion = v34BroadenOcclusion;
        mMaxTriangles = maxTriangles;''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                occluderMeshRes, occluderMaxMeshRes, occluderInsideThreshold, occluderMaxDistance, enableStatics,
                maxTriangles, mOcclusionStorage.get());''',
    '''                occluderMeshRes, occluderMaxMeshRes, occluderInsideThreshold, occluderMaxDistance, enableStatics,
                Settings::camera().mV34BroadenOcclusion, maxTriangles, mOcclusionStorage.get());''',
)

replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        unsigned int maxTriangles, OcclusionStorage* storage)''',
    '''        float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
        bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage)''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        , mEnableStaticOccluders(enableStaticOccluders)
        , mMaxTriangles(maxTriangles)''',
    '''        , mEnableStaticOccluders(enableStaticOccluders)
        , mV34BroadenOcclusion(v34BroadenOcclusion)
        , mMaxTriangles(maxTriangles)''',
)

# Track whether this large object inserted its own proxy. Self-testing an inserted proxy is intentionally forbidden.
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            if (mesh.aabb.valid() && mEnableStaticOccluders && !mesh.indices.empty()
                && mCuller->testVisibleAABBTerrainOnly(mesh.aabb))''',
    '''            bool v34RasterizedAsOccluder = false;
            if (mesh.aabb.valid() && mEnableStaticOccluders && !mesh.indices.empty()
                && mCuller->testVisibleAABBTerrainOnly(mesh.aabb))''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''                            mCuller->incrementBuildingOccluders(
                                newTris, static_cast<unsigned int>(mesh.vertices.size()));''',
    '''                            mCuller->incrementBuildingOccluders(
                                newTris, static_cast<unsigned int>(mesh.vertices.size()));
                            v34RasterizedAsOccluder = true;''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            // Always traverse large buildings. Do NOT gate traversal on testVisibleAABB —
            // buildings testing against a buffer that includes previously rasterized
            // buildings causes false culling (flickering) when child ordering happens to
            // place one building in front of another in the depth buffer. Large buildings
            // are correctly culled by PVS and the cell-level AABB test above; MSOC
            // is reserved for culling small objects in Pass 2.
            child->accept(*cv);''',
    '''            // Keep the V3.3 anti-self-occlusion rule for admitted building proxies. V3.4 only expands full-buffer
            // testing to large objects that did NOT insert their own proxy this frame (out of range, over budget,
            // unsuitable mesh, or camera-inside exclusion), so no object is tested against itself.
            if (mV34BroadenOcclusion && !v34RasterizedAsOccluder)
            {
                osg::BoundingBox largeBB;
                largeBB.expandBy(bs);
                if (!mCuller->testVisibleAABB(largeBB))
                    continue;
            }
            child->accept(*cv);''',
)

# V3.4 aggressive far-cascade experiment: keep near/middle untouched, permit divisor 4 for the far map.
replace_exact(
    "components/settings/categories/shadows.hpp",
    '''            "v3.3 far cascade resolution divisor", makeClampSanitizerInt(1, 2) };''',
    '''            "v3.3 far cascade resolution divisor", makeClampSanitizerInt(1, 4) };''',
)
replace_exact(
    "components/sceneutil/mwshadowtechnique.cpp",
    '''    _v33FarCascadeResolutionDivisor = std::clamp(divisor, 1u, 2u);''',
    '''    _v33FarCascadeResolutionDivisor = std::clamp(divisor, 1u, 4u);''',
)
replace_exact(
    "files/settings-default.cfg",
    '''# V3.3 far-cascade-only GPU experiment. 1 preserves full resolution. 2 keeps near/middle cascades at full
# resolution while rendering the far cascade at half width and height. All configured caster types still render.''',
    '''# V3.3/V3.4 far-cascade-only GPU experiment. 1 preserves full resolution, 2 uses half width/height, and 4 uses
# quarter width/height (1/16 the far-cascade pixels). Near/middle cascades and all configured caster types remain.''',
)

# Unified one-run telemetry and V3.4 menu. City becomes the normal superset benchmark.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$LuaIdleTimerFastPath = 'false'
$FarShadowResolutionDivisor = '1'
$RendererProfiling''',
    '''$LuaIdleTimerFastPath = 'false'
$FarShadowResolutionDivisor = '1'
$V34BroadenOcclusion = 'false'
$OccluderMinRadius = '400'
$OccluderMaxDistance = '6144'
$OcclusionMaxTriangles = '30000'
$RendererProfiling''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 14 = V3.3 idle-timer + far-cascade GPU optimizations'
do { $choice = Read-Host 'Enter 1 through 14' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14'))''',
    '''Write-Host ' 14 = V3.3 idle-timer + far-cascade GPU optimizations'
Write-Host ' 15 = V3.4 broadened MSOC (more/farther occluders + safe large-object rejection)'
Write-Host ' 16 = V3.4 aggressive far cascade (resolution divisor 4)'
Write-Host ' 17 = V3.4 broadened MSOC + proven Lua idle-timer fast path'
Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
do { $choice = Read-Host 'Enter 1 through 18' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18'))''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '14' { $Experiment = 'v33-tail-gpu-combined'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '2' }
}''',
    '''    '14' { $Experiment = 'v33-tail-gpu-combined'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '2' }
    '15' { $Experiment = 'v34-broadened-msoc'; $V34BroadenOcclusion = 'true'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '16' { $Experiment = 'v34-aggressive-far-shadow'; $FarShadowResolutionDivisor = '4' }
    '17' { $Experiment = 'v34-msoc-lua'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
}''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v33_idle_timer_fast_path=$LuaIdleTimerFastPath",
    "v33_far_shadow_resolution_divisor=$FarShadowResolutionDivisor"''',
    '''    "v33_idle_timer_fast_path=$LuaIdleTimerFastPath",
    "v33_far_shadow_resolution_divisor=$FarShadowResolutionDivisor",
    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "occlusion_occluder_min_radius=$OccluderMinRadius",
    "occlusion_occluder_max_distance=$OccluderMaxDistance",
    "occlusion_max_triangles=$OcclusionMaxTriangles"''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    """if ($Mode -eq 'City') {
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'""",
    """if ($Mode -eq 'City') {
    # Unified V3.4 benchmark: traversal/Lua plus render/GPU/MSOC/shadow in the same game run.
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_POSTFX_FILE = Join-Path $ProfileDir 'v3-postfx.csv'
    $env:OPENMW_V3_MSOC_DETAIL_FILE = Join-Path $ProfileDir 'v3-msoc-detail.csv'
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'""",
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
    '''    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling' 'true'
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling terrain' 'true'
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling statics' 'true'
    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder max distance' $OccluderMaxDistance
    Set-IniValue $SettingsPath 'Camera' 'occlusion max triangles' $OcclusionMaxTriangles
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    """        Write-Host 'Suggested route: use the same outdoor city save, wait 15-20 sec, then walk the same 2-3 minute route across several cell boundaries. Avoid doors.'""",
    """        Write-Host 'Unified benchmark: use the same outdoor city save. Hold the usual heavy outdoor view for ~45 sec, then walk the same 2-3 minute route across several cell boundaries. Avoid doors. One run captures render/GPU/MSOC/shadow + Lua/traversal telemetry.'""",
)

print("V3.4 broadened MSOC, aggressive far-shadow, persistent cell-occluder storage, and unified telemetry patch completed successfully.")
