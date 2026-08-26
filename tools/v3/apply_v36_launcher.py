import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 launcher match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 launcher patched {rel} ({count} match(es))")


launcher = "tools/v3/launchers/V3_Lab.ps1"

replace_exact(
    launcher,
    '''$OcclusionMaxTriangles = '30000'
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }''',
    '''$OcclusionMaxTriangles = '30000'
$OcclusionCulling = 'true'
$RamCacheMode = 'overdrive'
$ShadowDistance = '4096'
$V36PerformanceProfile = 'false'
$V36DisableRamOverdrive = 'false'
$V36DisableLuaFastPath = 'false'
$V36DisableCoarseChunkOcclusion = 'false'
$V36AsyncGpuProfiler = 'false'
$V36FarCasterMinimumPixels = '0.0'
$V36Attribution = $false
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }''',
)

replace_exact(
    launcher,
    '''Write-Host ' 23 = V3.5 full combined: coarse MSOC + Lua fast + divisor 4 + dynamic far reuse'
do { $choice = Read-Host 'Enter 1 through 23' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23'))''',
    '''Write-Host ' 23 = V3.5 full combined: coarse MSOC + Lua fast + divisor 4 + dynamic far reuse'
Write-Host ''
Write-Host 'V3.6 profiles and isolated diagnostics:' -ForegroundColor Yellow
Write-Host ' 24 = TRUE custom baseline (all V3 runtime optimizations off; normal RAM cache)'
Write-Host ' 25 = V3.6 normal profile (RAM overdrive/balanced + Lua fast path + coarse MSOC)'
Write-Host ' 26 = V3.6 normal profile + asynchronous GPU pass profiler'
Write-Host ' 27 = V3.6 far-caster pruning isolated'
Write-Host ' 28 = V3.6 coarse MSOC isolated + v2 skipped-work telemetry'
Write-Host ' 29 = V3.6 hitch attribution only (Lua + controllers + residency + batching audit)'
Write-Host ' 30 = V3.6 steady-state combined (normal + GPU profiler + far-caster pruning)'
Write-Host ' 31 = V3.6 hitch combined (normal + deep hitch attribution)'
Write-Host ' 32 = V3.6 full diagnostic combined'
do { $choice = Read-Host 'Enter 1 through 32' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32'))''',
)

replace_exact(
    launcher,
    '''    '23' { $Experiment = 'v35-full-combined'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
}''',
    '''    '23' { $Experiment = 'v35-full-combined'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
    '24' { $Experiment = 'v36-true-custom-baseline'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false' }
    '25' { $Experiment = 'v36-normal-profile'; $V36PerformanceProfile = 'true' }
    '26' { $Experiment = 'v36-normal-gpu-profiler'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true' }
    '27' { $Experiment = 'v36-far-caster-pruning-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V36FarCasterMinimumPixels = '2.0' }
    '28' { $Experiment = 'v36-coarse-msoc-isolated'; $RamCacheMode = 'normal'; $V35CoarseChunkOcclusion = 'true' }
    '29' { $Experiment = 'v36-hitch-attribution-only'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V36Attribution = $true }
    '30' { $Experiment = 'v36-steady-combined'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0' }
    '31' { $Experiment = 'v36-hitch-combined'; $V36PerformanceProfile = 'true'; $V36Attribution = $true }
    '32' { $Experiment = 'v36-full-diagnostic'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0'; $V36Attribution = $true }
}

if ([int]$choice -ge 24) {
    Write-Host ''
    Write-Host 'Choose benchmark shadow distance:' -ForegroundColor Cyan
    Write-Host '  1 = 4096 (recommended comparison baseline)'
    Write-Host '  2 = 6144'
    Write-Host '  3 = 8192 (stress test)'
    do { $shadowDistanceChoice = Read-Host 'Enter 1, 2, or 3' } until ($shadowDistanceChoice -in @('1','2','3'))
    $ShadowDistance = @{ '1' = '4096'; '2' = '6144'; '3' = '8192' }[$shadowDistanceChoice]
}
if ($choice -in @('27','30','32')) {
    Write-Host ''
    Write-Host 'Choose far-cascade resolution divisor:' -ForegroundColor Cyan
    Write-Host '  1 = divisor 1 (2048 when base shadow resolution is 2048; recommended)'
    Write-Host '  2 = divisor 2 (1024)'
    Write-Host '  3 = divisor 4 (512; known flicker-risk comparison only)'
    do { $farResolutionChoice = Read-Host 'Enter 1, 2, or 3' } until ($farResolutionChoice -in @('1','2','3'))
    $FarShadowResolutionDivisor = @{ '1' = '1'; '2' = '2'; '3' = '4' }[$farResolutionChoice]
}''',
)

replace_exact(
    launcher,
    '''if (Test-Path -LiteralPath (Join-Path $GameDir 'CI-ID.txt')) { Copy-Item -LiteralPath (Join-Path $GameDir 'CI-ID.txt') -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force }

$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash''',
    '''$ciIdPath = Join-Path $GameDir 'CI-ID.txt'
if (Test-Path -LiteralPath $ciIdPath) { Copy-Item -LiteralPath $ciIdPath -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force }
$buildCommit = 'unknown'
if (Test-Path -LiteralPath $ciIdPath) {
    $commitLine = Get-Content -LiteralPath $ciIdPath | Where-Object { $_ -match '^Commit\\s+' } | Select-Object -First 1
    if ($commitLine) { $buildCommit = ($commitLine -replace '^Commit\\s+', '').Trim() }
}

$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash''',
)

replace_exact(
    launcher,
    '''    "openmw_exe_sha256=$exeHash",
    "game_dir=$GameDir",''',
    '''    "openmw_exe_sha256=$exeHash",
    "build_commit=$buildCommit",
    "game_dir=$GameDir",''',
)

replace_exact(
    launcher,
    '''    "v35_allow_dynamic_far_reuse=$V35AllowDynamicFarReuse",
    "benchmark_groundcover_density=1.0",''',
    '''    "v35_allow_dynamic_far_reuse=$V35AllowDynamicFarReuse",
    "v36_performance_profile=$V36PerformanceProfile",
    "v36_disable_ram_overdrive=$V36DisableRamOverdrive",
    "v36_disable_lua_fast_path=$V36DisableLuaFastPath",
    "v36_disable_coarse_chunk_occlusion=$V36DisableCoarseChunkOcclusion",
    "v36_async_gpu_profiler=$V36AsyncGpuProfiler",
    "v36_far_caster_minimum_pixels=$V36FarCasterMinimumPixels",
    "v36_deep_attribution=$V36Attribution",
    "shadow_distance=$ShadowDistance",
    "benchmark_groundcover_density=1.0",''',
)

replace_exact(
    launcher,
    '''    'OPENMW_V35_LUA_LOAD_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
    '''    'OPENMW_V35_LUA_LOAD_FILE','OPENMW_V36_GPU_PASS_FILE','OPENMW_V36_LUA_ADDSCRIPT_FILE',
    'OPENMW_V36_CONTROLLER_FILE','OPENMW_V36_RESIDENCY_FILE','OPENMW_V36_BATCHING_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
)

replace_exact(
    launcher,
    '''$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'

if ($Mode -eq 'City')''',
    '''$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'
if ($V36AsyncGpuProfiler -eq 'true') {
    $env:OPENMW_V36_GPU_PASS_FILE = Join-Path $ProfileDir 'v36-gpu-passes.csv'
}
if ($V36Attribution) {
    $env:OPENMW_V36_LUA_ADDSCRIPT_FILE = Join-Path $ProfileDir 'v36-lua-addscript.csv'
    $env:OPENMW_V36_CONTROLLER_FILE = Join-Path $ProfileDir 'v36-controller-build.csv'
    $env:OPENMW_V36_RESIDENCY_FILE = Join-Path $ProfileDir 'v36-source-residency.csv'
    $env:OPENMW_V36_BATCHING_FILE = Join-Path $ProfileDir 'v36-static-batching-audit.csv'
}

if ($Mode -eq 'City')''',
)

replace_exact(
    launcher,
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Shadows' 'v3.5 allow dynamic far cascade reuse' $V35AllowDynamicFarReuse''',
    '''    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Shadows' 'v3.5 allow dynamic far cascade reuse' $V35AllowDynamicFarReuse
    Set-IniValue $SettingsPath 'Shadows' 'maximum shadow map distance' $ShadowDistance
    Set-IniValue $SettingsPath 'V3' 'v3.6 performance profile' $V36PerformanceProfile
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable ram overdrive' $V36DisableRamOverdrive
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable lua fast path' $V36DisableLuaFastPath
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable coarse chunk occlusion' $V36DisableCoarseChunkOcclusion
    Set-IniValue $SettingsPath 'V3' 'v3.6 async gpu profiler' $V36AsyncGpuProfiler
    Set-IniValue $SettingsPath 'V3' 'v3.6 far caster minimum pixels' $V36FarCasterMinimumPixels''',
)

replace_exact(
    launcher,
    "    Set-IniValue $SettingsPath 'Camera' 'occlusion culling' 'true'",
    '''    Set-IniValue $SettingsPath 'Camera' 'occlusion culling' $OcclusionCulling''',
)

replace_exact(
    launcher,
    '''    if ($Mode -ne 'Render') {
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'
        Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'
    }''',
    "    Set-IniValue $SettingsPath 'Cells' 'ram cache mode' $RamCacheMode\n"
    "    Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'",
)

print("V3.6 unified launcher/profile matrix source patch completed successfully.")
