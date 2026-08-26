from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.2 foundation match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.2 foundation patched {rel}")


# Settings. Every V3.2 behavior remains explicitly opt-in.
replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<std::string> mV3StreamingScheduler{ mIndex, "Cells", "v3 streaming scheduler",
            makeEnumSanitizerString({ "off", "adaptive" }) };
        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mV3PreparedInstanceCache{ mIndex, "Cells", "v3 prepared instance cache" };
        SettingValue<int> mV3PreparedInstanceCacheMax{ mIndex, "Cells", "v3 prepared instance cache max",
            makeClampSanitizerInt(256, 65536) };''',
    '''        SettingValue<std::string> mV3StreamingScheduler{ mIndex, "Cells", "v3 streaming scheduler",
            makeEnumSanitizerString({ "off", "adaptive", "adaptive-v2" }) };
        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mV32StreamingMaxDefers{ mIndex, "Cells", "v3.2 streaming max defers",
            makeClampSanitizerInt(0, 16) };
        SettingValue<bool> mV3PreparedInstanceCache{ mIndex, "Cells", "v3 prepared instance cache" };
        SettingValue<int> mV3PreparedInstanceCacheMax{ mIndex, "Cells", "v3 prepared instance cache max",
            makeClampSanitizerInt(256, 65536) };
        SettingValue<bool> mV32ExteriorHibernation{ mIndex, "Cells", "v3.2 exterior hibernation" };
        SettingValue<bool> mV32RendererInsertionProfiling{ mIndex, "Cells", "v3.2 renderer insertion profiling" };
        SettingValue<bool> mV32GpuMemoryTelemetry{ mIndex, "Cells", "v3.2 gpu memory telemetry" };
        SettingValue<bool> mV32GpuMemoryManagement{ mIndex, "Cells", "v3.2 gpu memory management" };
        SettingValue<int> mV32GpuSoftBudgetMb{ mIndex, "Cells", "v3.2 gpu soft budget mb",
            makeClampSanitizerInt(512, 65536) };
        SettingValue<int> mV32GpuHardBudgetMb{ mIndex, "Cells", "v3.2 gpu hard budget mb",
            makeClampSanitizerInt(512, 65536) };''',
)

replace_once(
    "files/settings-default.cfg",
    '''# V3 experimental prepared static-instance pool. The preload worker clones safe static scene instances ahead of time,
# and cell activation consumes those already-prepared clones instead of cloning the same templates on the main thread.
# OFF preserves normal OpenMW behavior. Start around 8192 on a 32 GB system; the pool is strictly bounded.
v3 prepared instance cache = false
v3 prepared instance cache max = 8192

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
    '''# V3 experimental prepared static-instance pool. The preload worker clones safe static scene instances ahead of time,
# and cell activation consumes those already-prepared clones instead of cloning the same templates on the main thread.
# OFF preserves normal OpenMW behavior. Start around 8192 on a 32 GB system; the pool is strictly bounded.
v3 prepared instance cache = false
v3 prepared instance cache max = 8192

# V3.2 transition/memory experiments. All are OFF by default.
v3.2 exterior hibernation = false
v3.2 renderer insertion profiling = false
v3.2 gpu memory telemetry = false
v3.2 gpu memory management = false
v3.2 gpu soft budget mb = 6800
v3.2 gpu hard budget mb = 7400
v3.2 streaming max defers = 2

# Preload cells in a background thread. All settings starting with 'preload' have no effect unless this is enabled.''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''                     << " prepared instances="
                     << (static_cast<bool>(Settings::cells().mV3PreparedInstanceCache) ? "on" : "off")
                     << "/" << static_cast<int>(Settings::cells().mV3PreparedInstanceCacheMax);''',
    '''                     << " prepared instances="
                     << (static_cast<bool>(Settings::cells().mV3PreparedInstanceCache) ? "on" : "off")
                     << "/" << static_cast<int>(Settings::cells().mV3PreparedInstanceCacheMax)
                     << " v3.2 hibernation="
                     << (static_cast<bool>(Settings::cells().mV32ExteriorHibernation) ? "on" : "off")
                     << " gpu telemetry="
                     << (static_cast<bool>(Settings::cells().mV32GpuMemoryTelemetry) ? "on" : "off")
                     << " gpu management="
                     << (static_cast<bool>(Settings::cells().mV32GpuMemoryManagement) ? "on" : "off")
                     << " gpu budget=" << static_cast<int>(Settings::cells().mV32GpuSoftBudgetMb) << "/"
                     << static_cast<int>(Settings::cells().mV32GpuHardBudgetMb) << " MiB";''',
)

replace_once(
    "components/debug/v3diagnostics.hpp",
    '''    inline CsvWriter& streamingWriter()
    {
        static CsvWriter writer("OPENMW_V3_STREAMING_FILE",
            "frame,epoch_ms,event,category,detail,last_frame_ms,limit,count");
        return writer;
    }

    inline CsvWriter& traceWriter()''',
    '''    inline CsvWriter& streamingWriter()
    {
        static CsvWriter writer("OPENMW_V3_STREAMING_FILE",
            "frame,epoch_ms,event,category,detail,last_frame_ms,limit,count");
        return writer;
    }

    inline CsvWriter& gpuMemoryWriter()
    {
        static CsvWriter writer("OPENMW_V32_GPU_MEMORY_FILE",
            "frame,epoch_ms,dedicated_usage_mb,dedicated_budget_mb,available_for_reservation_mb,"
            "current_reservation_mb,budget_used_pct,effective_soft_mb,effective_hard_mb,pressure");
        return writer;
    }

    inline CsvWriter& traceWriter()''',
)

replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v3gpumemory.hpp>''',
)

replace_once(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''    void RenderingManager::update(float dt, bool paused)
    {
        reportStats();''',
    '''    void RenderingManager::update(float dt, bool paused)
    {
        if (Settings::cells().mV32GpuMemoryTelemetry || Settings::cells().mV32GpuMemoryManagement)
        {
            Debug::V3GpuMemory::sampleIfDue(static_cast<bool>(Settings::cells().mV32GpuMemoryTelemetry),
                static_cast<bool>(Settings::cells().mV32GpuMemoryManagement),
                static_cast<int>(Settings::cells().mV32GpuSoftBudgetMb),
                static_cast<int>(Settings::cells().mV32GpuHardBudgetMb));
        }
        reportStats();''',
)

replace_once(
    "components/settings/ramcache.hpp",
    '''    inline bool adaptiveStreamingEnabled()
    {
        const std::string value = cells().mV3StreamingScheduler;
        return value == "adaptive";
    }

    inline float streamingTargetFrameMs()''',
    '''    inline bool adaptiveStreamingEnabled()
    {
        const std::string value = cells().mV3StreamingScheduler;
        return value == "adaptive";
    }

    inline bool adaptiveStreamingV2Enabled()
    {
        const std::string value = cells().mV3StreamingScheduler;
        return value == "adaptive-v2";
    }

    inline int streamingMaxDefers()
    {
        return static_cast<int>(cells().mV32StreamingMaxDefers);
    }

    inline float streamingTargetFrameMs()''',
)

# Launcher edits intentionally run after the existing V3 launcher-safety patch.
# That patch first establishes the restoration guard; V3.2 then layers GPU
# sampling on top without weakening the original safety invariant.
replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$Experiment = 'current-settings'
$Prepared = $null
$Scheduler = $null
if ($Mode -ne 'Render') {
    Write-Host ''
    Write-Host 'Choose the runtime experiment for this test:' -ForegroundColor Cyan
    Write-Host '  1 = Baseline Overdrive (new diagnostics, experiments off)'
    Write-Host '  2 = Prepared static instances'
    Write-Host '  3 = Adaptive predictive preload'
    Write-Host '  4 = Prepared instances + adaptive preload'
    do { $choice = Read-Host 'Enter 1, 2, 3, or 4' } until ($choice -in @('1','2','3','4'))
    switch ($choice) {
        '1' { $Experiment = 'baseline'; $Prepared = 'false'; $Scheduler = 'off' }
        '2' { $Experiment = 'prepared'; $Prepared = 'true'; $Scheduler = 'off' }
        '3' { $Experiment = 'adaptive'; $Prepared = 'false'; $Scheduler = 'adaptive' }
        '4' { $Experiment = 'combined'; $Prepared = 'true'; $Scheduler = 'adaptive' }
    }
}''',
    '''$Experiment = 'render-baseline'
$Hibernation = 'false'
$Prepared = 'false'
$Scheduler = 'off'
$RendererProfiling = if ($Mode -eq 'Transition') { 'true' } else { 'false' }
if ($Mode -ne 'Render') {
    Write-Host ''
    Write-Host 'Choose the runtime experiment for this test:' -ForegroundColor Cyan
    Write-Host '  1 = Baseline Overdrive (all runtime experiments off)'
    Write-Host '  2 = V3.2 recent-exterior hibernation'
    Write-Host '  3 = V3.2 Adaptive Scheduler v2'
    Write-Host '  4 = Hibernation + Adaptive v2'
    Write-Host '  5 = Legacy Prepared Static Instances v1'
    Write-Host '  6 = Legacy Adaptive Scheduler v1'
    Write-Host '  7 = Legacy Prepared v1 + Adaptive v1'
    Write-Host '  8 = Hibernation + Adaptive v2 + Prepared v1'
    do { $choice = Read-Host 'Enter 1 through 8' } until ($choice -in @('1','2','3','4','5','6','7','8'))
    switch ($choice) {
        '1' { $Experiment = 'baseline'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'off' }
        '2' { $Experiment = 'hibernation'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'off' }
        '3' { $Experiment = 'adaptive-v2'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
        '4' { $Experiment = 'hibernation-adaptive-v2'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
        '5' { $Experiment = 'prepared-v1'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'off' }
        '6' { $Experiment = 'adaptive-v1'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive' }
        '7' { $Experiment = 'legacy-combined'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'adaptive' }
        '8' { $Experiment = 'all-experimental'; $Hibernation = 'true'; $Prepared = 'true'; $Scheduler = 'adaptive-v2' }
    }
}''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
    '''    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V32_GPU_MEMORY_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'
)''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'
$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'

if ($Mode -eq 'City') {''',
    '''$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'
$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'
$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'

if ($Mode -eq 'City') {''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    '''try {
    if ($Mode -ne 'Render') {
        $changedSettings = $true
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'
        Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'
        Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler
        Set-IniValue $SettingsPath 'Cells' 'v3 streaming target frametime' '25'
        Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache' $Prepared
        Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache max' '8192'
        $changedSettings = $true
    }''',
    '''try {
    $changedSettings = $true
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory telemetry' 'true'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' 'false'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu soft budget mb' '6800'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu hard budget mb' '7400'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 exterior hibernation' $Hibernation
    Set-IniValue $SettingsPath 'Cells' 'v3.2 renderer insertion profiling' $RendererProfiling
    Set-IniValue $SettingsPath 'Cells' 'v3.2 streaming max defers' '2'
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming target frametime' '25'
    Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache' $Prepared
    Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache max' '8192'
    if ($Mode -ne 'Render') {
        Set-IniValue $SettingsPath 'Cells' 'ram cache mode' 'overdrive'
        Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'
    }''',
)

print("V3.2 foundation source patch completed successfully.")
