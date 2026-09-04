param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('City','Transition','Render')]
    [string]$Mode
)

$ErrorActionPreference = 'Stop'
$GameDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Exe = Join-Path $GameDir 'openmw.exe'
$UserOpenMW = Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'
$SettingsPath = Join-Path $UserOpenMW 'settings.cfg'
$OpenmwCfgPath = Join-Path $UserOpenMW 'openmw.cfg'
$ProfilesRoot = Join-Path $UserOpenMW 'V3Profiles'

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host 'ERROR: This launcher must be beside openmw.exe.' -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}
if (-not (Test-Path -LiteralPath $SettingsPath)) {
    Write-Host "ERROR: settings.cfg was not found at $SettingsPath" -ForegroundColor Red
    Read-Host 'Press Enter to close'
    exit 1
}

function Set-IniValue {
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $lines = [System.Collections.Generic.List[string]]::new()
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) { [void]$lines.Add($line) }

    $sectionPattern = '^\s*\[' + [regex]::Escape($Section) + '\]\s*$'
    $keyPattern = '^\s*' + [regex]::Escape($Key) + '\s*='
    $sectionIndex = -1
    for ($i = 0; $i -lt $lines.Count; ++$i) {
        if ($lines[$i] -match $sectionPattern) { $sectionIndex = $i; break }
    }

    if ($sectionIndex -lt 0) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') { [void]$lines.Add('') }
        [void]$lines.Add("[$Section]")
        [void]$lines.Add("$Key = $Value")
    }
    else {
        $end = $lines.Count
        for ($i = $sectionIndex + 1; $i -lt $lines.Count; ++$i) {
            if ($lines[$i] -match '^\s*\[.+\]\s*$') { $end = $i; break }
        }
        $found = -1
        for ($i = $sectionIndex + 1; $i -lt $end; ++$i) {
            if ($lines[$i] -match $keyPattern) { $found = $i; break }
        }
        if ($found -ge 0) { $lines[$found] = "$Key = $Value" }
        else { $lines.Insert($end, "$Key = $Value") }
    }

    [System.IO.File]::WriteAllLines($Path, $lines, [System.Text.UTF8Encoding]::new($false))
}

function Write-HardwareSnapshot {
    param([string]$Path)
    $out = [System.Collections.Generic.List[string]]::new()
    [void]$out.Add("Captured: $([DateTimeOffset]::Now.ToString('o'))")
    [void]$out.Add('')
    [void]$out.Add('=== CPU ===')
    try {
        foreach ($cpu in Get-CimInstance Win32_Processor) {
            [void]$out.Add("$($cpu.Name) | cores=$($cpu.NumberOfCores) logical=$($cpu.NumberOfLogicalProcessors) maxMHz=$($cpu.MaxClockSpeed)")
        }
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== MEMORY ===')
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        [void]$out.Add("TotalVisibleMemoryMB=$([math]::Round($os.TotalVisibleMemorySize / 1024, 1))")
        [void]$out.Add("FreePhysicalMemoryMB=$([math]::Round($os.FreePhysicalMemory / 1024, 1))")
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== GPU ===')
    try {
        foreach ($gpu in Get-CimInstance Win32_VideoController) {
            [void]$out.Add("$($gpu.Name) | Driver=$($gpu.DriverVersion) | AdapterRAM=$($gpu.AdapterRAM)")
        }
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== OS ===')
    try {
        $os = Get-CimInstance Win32_OperatingSystem
        [void]$out.Add("$($os.Caption) $($os.Version) build $($os.BuildNumber)")
    } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== ACTIVE POWER PLAN ===')
    try { [void]$out.Add((powercfg /getactivescheme | Out-String).Trim()) } catch { [void]$out.Add($_.Exception.Message) }
    [void]$out.Add('')
    [void]$out.Add('=== NVIDIA-SMI SNAPSHOT ===')
    try {
        if (Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue) {
            [void]$out.Add((& nvidia-smi.exe | Out-String).Trim())
        } else { [void]$out.Add('nvidia-smi not found') }
    } catch { [void]$out.Add($_.Exception.Message) }
    [System.IO.File]::WriteAllLines($Path, $out, [System.Text.UTF8Encoding]::new($false))
}

$Experiment = 'render-baseline'
$Hibernation = 'false'
$Prepared = 'false'
$Scheduler = 'off'
$PreloadBudget = '0'
$FarShadowInterval = '1'
$FarShadowMaxTexelDrift = '0.75'
$LuaIdleTimerFastPath = 'false'
$FarShadowResolutionDivisor = '1'
$V34BroadenOcclusion = 'false'
$V35CoarseChunkOcclusion = 'false'
$V35AllowDynamicFarReuse = 'false'
$OccluderMinRadius = '400'
$OccluderMaxDistance = '6144'
$OcclusionMaxTriangles = '30000'
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
$V37ActiveEventFastPath = 'false'
$V37CompanionKeyframePreload = 'false'
$V37RelaxedResourceSweep = 'false'
$V37ResourceSweepSeconds = '5.0'
$V37GpuMemoryManagement = 'false'
$V37StabilizeFarCascade = 'false'
$V38WorldBatchingMode = '0'
$V38WorldBatchingMergeMultiplier = '1.5'
$V38WorldBatchingMinInstances = '2'
$V38GpuResidencyMode = '0'
$V38FarShadowMode = '0'
$V38CompilePacingMode = '0'
$V39FrontloadMode = '0'
$V39BatchOptimizerMode = '0'
$V39ProactiveResidencyMode = '0'
$V310FreshInitialObjectPaging = 'false'
$V310PreloadPostTransform = 'false'
$V311ActiveGridPrepareMode = '0'
$V312PredictorMode = '0'
$V312PredictorLeadSeconds = '3.0'
$V312LuaPrecompile = 'false'
$V312SpatialBatchMode = '0'
$V313ChunkQualityMode = '0'
$V314LuaDependencyPrecompileMode = '0'
$V314LuaPackagePrototypeReuse = 'false'
$V314GroundcoverCompileMode = '0'
$V314PostfxCompileWarmup = 'false'
$V315PremergeStateCanonicalization = 'false'
$V315PacketizedPremergeMode = '0'
$V315AdaptiveCompileGovernor = '0'
$V316HeadCacheSize = '0'
$V316BufferCacheMin = ''
$V316BufferCacheMax = ''
$V316SfxPredecodeCacheSize = '0'
$V316SfxPredecodeWorkers = '0'
$V316SfxMetadataFrontload = 'false'
$V316IdleResourceSweep = 'false'
$V317LuaRuntime = 'stock'
$V317EngineLuaOptimizations = 'false'
$V318RenderScale = '1.0'
$V318Upscaler = 'bilinear'
$V318UpscalerSharpness = '0.20'
$V318RenderScaleManaged = 'false'
$V319FocusCadence = '1'
$V319OsgThreading = ''
$V320FocusAdaptive = '0'
$V321CompletionGovernor = '0'
$V321CP2Fairness = '0'
$V321CP3FullBodyFirstPerson = '0'
$V321CP4ShadowCompat = '0'
$V322CP1MsocHotPath = '0'
$V322CP2OccluderMode = '0'
$V322ParallelActorAvoidance = '0'
$V323ParallelMsocMode = '0'
$V324FrameJobQos = '0'
$V324AsyncMsoc = '0'
$V325ActorSourceBatch = '0'
$V325ParallelActorBinding = '0'
$V320EngineLuaFastPaths = '0'
$V320SoundConversionCache = '0'
$V320SoundQueryCoalescing = '0'
$V320LuaProfilerRecorderCapable = '0'
$RendererProfiling = if ($Mode -in @('City','Transition')) { 'true' } else { 'false' }
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
Write-Host '  9 = V3.3 predictive preload budget (diagnostic; budget 2 rarely binds)'
Write-Host ' 10 = V3.3 far-shadow reuse (requires actor and player shadows off)'
Write-Host ' 11 = V3.3 combined legacy experiments (same limitations as 9 and 10)'
Write-Host ' 12 = V3.3 idle-timer fast path (Lua/traversal optimization only)'
Write-Host ' 13 = V3.3 half-resolution far cascade (GPU optimization only)'
Write-Host ' 14 = V3.3 idle-timer + far-cascade GPU optimizations'
Write-Host ' 15 = V3.4 broadened MSOC (more/farther occluders + safe large-object rejection)'
Write-Host ' 16 = V3.4 aggressive far cascade (resolution divisor 4)'
Write-Host ' 17 = V3.4 broadened MSOC + proven Lua idle-timer fast path'
Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
Write-Host ' 19 = V3.5 coarse chunk MSOC (paged objects + groundcover)'
Write-Host ' 20 = V3.5 bounded one-frame far reuse + divisor 4 (actor/player shadows stay enabled)'
Write-Host ' 21 = V3.5 coarse chunk MSOC + divisor 4'
Write-Host ' 22 = V3.5 coarse chunk MSOC + proven Lua fast path'
Write-Host ' 23 = V3.5 full combined: coarse MSOC + Lua fast + divisor 4 + dynamic far reuse'
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
Write-Host ''
Write-Host 'V3.7 hitch-path experiments:' -ForegroundColor Magenta
Write-Host ' 33 = V3.7 normal candidate (V3.6 profile + active-event fast path + keyframe preload + relaxed cache sweep)'
Write-Host ' 34 = V3.7 active-event fast path isolated'
Write-Host ' 35 = V3.7 companion-keyframe preload isolated + hitch attribution'
Write-Host ' 36 = V3.7 hitch combined + deep attribution'
Write-Host ' 37 = V3.7 adapter-aware speculative preload admission isolated'
Write-Host ' 38 = V3.7 far-cascade texel stabilization at 6144 shadow distance'
Write-Host ' 39 = V3.8 clean traversal baseline (proven stack, new mechanisms off)'
Write-Host ' 40 = V3.8 world batching conservative'
Write-Host ' 41 = V3.8 world batching moderate'
Write-Host ' 42 = V3.8 world batching aggressive'
Write-Host ' 43 = V3.8 GPU residency conservative'
Write-Host ' 44 = V3.8 GPU residency moderate'
Write-Host ' 45 = V3.8 GPU residency aggressive'
Write-Host ' 46 = V3.8 traversal combined conservative'
Write-Host ' 47 = V3.8 traversal combined moderate'
Write-Host ' 48 = V3.8 traversal combined aggressive'
Write-Host ' 49 = V3.8 far-shadow conservative (2.5px)'
Write-Host ' 50 = V3.8 far-shadow moderate (3.5px)'
Write-Host ' 51 = V3.8 far-shadow aggressive (5px)'
Write-Host ' 52 = V3.8 compile pacing conservative'
Write-Host ' 53 = V3.8 compile pacing balanced'
Write-Host ' 54 = V3.8 compile pacing aggressive preparation'
Write-Host ' 55 = V3.9 V3.8-safe reference (Mode 46 equivalent)'
Write-Host ' 56 = V3.9 frontloaded strong batching'
Write-Host ' 57 = V3.9 combined candidate'
Write-Host ' 58 = V3.9 aggressive frontload / batching / residency'
Write-Host ' 59 = V3.10 fresh-frontload control (no post-transform)'
Write-Host ' 60 = V3.10 fresh frontload + post-transform 3x3'
Write-Host ' 61 = V3.10 combined post-transform + 5px far shadow'
Write-Host ' 62 = V3.10 post-transform 5x5 startup coverage'
Write-Host ' 63 = V3.11 exact active-grid strong + shared state (FIRST TEST)'
Write-Host ' 64 = V3.11 exact active-grid strong + post-transform'
Write-Host ' 65 = V3.11 Mode63 + 5px far shadow combined'
Write-Host ' 66 = V3.11 Mode64 + 5px far shadow combined'
Write-Host ' 67 = V3.12 exact Mode66 control'
Write-Host ' 68 = V3.12 Mode66 + ETA/deadline predictor'
Write-Host ' 69 = V3.12 Mode66 + safe Lua bytecode precompile'
Write-Host ' 70 = V3.12 combined ETA predictor + Lua precompile (FIRST SAFE CANDIDATE)'
Write-Host ' 71 = V3.12 combined safe + spatial prepared-active batching'
Write-Host ' 72 = V3.12 aggressive two-horizon + spatial batching'
Write-Host ' 73 = V3.13 exact Mode66 foundation control'
Write-Host ' 74 = V3.13 deterministic ObjectPaging quality repair'
Write-Host ' 75 = V3.13 quality repair + Lua precompile (RECOMMENDED)'
Write-Host ' 76 = V3.13 strict quality signature + Lua + spatial experiment'
Write-Host ' 77 = V3.14 exact promoted V3.13 Mode75 control'
Write-Host ' 78 = V3.14 balanced first-use preparation (FIRST TEST)'
Write-Host ' 79 = V3.14 aggressive recursive/groundcover preparation'
Write-Host ' 80 = V3.15 exact V3.14 Mode79 control'
Write-Host ' 81 = V3.15 premerge shared-state canonicalization'
Write-Host ' 82 = V3.15 canonicalization + 4-packet hierarchical premerge'
Write-Host ' 83 = V3.15 canonicalization + adaptive ICO governor'
Write-Host ' 84 = V3.15 full balanced candidate'
Write-Host ' 85 = V3.15 aggressive packet/governor candidate'
Write-Host ' 86 = V3.16 exact V3.15 Mode84 control'
Write-Host ' 87 = V3.16 Mode84 + 64MB streamed-audio head cache'
Write-Host ' 88 = V3.16 balanced: audio/SFX retention + idle resource maintenance'
Write-Host ' 89 = V3.16 aggressive: balanced + 384MB idle SFX predecode'
Write-Host ' 90 = V3.17 control: V3.16 Mode88 + stock LuaJIT'
Write-Host ' 91 = V3.17 Rubic0n runtime attribution'
Write-Host ' 92 = V3.17 engine Lua/materialization attribution + stock LuaJIT'
Write-Host ' 93 = V3.17 combined balanced candidate'
Write-Host ' 94 = V3.17 combined + aggressive SFX predecode'
Write-Host ' 95 = V3.18 native-resolution control (100% / bilinear path inactive)'
Write-Host ' 96 = V3.18 internal render scale 85% + bilinear upscale'
Write-Host ' 97 = V3.18 internal render scale 77% + bilinear upscale'
Write-Host ' 98 = V3.18 internal render scale 66.7% + bilinear upscale'
Write-Host ' 99 = V3.18 internal render scale 85% + NVIDIA Image Scaling'
Write-Host '100 = V3.18 internal render scale 77% + NVIDIA Image Scaling (first NIS test)'
Write-Host '101 = V3.18 internal render scale 66.7% + NVIDIA Image Scaling'
Write-Host '102 = V3.19 CPU control: native + OSG auto + focus every frame'
Write-Host '103 = V3.19 focus temporal coherence: refresh every 2 frames'
Write-Host '104 = V3.19 focus temporal coherence aggressive: refresh every 3 frames'
Write-Host '105 = V3.19 OSG CullDrawThreadPerContext + focus every frame'
Write-Host '106 = V3.19 OSG CullThreadPerCameraDrawThreadPerContext + focus every frame'
Write-Host '107 = V3.19 CullDrawThreadPerContext + focus every 2 frames'
Write-Host '108 = V3.19 per-camera cull/draw + focus every 2 frames'
Write-Host '109 = V3.20 CP1 exact P0/off fallback: focus every frame'
Write-Host '110 = V3.20 CP1 promoted fixed focus cadence 2'
Write-Host '111 = V3.20 CP1 aggressive fixed focus cadence 3'
Write-Host '112 = V3.20 CP1 adaptive camera-dirty focus, cadence bound 2'
Write-Host '113 = V3.20 CP1 adaptive camera-dirty focus, cadence bound 3'
Write-Host '114 = V3.20 CP2 exact P0 Lua control'
Write-Host '115 = V3.20 CP2 engine event handler fastpaths only'
Write-Host '116 = V3.20 CP2 pure sound ID/path conversion cache only'
Write-Host '117 = V3.20 CP2 combined engine event + sound conversion fastpaths'
Write-Host '118 = V3.20 CP3 exact P0 sound-query control'
Write-Host '119 = V3.20 CP3 same-frame sound-query coalescing only'
Write-Host '120 = V3.20 CP3 CP2-combined + sound-query coalescing'
Write-Host '121 = V3.20 CP6 exact P0 stock-LuaJIT control'
Write-Host '122 = V3.20 CP6 safe-JIT-only causal mode'
Write-Host '123 = V3.20 CP6 combined stack with stock LuaJIT'
Write-Host '124 = V3.20 CP6 combined stack with safe LuaJIT'
Write-Host '125 = V3.21 CP1 exact final V3.20 foundation control'
Write-Host '126 = V3.21 CP1 fixed completed-work admission governor'
Write-Host '127 = V3.21 CP1 adaptive slack/debt completed-work governor'
Write-Host '128 = reserved; no CP1 combination implemented'
Write-Host '129 = V3.21 CP2 class-aware completion fairness/dephasing'
Write-Host '130 = V3.21 CP3 true full-body first person (Mode129 + FBFP)'
Write-Host '131 = V3.21 CP4 full-body shadow and animation compatibility'
Write-Host '135 = V3.22 CP1 final V3.21 CP4 behavior control'
Write-Host '136 = V3.22 CP1 MSOC hot-path cache (Mode135 + cached camera inverse/PagedData)'
Write-Host '137 = V3.22 CP2 front-to-back occluder budget (400 radius)'
Write-Host '138 = V3.22 CP2 utility-ranked occluder budget (400 radius)'
Write-Host '139 = V3.22 CP2 utility-ranked clean 300-radius eligibility'
Write-Host '140 = V3.22 CP2 aggressive 300-radius + redundant-raster suppression'
Write-Host '141 = V3.22 parallel immutable actor-avoidance prediction'
Write-Host '142 = V3.23 parallel MSOC parity + dedicated QoS terrain worker'
Write-Host '143 = V3.23 strong parallel paged MSOC (1.5x range/budget)'
Write-Host '144 = V3.23 aggressive parallel paged MSOC (2x range/budget, default-off)'
Write-Host '145 = V3.24 frame-job QoS infrastructure (no experimental workload)'
Write-Host '146 = V3.24 zero-wait async terrain MSOC on opportunistic QoS lane'
Write-Host '149 = V3.25 V3.24-behavior control (actor batching OFF)'
Write-Host '150 = V3.25 batched NPC animation-source finalization'
Write-Host '151 = V3.25 CP2 batched + parallel NIF controller-clone preparation'
do { $choice = Read-Host 'Enter a listed mode (1-127, 129-131, or 135-146, 149-151)' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20','21','22','23','24','25','26','27','28','29','30','31','32','33','34','35','36','37','38','39','40','41','42','43','44','45','46','47','48','49','50','51','52','53','54','55','56','57','58','59','60','61','62','63','64','65','66','67','68','69','70','71','72','73','74','75','76','77','78','79','80','81','82','83','84','85','86','87','88','89','90','91','92','93','94','95','96','97','98','99','100','101','102','103','104','105','106','107','108','109','110','111','112','113','114','115','116','117','118','119','120','121','122','123','124','125','126','127','129','130','131','135','136','137','138','139','140','141','142','143','144','145','146','149','150','151'))
switch ($choice) {
    '1' { $Experiment = 'baseline'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'off' }
    '2' { $Experiment = 'hibernation'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'off' }
    '3' { $Experiment = 'adaptive-v2'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
    '4' { $Experiment = 'hibernation-adaptive-v2'; $Hibernation = 'true'; $Prepared = 'false'; $Scheduler = 'adaptive-v2' }
    '5' { $Experiment = 'prepared-v1'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'off' }
    '6' { $Experiment = 'adaptive-v1'; $Hibernation = 'false'; $Prepared = 'false'; $Scheduler = 'adaptive' }
    '7' { $Experiment = 'legacy-combined'; $Hibernation = 'false'; $Prepared = 'true'; $Scheduler = 'adaptive' }
    '8' { $Experiment = 'all-experimental'; $Hibernation = 'true'; $Prepared = 'true'; $Scheduler = 'adaptive-v2' }
    '9' { $Experiment = 'v33-preload-budget'; $PreloadBudget = '2' }
    '10' { $Experiment = 'v33-far-shadow-reuse'; $FarShadowInterval = '2' }
    '11' { $Experiment = 'v33-framepacing-gpu'; $PreloadBudget = '2'; $FarShadowInterval = '2' }
    '12' { $Experiment = 'v33-idle-timer-fast-path'; $LuaIdleTimerFastPath = 'true' }
    '13' { $Experiment = 'v33-far-cascade-half-res'; $FarShadowResolutionDivisor = '2' }
    '14' { $Experiment = 'v33-tail-gpu-combined'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '2' }
    '15' { $Experiment = 'v34-broadened-msoc'; $V34BroadenOcclusion = 'true'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '16' { $Experiment = 'v34-aggressive-far-shadow'; $FarShadowResolutionDivisor = '4' }
    '17' { $Experiment = 'v34-msoc-lua'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '19' { $Experiment = 'v35-coarse-msoc'; $V35CoarseChunkOcclusion = 'true' }
    '20' { $Experiment = 'v35-bounded-far-reuse'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
    '21' { $Experiment = 'v35-coarse-shadow'; $V35CoarseChunkOcclusion = 'true'; $FarShadowResolutionDivisor = '4' }
    '22' { $Experiment = 'v35-coarse-lua'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true' }
    '23' { $Experiment = 'v35-full-combined'; $V35CoarseChunkOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $V35AllowDynamicFarReuse = 'true'; $FarShadowInterval = '2'; $FarShadowResolutionDivisor = '4' }
    '24' { $Experiment = 'v36-true-custom-baseline'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false' }
    '25' { $Experiment = 'v36-normal-profile'; $V36PerformanceProfile = 'true' }
    '26' { $Experiment = 'v36-normal-gpu-profiler'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true' }
    '27' { $Experiment = 'v36-far-caster-pruning-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V36FarCasterMinimumPixels = '2.0' }
    '28' { $Experiment = 'v36-coarse-msoc-isolated'; $RamCacheMode = 'normal'; $V35CoarseChunkOcclusion = 'true' }
    '29' { $Experiment = 'v36-hitch-attribution-only'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V36Attribution = $true }
    '30' { $Experiment = 'v36-steady-combined'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0' }
    '31' { $Experiment = 'v36-hitch-combined'; $V36PerformanceProfile = 'true'; $V36Attribution = $true }
    '32' { $Experiment = 'v36-full-diagnostic'; $V36PerformanceProfile = 'true'; $V36AsyncGpuProfiler = 'true'; $V36FarCasterMinimumPixels = '2.0'; $V36Attribution = $true }
    '33' { $Experiment = 'v37-normal-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true' }
    '34' { $Experiment = 'v37-active-event-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37ActiveEventFastPath = 'true' }
    '35' { $Experiment = 'v37-keyframe-preload-isolated'; $RamCacheMode = 'normal'; $OcclusionCulling = 'false'; $V37CompanionKeyframePreload = 'true'; $V36Attribution = $true }
    '36' { $Experiment = 'v37-hitch-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37CompanionKeyframePreload = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V36Attribution = $true }
    '37' { $Experiment = 'v37-vram-admission-isolated'; $V36PerformanceProfile = 'true'; $V37GpuMemoryManagement = 'true' }
    '38' { $Experiment = 'v37-far-stabilization-6144'; $V36PerformanceProfile = 'true'; $V37StabilizeFarCascade = 'true'; $ShadowDistance = '6144' }
    '39' { $Experiment = 'v38-traversal-baseline'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true' }
    '40' { $Experiment = 'v38-batching-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1' }
    '41' { $Experiment = 'v38-batching-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2' }
    '42' { $Experiment = 'v38-batching-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3' }
    '43' { $Experiment = 'v38-residency-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '1' }
    '44' { $Experiment = 'v38-residency-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '2' }
    '45' { $Experiment = 'v38-residency-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38GpuResidencyMode = '3' }
    '46' { $Experiment = 'v38-combined-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '1' }
    '47' { $Experiment = 'v38-combined-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '2'; $V38CompilePacingMode = '2' }
    '48' { $Experiment = 'v38-combined-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3' }
    '49' { $Experiment = 'v38-shadow-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '1' }
    '50' { $Experiment = 'v38-shadow-moderate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '2' }
    '51' { $Experiment = 'v38-shadow-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38FarShadowMode = '3' }
    '52' { $Experiment = 'v38-compile-conservative'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '1' }
    '53' { $Experiment = 'v38-compile-balanced'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '2' }
    '54' { $Experiment = 'v38-compile-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38CompilePacingMode = '3' }
    '55' { $Experiment = 'v39-v38-safe-reference'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '1'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '1' }
    '56' { $Experiment = 'v39-frontloaded-batching'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1' }
    '57' { $Experiment = 'v39-combined-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '2'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '2'; $V39ProactiveResidencyMode = '2' }
    '58' { $Experiment = 'v39-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '3'; $V38GpuResidencyMode = '3'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '3'; $V39ProactiveResidencyMode = '3' }
    '59' { $Experiment = 'v310-fresh-frontload-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true' }
    '60' { $Experiment = 'v310-posttransform-3x3'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V310PreloadPostTransform = 'true' }
    '61' { $Experiment = 'v310-combined-candidate'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V310PreloadPostTransform = 'true' }
    '62' { $Experiment = 'v310-posttransform-5x5'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '3'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V310PreloadPostTransform = 'true' }
    '63' { $Experiment = 'v311-exact-active-shared'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '1' }
    '64' { $Experiment = 'v311-exact-active-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '1'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '65' { $Experiment = 'v311-combined-shared'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '1' }
    '66' { $Experiment = 'v311-combined-posttransform'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '67' { $Experiment = 'v312-mode66-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '68' { $Experiment = 'v312-eta-predictor'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0' }
    '69' { $Experiment = 'v312-lua-precompile'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true' }
    '70' { $Experiment = 'v312-combined-safe'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true' }
    '71' { $Experiment = 'v312-spatial-batching'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '1'; $V312PredictorLeadSeconds = '3.0'; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1' }
    '72' { $Experiment = 'v312-aggressive-horizon'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312PredictorMode = '2'; $V312PredictorLeadSeconds = '4.0'; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1' }
    '73' { $Experiment = 'v313-mode66-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2' }
    '74' { $Experiment = 'v313-quality-repair'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V313ChunkQualityMode = '1' }
    '75' { $Experiment = 'v313-quality-lua'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1' }
    '76' { $Experiment = 'v313-strict-spatial'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V312SpatialBatchMode = '1'; $V313ChunkQualityMode = '2' }
    '77' { $Experiment = 'v314-mode75-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1' }
    '78' { $Experiment = 'v314-balanced'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '1'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '1'; $V314PostfxCompileWarmup = 'true' }
    '79' { $Experiment = 'v314-aggressive-prep'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true' }
    '80' { $Experiment = 'v315-mode79-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true' }
    '81' { $Experiment = 'v315-state-canonical'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true' }
    '82' { $Experiment = 'v315-packet4'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1' }
    '83' { $Experiment = 'v315-ico-governor'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315AdaptiveCompileGovernor = '1' }
    '84' { $Experiment = 'v315-balanced-full'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1' }
        '86' { $Experiment = 'v316-mode84-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1' }
        '87' { $Experiment = 'v316-audio64'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64' }
        '88' { $Experiment = 'v316-balanced-hitch'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true' }
        '89' { $Experiment = 'v316-aggressive-hitch'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '128'; $V316BufferCacheMin = '512'; $V316BufferCacheMax = '768'; $V316SfxPredecodeCacheSize = '384'; $V316SfxPredecodeWorkers = '1'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true' }
        '90' { $Experiment = 'v317-stock-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false' }
        '91' { $Experiment = 'v317-rubicon-only'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'false' }
        '92' { $Experiment = 'v317-engine-lua-only'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'true' }
        '93' { $Experiment = 'v317-combined-balanced'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'true' }
        '94' { $Experiment = 'v317-combined-aggressive-sfx'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '128'; $V316BufferCacheMin = '512'; $V316BufferCacheMax = '768'; $V316SfxPredecodeCacheSize = '384'; $V316SfxPredecodeWorkers = '1'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'rubicon'; $V317EngineLuaOptimizations = 'true' }
        '95' { $Experiment = 'v318-native-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true' }
        '96' { $Experiment = 'v318-bilinear-85'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.85'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true' }
        '97' { $Experiment = 'v318-bilinear-77'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.77'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true' }
        '98' { $Experiment = 'v318-bilinear-667'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.6666667'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true' }
        '99' { $Experiment = 'v318-nis-85'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.85'; $V318Upscaler = 'nis'; $V318RenderScaleManaged = 'true'; $V318UpscalerSharpness = '0.20' }
        '100' { $Experiment = 'v318-nis-77'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.77'; $V318Upscaler = 'nis'; $V318RenderScaleManaged = 'true'; $V318UpscalerSharpness = '0.20' }
        '101' { $Experiment = 'v318-nis-667'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '0.6666667'; $V318Upscaler = 'nis'; $V318RenderScaleManaged = 'true'; $V318UpscalerSharpness = '0.20' }
        '102' { $Experiment = 'v319-cpu-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = '' }
        '103' { $Experiment = 'v319-focus2'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = '' }
        '104' { $Experiment = 'v319-focus3'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '3'; $V319OsgThreading = '' }
        '105' { $Experiment = 'v319-osg-culldraw'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = 'CullDrawThreadPerContext' }
        '106' { $Experiment = 'v319-osg-percamera'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = 'CullThreadPerCameraDrawThreadPerContext' }
        '107' { $Experiment = 'v319-osg-culldraw-focus2'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = 'CullDrawThreadPerContext' }
        '108' { $Experiment = 'v319-osg-percamera-focus2'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = 'CullThreadPerCameraDrawThreadPerContext' }
        '109' { $Experiment = 'v320-cp1-p0-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0' }
        '110' { $Experiment = 'v320-cp1-fixed2'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0' }
        '111' { $Experiment = 'v320-cp1-fixed3'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '3'; $V319OsgThreading = ''; $V320FocusAdaptive = '0' }
        '112' { $Experiment = 'v320-cp1-adaptive2'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '1' }
        '113' { $Experiment = 'v320-cp1-adaptive3'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '3'; $V319OsgThreading = ''; $V320FocusAdaptive = '1' }
        '114' { $Experiment = 'v320-cp2-p0-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '0' }
        '115' { $Experiment = 'v320-cp2-engine-events'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '0' }
        '116' { $Experiment = 'v320-cp2-sound-conversion'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '1' }
        '117' { $Experiment = 'v320-cp2-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1' }
        '118' { $Experiment = 'v320-cp3-p0-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '0'; $V320SoundQueryCoalescing = '0' }
        '119' { $Experiment = 'v320-cp3-sound-query'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '0'; $V320SoundQueryCoalescing = '1' }
        '120' { $Experiment = 'v320-cp3-combined'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1' }
        '121' { $Experiment = 'v320-cp6-p0-stock-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '0'; $V320SoundQueryCoalescing = '0' }
        '122' { $Experiment = 'v320-cp6-safejit-only'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'safejit'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '1'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '0'; $V320SoundConversionCache = '0'; $V320SoundQueryCoalescing = '0' }
        '123' { $Experiment = 'v320-cp6-combined-stock'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1' }
        '124' { $Experiment = 'v320-cp6-combined-safejit'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'safejit'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1' }
        '125' { $Experiment = 'v321-cp1-v320-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0' }
        '126' { $Experiment = 'v321-cp1-fixed-completion-governor'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '1' }
        '127' { $Experiment = 'v321-cp1-adaptive-completion-governor'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '2' }
        '129' { $Experiment = 'v321-cp2-fairness-dephasing'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1' }
        '130' { $Experiment = 'v321-cp3-fullbody-first-person'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1' }
        '131' { $Experiment = 'v321-cp4-shadow-compat'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1' }
        '135' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1' }
        '136' { $Experiment = 'v322-cp1-msoc-hotpath'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322CP1MsocHotPath = '1' }
        '137' { $Experiment = 'v322-cp2-front-to-back-400'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322CP1MsocHotPath = '1'; $V322CP2OccluderMode = '1' }
        '138' { $Experiment = 'v322-cp2-utility-400'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322CP1MsocHotPath = '1'; $V322CP2OccluderMode = '2' }
        '139' { $Experiment = 'v322-cp2-utility-300'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322CP1MsocHotPath = '1'; $V322CP2OccluderMode = '3' }
        '140' { $Experiment = 'v322-cp2-utility-300-redundant-skip'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322CP1MsocHotPath = '1'; $V322CP2OccluderMode = '4' }
        '141' { $Experiment = 'v322-parallel-actor-avoidance'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V322ParallelActorAvoidance = '1' }
        '142' { $Experiment = 'v323-parallel-msoc-parity'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V323ParallelMsocMode = '1' }
        '143' { $Experiment = 'v323-parallel-msoc-strong'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V323ParallelMsocMode = '2' }
        '144' { $Experiment = 'v323-parallel-msoc-aggressive'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V323ParallelMsocMode = '3' }
        '145' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V324FrameJobQos = '1' }
        '146' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V324FrameJobQos = '1'; $V324AsyncMsoc = '1' }
        '149' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V324FrameJobQos = '1' }
        '150' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V324FrameJobQos = '1'; $V325ActorSourceBatch = '1' }
        '151' { $Experiment = 'v322-cp1-v321-control'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '1'; $V315AdaptiveCompileGovernor = '1'; $V316HeadCacheSize = '64'; $V316BufferCacheMin = '256'; $V316BufferCacheMax = '384'; $V316SfxMetadataFrontload = 'true'; $V316IdleResourceSweep = 'true'; $V317LuaRuntime = 'stock'; $V317EngineLuaOptimizations = 'false'; $V318RenderScale = '1.0'; $V318Upscaler = 'bilinear'; $V318RenderScaleManaged = 'true'; $V319FocusCadence = '2'; $V319OsgThreading = ''; $V320FocusAdaptive = '0'; $V320EngineLuaFastPaths = '1'; $V320SoundConversionCache = '1'; $V320SoundQueryCoalescing = '1'; $V321CompletionGovernor = '0'; $V321CP2Fairness = '1'; $V321CP3FullBodyFirstPerson = '1'; $V321CP4ShadowCompat = '1'; $V324FrameJobQos = '1'; $V325ActorSourceBatch = '1'; $V325ParallelActorBinding = '1' }
    '85' { $Experiment = 'v315-aggressive-full'; $V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true'; $V315PremergeStateCanonicalization = 'true'; $V315PacketizedPremergeMode = '2'; $V315AdaptiveCompileGovernor = '2' }
}

# Preserve inherited and exact-P0 allocator identity. Substantive V3.20 modes
# expose the runtime recorder capability, but recording remains off until the
# in-game console start command is issued.
if ([int]$choice -ge 110 -and $choice -notin @('114','118','121')) {
    $V320LuaProfilerRecorderCapable = '1'
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
}

$V324DeepTelemetry = '0'
Write-Host ''
Write-Host 'V3.24 deep optimization telemetry:' -ForegroundColor Cyan
Write-Host '  0 = OFF (identical-binary observer-effect control)'
Write-Host '  1 = ON  (invasive self-accounting mechanics/physics/animation/render/QoS/MSOC trace)'
do { $deepChoice = Read-Host 'Enter 0 or 1' } until ($deepChoice -in @('0','1'))
if ($deepChoice -eq '1') {
    $V324DeepTelemetry = '1'
    $Experiment = "$Experiment-deep"
}

$Stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$ProfileDir = Join-Path $ProfilesRoot ("V3_{0}_{1}_{2}" -f $Mode, $Experiment, $Stamp)
New-Item -ItemType Directory -Force -Path $ProfileDir | Out-Null
$BackupSettings = Join-Path $ProfileDir 'settings-before-test.cfg'
Copy-Item -LiteralPath $SettingsPath -Destination $BackupSettings -Force
if (Test-Path -LiteralPath $OpenmwCfgPath) { Copy-Item -LiteralPath $OpenmwCfgPath -Destination (Join-Path $ProfileDir 'openmw.cfg') -Force }
$ciIdPath = Join-Path $GameDir 'CI-ID.txt'
if (Test-Path -LiteralPath $ciIdPath) { Copy-Item -LiteralPath $ciIdPath -Destination (Join-Path $ProfileDir 'CI-ID.txt') -Force }
$buildCommit = 'unknown'
if (Test-Path -LiteralPath $ciIdPath) {
    $commitLine = Get-Content -LiteralPath $ciIdPath | Where-Object { $_ -match '^Commit\s+' } | Select-Object -First 1
    if ($commitLine) { $buildCommit = ($commitLine -replace '^Commit\s+', '').Trim() }
}

$exeHash = (Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
$manifest = @(
    "mode=$Mode",
    "experiment=$Experiment",
    "timestamp=$([DateTimeOffset]::Now.ToString('o'))",
    "openmw_exe_sha256=$exeHash",
    "v317_lua_runtime=$V317LuaRuntime",
    "v317_engine_lua_optimizations=$V317EngineLuaOptimizations",
    "v318_render_scale=$V318RenderScale",
    "v318_upscaler=$V318Upscaler",
    "v318_upscaler_sharpness=$V318UpscalerSharpness",
    "v319_focus_cadence=$V319FocusCadence",
    "v319_osg_threading=$V319OsgThreading",
    "v320_focus_adaptive=$V320FocusAdaptive",
    "v321_completion_governor=$V321CompletionGovernor",
    "v321_cp2_fairness=$V321CP2Fairness",
    "v321_cp3_fullbody_first_person=$V321CP3FullBodyFirstPerson",
    "v321_cp4_shadow_compat=$V321CP4ShadowCompat",
    "v322_cp1_msoc_hot_path=$V322CP1MsocHotPath",
    "v322_cp2_occluder_efficiency_mode=$V322CP2OccluderMode",
    "v322_parallel_actor_avoidance=$V322ParallelActorAvoidance",
    "v323_parallel_msoc_mode=$V323ParallelMsocMode",
    "v324_frame_job_qos=$V324FrameJobQos",
    "v324_async_msoc=$V324AsyncMsoc",
    "v325_actor_source_batch=$V325ActorSourceBatch",
    "v325_parallel_actor_binding=$V325ParallelActorBinding",
    "v320_engine_lua_fastpaths=$V320EngineLuaFastPaths",
    "v320_sound_conversion_cache=$V320SoundConversionCache",
    "v320_sound_query_coalescing=$V320SoundQueryCoalescing",
    "v320_lua_profiler_recorder_capable=$V320LuaProfilerRecorderCapable",
    "v318_render_scale_managed=$V318RenderScaleManaged",
    "build_commit=$buildCommit",
    "game_dir=$GameDir",
    "v324_deep_telemetry=$V324DeepTelemetry",
    "v33_speculative_preload_budget=$PreloadBudget",
    "v33_far_shadow_update_interval=$FarShadowInterval",
    "v33_far_shadow_max_texel_drift=$FarShadowMaxTexelDrift",
    "v33_idle_timer_fast_path=$LuaIdleTimerFastPath",
    "v33_far_shadow_resolution_divisor=$FarShadowResolutionDivisor",
    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "v35_coarse_chunk_occlusion=$V35CoarseChunkOcclusion",
    "v35_allow_dynamic_far_reuse=$V35AllowDynamicFarReuse",
    "v36_performance_profile=$V36PerformanceProfile",
    "v36_disable_ram_overdrive=$V36DisableRamOverdrive",
    "v36_disable_lua_fast_path=$V36DisableLuaFastPath",
    "v36_disable_coarse_chunk_occlusion=$V36DisableCoarseChunkOcclusion",
    "v36_async_gpu_profiler=$V36AsyncGpuProfiler",
    "v36_far_caster_minimum_pixels=$V36FarCasterMinimumPixels",
    "v36_deep_attribution=$V36Attribution",
    "v37_active_event_fast_path=$V37ActiveEventFastPath",
    "v37_companion_keyframe_preload=$V37CompanionKeyframePreload",
    "v37_relaxed_resource_sweep=$V37RelaxedResourceSweep",
    "v37_resource_sweep_seconds=$V37ResourceSweepSeconds",
    "v37_gpu_memory_management=$V37GpuMemoryManagement",
    "v37_stabilize_far_cascade=$V37StabilizeFarCascade",
    "v38_world_batching_mode=$V38WorldBatchingMode",
    "v38_world_batching_merge_multiplier=$V38WorldBatchingMergeMultiplier",
    "v38_world_batching_min_instances=$V38WorldBatchingMinInstances",
    "v38_gpu_residency_mode=$V38GpuResidencyMode",
    "v38_far_shadow_mode=$V38FarShadowMode",
    "v38_compile_pacing_mode=$V38CompilePacingMode",
    "v39_frontload_mode=$V39FrontloadMode",
    "v39_batch_optimizer_mode=$V39BatchOptimizerMode",
    "v39_proactive_residency_mode=$V39ProactiveResidencyMode",
    "v310_fresh_initial_object_paging=$V310FreshInitialObjectPaging",
    "v310_preload_posttransform=$V310PreloadPostTransform",
    "v311_active_grid_prepare_mode=$V311ActiveGridPrepareMode",
    "v312_predictor_mode=$V312PredictorMode",
    "v312_predictor_lead_seconds=$V312PredictorLeadSeconds",
    "v312_lua_precompile=$V312LuaPrecompile",
    "v312_spatial_batch_mode=$V312SpatialBatchMode",
    "v313_chunk_quality_mode=$V313ChunkQualityMode",
    "v314_lua_dependency_precompile_mode=$V314LuaDependencyPrecompileMode",
    "v314_lua_package_prototype_reuse=$V314LuaPackagePrototypeReuse",
    "v314_groundcover_compile_mode=$V314GroundcoverCompileMode",
    "v314_postfx_compile_warmup=$V314PostfxCompileWarmup",
    "v315_premerge_state_canonicalization=$V315PremergeStateCanonicalization",
    "v315_packetized_premerge_mode=$V315PacketizedPremergeMode",
    "v315_adaptive_compile_governor=$V315AdaptiveCompileGovernor",
    "shadow_distance=$ShadowDistance",
    "benchmark_groundcover_density=1.0",
    "occlusion_occluder_min_radius=$OccluderMinRadius",
    "occlusion_occluder_max_distance=$OccluderMaxDistance",
    "occlusion_max_triangles=$OcclusionMaxTriangles"
)
[System.IO.File]::WriteAllLines((Join-Path $ProfileDir 'TEST_MODE.txt'), $manifest, [System.Text.UTF8Encoding]::new($false))
Write-HardwareSnapshot (Join-Path $ProfileDir 'hardware.txt')

# Remove any inherited profiler variables before selecting this test's streams.
$allVars = @(
    'OPENMW_V3_HITCH_FILE','OPENMW_V3_FRAME_FILE','OPENMW_V3_TELEMETRY_FILE','OPENMW_V3_EVENT_FILE',
    'OPENMW_V3_LUASYNC_FILE','OPENMW_V3_LUA_ACTION_FILE','OPENMW_V3_LUA_UPDATE_FILE','OPENMW_V3_TRANSITION_FILE',
    'OPENMW_V3_PAGING_FILE','OPENMW_V3_RESOURCE_FILE','OPENMW_V3_NAV_FILE','OPENMW_V3_INSERT_FILE',
    'OPENMW_V3_WORKQUEUE_FILE','OPENMW_V3_RENDER_FILE','OPENMW_V3_POSTFX_FILE','OPENMW_V3_STREAMING_FILE',
    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V32_GPU_MEMORY_FILE',
    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_V33_FRAME_SUMMARY_FILE','OPENMW_V33_LUA_CALLBACK_FILE',
    'OPENMW_V35_LUA_LOAD_FILE','OPENMW_V36_GPU_PASS_FILE','OPENMW_V36_LUA_ADDSCRIPT_FILE',
    'OPENMW_V36_CONTROLLER_FILE','OPENMW_V36_RESIDENCY_FILE','OPENMW_V36_BATCHING_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST','OPENMW_V320_LUA_PROFILE_DIR',
    'OPENMW_V320_LUA_PROFILER_CAPABLE',
    'OPENMW_V324_DEEP_TELEMETRY','OPENMW_V324_DEEP_FILE'
)
foreach ($name in $allVars) { Remove-Item "Env:$name" -ErrorAction SilentlyContinue }
Remove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue
if ($V317EngineLuaOptimizations -eq 'true') { $env:OPENMW_V317_LUA_OPT = '1' }

$env:OPENMW_V320_LUA_PROFILE_DIR = $ProfileDir
$env:OPENMW_V3_HITCH_FILE = Join-Path $ProfileDir 'v3-hitch.csv'
$env:OPENMW_V3_FRAME_FILE = Join-Path $ProfileDir 'v3-frames.csv'
if ($V324DeepTelemetry -eq '1') {
    $env:OPENMW_V324_DEEP_TELEMETRY = '1'
    $env:OPENMW_V324_DEEP_FILE = Join-Path $ProfileDir 'v324-deep-trace.csv'
}
$env:OPENMW_V33_FRAME_SUMMARY_FILE = Join-Path $ProfileDir 'v33-frame-summary.csv'
$env:OPENMW_V32_GPU_MEMORY_FILE = Join-Path $ProfileDir 'v3-gpu-memory.csv'
if ($V36AsyncGpuProfiler -eq 'true') {
    $env:OPENMW_V36_GPU_PASS_FILE = Join-Path $ProfileDir 'v36-gpu-passes.csv'
}
if ($V36Attribution) {
    $env:OPENMW_V36_LUA_ADDSCRIPT_FILE = Join-Path $ProfileDir 'v36-lua-addscript.csv'
    $env:OPENMW_V36_CONTROLLER_FILE = Join-Path $ProfileDir 'v36-controller-build.csv'
    $env:OPENMW_V36_RESIDENCY_FILE = Join-Path $ProfileDir 'v36-source-residency.csv'
    $env:OPENMW_V36_BATCHING_FILE = Join-Path $ProfileDir 'v36-static-batching-audit.csv'
}

if ($Mode -eq 'City') {
    # Unified V3.4 benchmark: traversal/Lua plus render/GPU/MSOC/shadow in the same game run.
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_POSTFX_FILE = Join-Path $ProfileDir 'v3-postfx.csv'
    $env:OPENMW_V3_MSOC_DETAIL_FILE = Join-Path $ProfileDir 'v3-msoc-detail.csv'
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_LUASYNC_FILE = Join-Path $ProfileDir 'v3-luasync.csv'
    $env:OPENMW_V3_LUA_ACTION_FILE = Join-Path $ProfileDir 'v3-lua-actions.csv'
    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V33_LUA_CALLBACK_FILE = Join-Path $ProfileDir 'v33-lua-callbacks.csv'
    $env:OPENMW_V35_LUA_LOAD_FILE = Join-Path $ProfileDir 'v35-lua-loads.csv'
    $env:OPENMW_V3_TRANSITION_FILE = Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_NAV_FILE = Join-Path $ProfileDir 'v3-nav.csv'
    $env:OPENMW_V3_TRACE_FILE = Join-Path $ProfileDir 'v3-trace.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RESOURCE_FILE = Join-Path $ProfileDir 'v3-resource.csv'
    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V32_RENDER_INSERT_FILE = Join-Path $ProfileDir 'v3-render-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
    $env:OPENMW_V3_SHADOW_FILE = Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}
elseif ($Mode -eq 'Transition') {
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_EVENT_FILE = Join-Path $ProfileDir 'v3-events.csv'
    $env:OPENMW_V3_LUASYNC_FILE = Join-Path $ProfileDir 'v3-luasync.csv'
    $env:OPENMW_V3_LUA_ACTION_FILE = Join-Path $ProfileDir 'v3-lua-actions.csv'
    $env:OPENMW_V3_LUA_UPDATE_FILE = Join-Path $ProfileDir 'v3-lua-update.csv'
    $env:OPENMW_V3_TRANSITION_FILE = Join-Path $ProfileDir 'v3-transition.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RESOURCE_FILE = Join-Path $ProfileDir 'v3-resource.csv'
    $env:OPENMW_V3_NAV_FILE = Join-Path $ProfileDir 'v3-nav.csv'
    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V32_RENDER_INSERT_FILE = Join-Path $ProfileDir 'v3-render-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_STREAMING_FILE = Join-Path $ProfileDir 'v3-streaming.csv'
    $env:OPENMW_V3_TRACE_FILE = Join-Path $ProfileDir 'v3-trace.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}
else {
    $env:OPENMW_V3_TELEMETRY_FILE = Join-Path $ProfileDir 'v3-occlusion.csv'
    $env:OPENMW_V3_PAGING_FILE = Join-Path $ProfileDir 'v3-paging.csv'
    $env:OPENMW_V3_RENDER_FILE = Join-Path $ProfileDir 'v3-render.csv'
    $env:OPENMW_V3_POSTFX_FILE = Join-Path $ProfileDir 'v3-postfx.csv'
    $env:OPENMW_V3_MSOC_DETAIL_FILE = Join-Path $ProfileDir 'v3-msoc-detail.csv'
    $env:OPENMW_V3_SHADOW_FILE = Join-Path $ProfileDir 'v3-shadow.csv'
    $env:OPENMW_OSG_STATS_FILE = Join-Path $ProfileDir 'v3-osg-stats.log'
    $env:OPENMW_OSG_STATS_LIST = 'times;resource'
}

$changedSettings = $false
$V317RuntimeSwapped = $false
try {
    $changedSettings = $true
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory telemetry' 'true'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu memory management' $V37GpuMemoryManagement
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu soft budget mb' '6800'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 gpu hard budget mb' '7400'
    Set-IniValue $SettingsPath 'Cells' 'v3.2 exterior hibernation' $Hibernation
    Set-IniValue $SettingsPath 'Cells' 'v3.2 renderer insertion profiling' $RendererProfiling
    Set-IniValue $SettingsPath 'Cells' 'v3.2 streaming max defers' '2'
    Set-IniValue $SettingsPath 'Cells' 'v3.3 speculative preload budget' $PreloadBudget
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade update interval' $FarShadowInterval
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade max texel drift' $FarShadowMaxTexelDrift
    Set-IniValue $SettingsPath 'Shadows' 'v3.3 far cascade resolution divisor' $FarShadowResolutionDivisor
    Set-IniValue $SettingsPath 'Shadows' 'v3.5 allow dynamic far cascade reuse' $V35AllowDynamicFarReuse
    Set-IniValue $SettingsPath 'Shadows' 'maximum shadow map distance' $ShadowDistance
    Set-IniValue $SettingsPath 'V3' 'v3.6 performance profile' $V36PerformanceProfile
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable ram overdrive' $V36DisableRamOverdrive
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable lua fast path' $V36DisableLuaFastPath
    Set-IniValue $SettingsPath 'V3' 'v3.6 disable coarse chunk occlusion' $V36DisableCoarseChunkOcclusion
    Set-IniValue $SettingsPath 'V3' 'v3.6 async gpu profiler' $V36AsyncGpuProfiler
    Set-IniValue $SettingsPath 'V3' 'v3.6 far caster minimum pixels' $V36FarCasterMinimumPixels
    Set-IniValue $SettingsPath 'V3' 'v3.7 active event fast path' $V37ActiveEventFastPath
    Set-IniValue $SettingsPath 'V3' 'v3.7 companion keyframe preload' $V37CompanionKeyframePreload
    Set-IniValue $SettingsPath 'V3' 'v3.7 relaxed resource cache sweep' $V37RelaxedResourceSweep
    Set-IniValue $SettingsPath 'V3' 'v3.7 resource cache sweep seconds' $V37ResourceSweepSeconds
    Set-IniValue $SettingsPath 'V3' 'v3.7 stabilize far shadow cascade' $V37StabilizeFarCascade
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching mode' $V38WorldBatchingMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching merge multiplier' $V38WorldBatchingMergeMultiplier
    Set-IniValue $SettingsPath 'V3' 'v3.8 world batching min instances' $V38WorldBatchingMinInstances
    Set-IniValue $SettingsPath 'V3' 'v3.8 gpu residency mode' $V38GpuResidencyMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 far shadow mode' $V38FarShadowMode
    Set-IniValue $SettingsPath 'V3' 'v3.8 compile pacing mode' $V38CompilePacingMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 frontload mode' $V39FrontloadMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 batch optimizer mode' $V39BatchOptimizerMode
    Set-IniValue $SettingsPath 'V3' 'v3.9 proactive residency mode' $V39ProactiveResidencyMode
    Set-IniValue $SettingsPath 'V3' 'v3.10 fresh initial object paging' $V310FreshInitialObjectPaging
    Set-IniValue $SettingsPath 'V3' 'v3.10 preload post-transform' $V310PreloadPostTransform
    Set-IniValue $SettingsPath 'V3' 'v3.11 active grid prepare mode' $V311ActiveGridPrepareMode
    Set-IniValue $SettingsPath 'V3' 'v3.12 predictor mode' $V312PredictorMode
    Set-IniValue $SettingsPath 'V3' 'v3.12 predictor lead seconds' $V312PredictorLeadSeconds
    Set-IniValue $SettingsPath 'V3' 'v3.12 lua precompile' $V312LuaPrecompile
    Set-IniValue $SettingsPath 'V3' 'v3.12 spatial batch mode' $V312SpatialBatchMode
    Set-IniValue $SettingsPath 'V3' 'v3.13 chunk quality mode' $V313ChunkQualityMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 lua dependency precompile mode' $V314LuaDependencyPrecompileMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 lua package prototype reuse' $V314LuaPackagePrototypeReuse
    Set-IniValue $SettingsPath 'V3' 'v3.14 groundcover compile mode' $V314GroundcoverCompileMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 postfx compile warmup' $V314PostfxCompileWarmup
    Set-IniValue $SettingsPath 'V3' 'v3.15 premerge state canonicalization' $V315PremergeStateCanonicalization
    Set-IniValue $SettingsPath 'V3' 'v3.15 packetized premerge mode' $V315PacketizedPremergeMode
    Set-IniValue $SettingsPath 'V3' 'v3.15 adaptive compile governor' $V315AdaptiveCompileGovernor
        Set-IniValue $SettingsPath 'V3' 'v3.16 idle resource sweep' $V316IdleResourceSweep
        Set-IniValue $SettingsPath 'Sound' 'head cache size' $V316HeadCacheSize
        Set-IniValue $SettingsPath 'Sound' 'sfx predecode cache size' $V316SfxPredecodeCacheSize
        Set-IniValue $SettingsPath 'Sound' 'sfx predecode workers' $V316SfxPredecodeWorkers
        Set-IniValue $SettingsPath 'V3' 'v3.16 sfx metadata frontload' $V316SfxMetadataFrontload
        if ($V316BufferCacheMin -ne '') {
            Set-IniValue $SettingsPath 'Sound' 'buffer cache min' $V316BufferCacheMin
            Set-IniValue $SettingsPath 'Sound' 'buffer cache max' $V316BufferCacheMax
        }
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling' $OcclusionCulling
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling terrain' 'true'
    Set-IniValue $SettingsPath 'Camera' 'occlusion culling statics' 'true'
    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'v3.5 coarse chunk occlusion' $V35CoarseChunkOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder max distance' $OccluderMaxDistance
    Set-IniValue $SettingsPath 'Camera' 'occlusion max triangles' $OcclusionMaxTriangles
    # Benchmark invariant: historical V3 comparison runs use groundcover density 1.0.
    # settings.cfg is restored in finally, so normal-play density is preserved after the run.
    Set-IniValue $SettingsPath 'Groundcover' 'density' '1.0'
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming scheduler' $Scheduler
    Set-IniValue $SettingsPath 'Cells' 'v3 streaming target frametime' '25'
    Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache' $Prepared
    Set-IniValue $SettingsPath 'Cells' 'v3 prepared instance cache max' '8192'
    Set-IniValue $SettingsPath 'Cells' 'ram cache mode' $RamCacheMode
    Set-IniValue $SettingsPath 'Cells' 'ram cache overdrive preload' 'balanced'
    if ($V318RenderScaleManaged -eq 'true') {
        Set-IniValue $SettingsPath 'Video' 'render scale' $V318RenderScale
        Set-IniValue $SettingsPath 'Video' 'upscaler' $V318Upscaler
        Set-IniValue $SettingsPath 'Video' 'upscaler sharpness' $V318UpscalerSharpness
        $changedSettings = $true
    }
    Copy-Item -LiteralPath $SettingsPath -Destination (Join-Path $ProfileDir 'settings-effective-test.cfg') -Force

    Write-Host ''
    Write-Host "V3 $Mode test: $Experiment" -ForegroundColor Green
    if ($Mode -eq 'City') {
        Write-Host 'Unified benchmark: use the same outdoor city save. Hold the usual heavy outdoor view for ~45 sec, then walk the same 2-3 minute route across several cell boundaries. Avoid doors. One run captures render/GPU/MSOC/shadow + Lua/traversal telemetry.'
    }
    elseif ($Mode -eq 'Transition') {
        Write-Host 'Suggested route: same save outside Anvil -> walk 30 sec -> enter Fighters Guild 20 sec -> exit 20 sec -> re-enter 10 sec -> exit 20 sec -> quit.'
    }
    else {
        Write-Host 'Suggested route: stay in one representative outdoor area for 45-60 sec. Move/rotate normally, but avoid doors and long-distance travel.'
    }
    Write-Host ''

    $memoryCsv = Join-Path $ProfileDir 'v3-process-memory.csv'
    'epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s' | Set-Content -LiteralPath $memoryCsv -Encoding Ascii
    $start = [DateTimeOffset]::UtcNow
    $runtimeRoot = Join-Path $GameDir 'v317-runtime'
    $stockLua = Join-Path $runtimeRoot 'stock\lua51.dll'
    $rubiconLua = Join-Path $runtimeRoot 'rubicon\lua51.dll'
    $safeJitLua = Join-Path $runtimeRoot 'safejit\lua51.dll'
    $rootLua = Join-Path $GameDir 'lua51.dll'
    $selectedLua = if ($V317LuaRuntime -eq 'rubicon') { $rubiconLua }
        elseif ($V317LuaRuntime -eq 'safejit') { $safeJitLua }
        else { $stockLua }
    $V319StockLuaSha256 = 'A8636655927F70BAD350ED60E0F369992B32259EC8D2FD5D350E1A9A9811AE8B'
    if ($V317LuaRuntime -eq 'stock' -and -not (Test-Path -LiteralPath $stockLua)) {
        if (-not (Test-Path -LiteralPath $rootLua)) {
            throw "V3.19 stock Lua bootstrap failure: missing both $stockLua and $rootLua"
        }
        $rootLuaHash = (Get-FileHash -LiteralPath $rootLua -Algorithm SHA256).Hash
        if ($rootLuaHash -ne $V319StockLuaSha256) {
            throw "V3.19 stock Lua bootstrap failure: root lua51.dll hash mismatch ($rootLuaHash)"
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $stockLua) -Force | Out-Null
        Copy-Item -LiteralPath $rootLua -Destination $stockLua -Force
        Write-Host 'V3.19: bootstrapped verified stock LuaJIT runtime from packaged root lua51.dll.' -ForegroundColor DarkGray
    }

    foreach ($requiredLua in @($stockLua, $selectedLua)) {
        if (-not (Test-Path -LiteralPath $requiredLua)) {
            throw "V3.17 runtime identity failure: missing $requiredLua"
        }
    }
    Copy-Item -LiteralPath $selectedLua -Destination $rootLua -Force
    $selectedLuaHash = (Get-FileHash -LiteralPath $selectedLua -Algorithm SHA256).Hash
    Add-Content -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt') -Value "lua51_sha256=$selectedLuaHash" -Encoding Ascii
    if (Test-Path -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt')) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot 'V317-LUAJIT-RUNTIME.txt') -Destination (Join-Path $ProfileDir 'V317-LUAJIT-RUNTIME.txt') -Force
    }
    if (Test-Path -LiteralPath (Join-Path $runtimeRoot 'V320-SAFE-LUAJIT-RUNTIME.txt')) {
        Copy-Item -LiteralPath (Join-Path $runtimeRoot 'V320-SAFE-LUAJIT-RUNTIME.txt') -Destination (Join-Path $ProfileDir 'V320-SAFE-LUAJIT-RUNTIME.txt') -Force
    }
    $V317RuntimeSwapped = $true
    $env:OPENMW_V320_FOCUS_ADAPTIVE = $V320FocusAdaptive
    $env:OPENMW_V321_COMPLETION_GOVERNOR = $V321CompletionGovernor
    $env:OPENMW_V321_CP2_FAIRNESS = $V321CP2Fairness
    $env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON = $V321CP3FullBodyFirstPerson
    $env:OPENMW_V321_CP4_SHADOW_COMPAT = $V321CP4ShadowCompat
    $env:OPENMW_V322_CP1_MSOC_HOT_PATH = $V322CP1MsocHotPath
    $env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE = $V322CP2OccluderMode
    $env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE = $V322ParallelActorAvoidance
    $env:OPENMW_V323_PARALLEL_MSOC_MODE = $V323ParallelMsocMode
    $env:OPENMW_V324_FRAME_JOB_QOS = $V324FrameJobQos
    $env:OPENMW_V324_ASYNC_MSOC = $V324AsyncMsoc
    $env:OPENMW_V325_ACTOR_SOURCE_BATCH = $V325ActorSourceBatch
    $env:OPENMW_V325_PARALLEL_ACTOR_BINDING = $V325ParallelActorBinding
    if ($V325ParallelActorBinding -eq '1') {
        $env:OPENMW_V325_JOBGROUP_STATS_FILE = Join-Path $ProfileDir 'v325-jobgroup-summary.csv'
    }
    $env:OPENMW_V320_ENGINE_LUA_FASTPATHS = $V320EngineLuaFastPaths
    $env:OPENMW_V320_SOUND_CONVERSION_CACHE = $V320SoundConversionCache
    $env:OPENMW_V320_SOUND_QUERY_COALESCING = $V320SoundQueryCoalescing
    $env:OPENMW_V320_LUA_PROFILER_CAPABLE = $V320LuaProfilerRecorderCapable
    $env:OPENMW_V319_FOCUS_CADENCE = $V319FocusCadence
    if ([string]::IsNullOrWhiteSpace($V319OsgThreading)) {
        Remove-Item Env:OSG_THREADING -ErrorAction SilentlyContinue
    }
    else {
        $env:OSG_THREADING = $V319OsgThreading
    }
    $process = Start-Process -FilePath $Exe -WorkingDirectory $GameDir -PassThru
    while (-not $process.HasExited) {
        try {
            $process.Refresh()
            $now = [DateTimeOffset]::UtcNow
            $line = '{0},{1:F3},{2:F1},{3:F1},{4:F1},{5:F3}' -f $now.ToUnixTimeMilliseconds(), ($now - $start).TotalSeconds,
                ($process.WorkingSet64 / 1MB), ($process.PrivateMemorySize64 / 1MB), ($process.VirtualMemorySize64 / 1MB),
                $process.TotalProcessorTime.TotalSeconds
            Add-Content -LiteralPath $memoryCsv -Value $line -Encoding Ascii
        } catch {}
        Start-Sleep -Milliseconds 1000
    }
    $process.WaitForExit()
    if ($V324DeepTelemetry -eq '1') {
        $deepPath = Join-Path $ProfileDir 'v324-deep-trace.csv'
        if (Test-Path -LiteralPath $deepPath) {
            # V3.25 packaging repair: never materialize the full deep trace in PowerShell.
            # Offline analysis owns aggregation; runtime packaging must remain bounded-memory.
            $rows = @()
            if ($rows.Count -gt 0) {
                $sumSetup = [double](($rows | Measure-Object -Property scope_setup_ms -Sum).Sum)
                $sumFormat = [double](($rows | Measure-Object -Property prev_format_ms -Sum).Sum)
                $sumLock = [double](($rows | Measure-Object -Property prev_lock_wait_ms -Sum).Sum)
                $sumOpen = [double](($rows | Measure-Object -Property prev_open_ms -Sum).Sum)
                $sumWrite = [double](($rows | Measure-Object -Property prev_write_ms -Sum).Sum)
                $sumFlush = [double](($rows | Measure-Object -Property prev_flush_ms -Sum).Sum)
                $sumBytes = [double](($rows | Measure-Object -Property prev_bytes -Sum).Sum)
                $direct = $sumSetup + $sumFormat + $sumLock + $sumOpen + $sumWrite + $sumFlush
                $overhead = @(
                    "rows=$($rows.Count)",
                    "reported_prev_bytes=$([long]$sumBytes)",
                    "scope_setup_ms=$('{0:F6}' -f $sumSetup)",
                    "format_ms=$('{0:F6}' -f $sumFormat)",
                    "lock_wait_ms=$('{0:F6}' -f $sumLock)",
                    "open_ms=$('{0:F6}' -f $sumOpen)",
                    "write_ms=$('{0:F6}' -f $sumWrite)",
                    "flush_ms=$('{0:F6}' -f $sumFlush)",
                    "direct_recorded_profiler_ms=$('{0:F6}' -f $direct)",
                    "accounting_note=writer costs are carried by the following row; only the final writer operation is not directly carried forward",
                    "observer_effect_note=compare identical mode telemetry OFF vs ON; that delta bounds cache scheduling allocation and the unreported final writer operation"
                )
                [System.IO.File]::WriteAllLines((Join-Path $ProfileDir 'v324-profiler-overhead-summary.txt'), $overhead, [System.Text.UTF8Encoding]::new($false))
            }
        }
    }
}
finally {
    Remove-Item Env:OPENMW_V320_LUA_PROFILE_DIR -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_LUA_PROFILER_CAPABLE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_SOUND_QUERY_COALESCING -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_ENGINE_LUA_FASTPATHS -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_SOUND_CONVERSION_CACHE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V325_JOBGROUP_STATS_FILE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V325_PARALLEL_ACTOR_BINDING -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V325_ACTOR_SOURCE_BATCH -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V324_ASYNC_MSOC -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V324_FRAME_JOB_QOS -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V323_PARALLEL_MSOC_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V322_PARALLEL_ACTOR_AVOIDANCE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V322_CP2_OCCLUDER_EFFICIENCY_MODE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V322_CP1_MSOC_HOT_PATH -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V321_CP4_SHADOW_COMPAT -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V321_CP2_FAIRNESS -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V321_COMPLETION_GOVERNOR -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V320_FOCUS_ADAPTIVE -ErrorAction SilentlyContinue
    Remove-Item Env:OPENMW_V317_LUA_OPT -ErrorAction SilentlyContinue
    if ($V317RuntimeSwapped) {
        try { Copy-Item -LiteralPath $stockLua -Destination $rootLua -Force } catch {
            Write-Warning "Unable to restore stock V3.17 lua51.dll: $($_.Exception.Message)"
        }
    }
    if (Test-Path -LiteralPath (Join-Path $UserOpenMW 'openmw.log')) {
        Copy-Item -LiteralPath (Join-Path $UserOpenMW 'openmw.log') -Destination (Join-Path $ProfileDir 'openmw.log') -Force -ErrorAction SilentlyContinue
    }
    if ($changedSettings -and (Test-Path -LiteralPath $BackupSettings)) {
        Copy-Item -LiteralPath $BackupSettings -Destination $SettingsPath -Force
    }
}

$zipPath = "$ProfileDir.zip"
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
Compress-Archive -Path (Join-Path $ProfileDir '*') -DestinationPath $zipPath -CompressionLevel Optimal
Write-Host ''
Write-Host 'Profile complete and settings restored.' -ForegroundColor Green
Write-Host "Upload this ZIP: $zipPath" -ForegroundColor Cyan
try { Start-Process explorer.exe -ArgumentList "/select,`"$zipPath`"" } catch {}
Read-Host 'Press Enter to close'
