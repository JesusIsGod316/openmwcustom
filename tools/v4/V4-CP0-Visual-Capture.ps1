[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [Parameter(Mandatory = $true)]
    [string]$ScreenshotDir,

    [Parameter(Mandatory = $true)]
    [string]$SaveFile,

    [string]$UserConfigDir = (Join-Path $env:USERPROFILE 'Documents\My Games\OpenMW'),

    [string]$OutputDir,

    [switch]$SkipFreezeCapture,

    [switch]$PlanOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Resolve-RequiredDirectory {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label directory does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredFile {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label file does not exist: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Get-Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-LosslessImage {
    param([string]$Path)
    $ext = [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
    return $ext -in @('.png', '.bmp', '.tga', '.tif', '.tiff')
}

function Find-NewScreenshot {
    param(
        [datetime]$Since,
        [System.Collections.Generic.HashSet[string]]$Seen
    )

    $extensions = @('.png', '.bmp', '.tga', '.tif', '.tiff', '.jpg', '.jpeg')
    $candidates = @(
        Get-ChildItem -LiteralPath $ScreenshotDir -File -Recurse -ErrorAction SilentlyContinue |
            Where-Object {
                $extensions -contains $_.Extension.ToLowerInvariant() -and
                $_.LastWriteTime -ge $Since -and
                -not $Seen.Contains($_.FullName)
            } |
            Sort-Object LastWriteTime -Descending
    )

    if ($candidates.Count -eq 0) { return $null }
    return $candidates[0]
}

function Read-CapturePath {
    param(
        [string]$StepId,
        [int]$Index,
        [datetime]$Since,
        [System.Collections.Generic.HashSet[string]]$Seen
    )

    while ($true) {
        [void](Read-Host "[$StepId $Index] Take the OpenMW screenshot now, return here, then press Enter")
        $candidate = Find-NewScreenshot -Since $Since -Seen $Seen
        if ($null -ne $candidate) {
            return $candidate.FullName
        }

        $manual = Read-Host 'No new screenshot was detected. Paste the screenshot path, type R to retry, or S to skip'
        if ($manual -match '^[Rr]$') {
            $Since = (Get-Date).AddSeconds(-2)
            continue
        }
        if ($manual -match '^[Ss]$') { return $null }
        if (Test-Path -LiteralPath $manual -PathType Leaf) {
            return (Resolve-Path -LiteralPath $manual).Path
        }
        Write-Warning "Not a valid file: $manual"
    }
}

function Read-CheckResult {
    param([string]$StepId)
    while ($true) {
        $value = (Read-Host "[$StepId] Result: P=pass, F=fail, S=skip").Trim().ToUpperInvariant()
        switch ($value) {
            'P' { return 'PASS' }
            'F' { return 'FAIL' }
            'S' { return 'SKIP' }
        }
    }
}

$BuildRoot = Resolve-RequiredDirectory -Path $BuildRoot -Label 'BuildRoot'
$ScreenshotDir = Resolve-RequiredDirectory -Path $ScreenshotDir -Label 'ScreenshotDir'
$SaveFile = Resolve-RequiredFile -Path $SaveFile -Label 'Canonical save'
$UserConfigDir = Resolve-RequiredDirectory -Path $UserConfigDir -Label 'UserConfigDir'

$expectedExeHash = '34d7a715e25d92dcad6b20f807e8b44a272fd3382e2d2f0a22e03bedac3e25c2'
$exePath = Resolve-RequiredFile -Path (Join-Path $BuildRoot 'openmw.exe') -Label 'Final V3.25 openmw.exe'
$wrapperPath = Resolve-RequiredFile -Path (Join-Path $BuildRoot 'V3.25_Mode151_Gaming.bat') -Label 'Mode151 gaming wrapper'
$frozenSettingsPath = Resolve-RequiredFile -Path (Join-Path $BuildRoot 'settings-v325-mode151-gaming.cfg') -Label 'Frozen Mode151 gaming settings'
$effectiveSettingsPath = Resolve-RequiredFile -Path (Join-Path $UserConfigDir 'settings.cfg') -Label 'Effective user settings.cfg'

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $BuildRoot ('V4_CP0_VISUAL_' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$OutputDir = (Resolve-Path -LiteralPath $OutputDir).Path
$imagesDir = Join-Path $OutputDir 'images'
New-Item -ItemType Directory -Force -Path $imagesDir | Out-Null

$steps = @(
    [ordered]@{ id='EXT_DAY_01'; kind='image'; count=3; required=$true; prompt='Canonical exterior daylight anchor: terrain, distant statics, groundcover, foliage, sky/sun, shadows, PBR. Capture stable center view then slight left/right rotations.' },
    [ordered]@{ id='EXT_WEATHER_02'; kind='image'; count=2; required=$true; prompt='Repeatable exterior weather: precipitation/weather, fog, animated vegetation/groundcover, lighting.' },
    [ordered]@{ id='WATER_SURFACE_03'; kind='image'; count=2; required=$true; prompt='Shoreline showing water surface, reflection, refraction/shore, sky/statics and preferably vegetation.' },
    [ordered]@{ id='WATER_UNDER_04'; kind='image'; count=1; required=$true; prompt='Underwater just below surface looking through water boundary: fog/color/refraction/depth.' },
    [ordered]@{ id='INTERIOR_LIT_05'; kind='image'; count=2; required=$true; prompt='Representative lit interior with several materials/local lights, shadows, alpha if possible, HBAO/postfx.' },
    [ordered]@{ id='CROWD_ACTORS_06'; kind='image'; count=3; required=$true; prompt='Crowded actor scene with mixed equipment. Capture three visibly different animation frames.' },
    [ordered]@{ id='ANIM_NIF_07'; kind='image'; count=2; required=$true; prompt='Non-actor animated NIF/controller. Capture two clearly different controller states.' },
    [ordered]@{ id='ALPHA_EFFECTS_08'; kind='image'; count=3; required=$true; prompt='Alpha-test/blend plus particles/VFX/projectile/overlap if practical. Capture animated sequence.' },
    [ordered]@{ id='FBFP_MAIN_09'; kind='image'; count=2; required=$true; prompt='Frozen full-body first person: body/gear visible, owner head hidden from main view, world/postfx normal.' },
    [ordered]@{ id='FBFP_SECONDARY_10'; kind='image'; count=2; required=$true; prompt='FBFP secondary-view anchor: main body view plus player shadow; include water reflection/refraction if practical.' },
    [ordered]@{ id='FIRSTPERSON_NATIVE_VARIANT_11'; kind='image'; count=1; required=$true; prompt='EXPLICIT VARIANT: use tools/v4/V4-CP0-NativeFP-Reference.bat so only OPENMW_V321_CP3_FULL_BODY_FIRST_PERSON=0 changes while all other frozen gates/settings stay unchanged; capture equivalent native first-person view.' },
    [ordered]@{ id='UI_MAPS_12'; kind='image'; count=4; required=$true; prompt='Capture, in order: gameplay HUD; inventory/character preview; local map; world map.' },
    [ordered]@{ id='LOADING_13'; kind='image'; count=1; required=$true; prompt='Loading screen from a normal canonical interior/exterior transition.' },
    [ordered]@{ id='RCN_CHECK_14'; kind='check'; required=$true; prompt='Known RootCollisionNode/root-RCN asset: intended collision works and collision-only geometry remains hidden; recursive child behavior is correct.' },
    [ordered]@{ id='CELL_CROSSING_CHECK_15'; kind='check'; required=$true; prompt='Canonical exterior crossing: no missing/duplicate/stale statics; terrain/groundcover transitions correct; route completes.' },
    [ordered]@{ id='INTERIOR_EXTERIOR_CHECK_16'; kind='check'; required=$true; prompt='Exterior -> interior -> exterior: sky/water/terrain and fog/light state switch correctly; no stale previous-world geometry.' },
    [ordered]@{ id='SAVELOAD_ACTOR_CHECK_17'; kind='check'; required=$true; prompt='Save/reload around visibly animated/equipped actors: equipment, animation behavior and scene membership recover normally.' }
)

Write-Host ''
Write-Host 'OpenMW Custom Build V4 CP0 visual corpus capture'
Write-Host ('Build root: ' + $BuildRoot)
Write-Host ('Screenshot source: ' + $ScreenshotDir)
Write-Host ('Canonical save: ' + $SaveFile)
Write-Host ('Output: ' + $OutputDir)
Write-Host ''

if ($PlanOnly) {
    foreach ($step in $steps) {
        $count = 0
        if ($step.Contains('count')) {
            $count = [int]$step['count']
        }
        Write-Host ('{0} [{1}] x{2}: {3}' -f $step.id, $step.kind, $count, $step.prompt)
    }
    exit 0
}

# Capture the complete build/config/content/hardware identity alongside the images.
$identityDir = Join-Path $OutputDir 'identity'
if (-not $SkipFreezeCapture) {
    $freezeScript = Join-Path $PSScriptRoot 'V4-CP0-Capture.ps1'
    if (-not (Test-Path -LiteralPath $freezeScript -PathType Leaf)) {
        throw "Missing CP0 freeze capture helper: $freezeScript"
    }
    & $freezeScript -BuildRoot $BuildRoot -UserConfigDir $UserConfigDir -SaveFile $SaveFile -OutputDir $identityDir
}

$identityJson = Join-Path $identityDir 'cp0-freeze-capture.json'
$identity = $null
if (Test-Path -LiteralPath $identityJson -PathType Leaf) {
    $identity = Get-Content -LiteralPath $identityJson -Raw | ConvertFrom-Json
}

$identityChecks = [ordered]@{
    openmw_exe_sha256 = Get-Sha256 $exePath
    openmw_exe_matches_final = ((Get-Sha256 $exePath) -eq $expectedExeHash)
    wrapper_sha256 = Get-Sha256 $wrapperPath
    frozen_gaming_settings_sha256 = Get-Sha256 $frozenSettingsPath
    effective_settings_sha256 = Get-Sha256 $effectiveSettingsPath
    effective_settings_matches_frozen_gaming_profile = ((Get-Sha256 $effectiveSettingsPath) -eq (Get-Sha256 $frozenSettingsPath))
    canonical_save_sha256 = Get-Sha256 $SaveFile
    freeze_capture_json_sha256 = if (Test-Path -LiteralPath $identityJson -PathType Leaf) { Get-Sha256 $identityJson } else { $null }
    freeze_capture_all_required_present = if ($null -ne $identity) { [bool]$identity.verification.all_required_files_present } else { $false }
}

if (-not $identityChecks.openmw_exe_matches_final) {
    throw "openmw.exe does not match the frozen final V3.25 executable hash. Refusing visual capture."
}
if (-not $identityChecks.effective_settings_matches_frozen_gaming_profile) {
    throw "Effective user settings.cfg does not byte-match settings-v325-mode151-gaming.cfg. Copy the frozen gaming profile into the active user settings before capturing the base corpus."
}

$records = @()
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::OrdinalIgnoreCase)

foreach ($step in $steps) {
    Write-Host ''
    Write-Host ('=== ' + $step.id + ' ===') -ForegroundColor Cyan
    Write-Host $step.prompt

    if ($step.kind -eq 'image') {
        $count = [int]$step.count
        for ($i = 1; $i -le $count; ++$i) {
            $since = (Get-Date).AddSeconds(-2)
            $source = Read-CapturePath -StepId $step.id -Index $i -Since $since -Seen $seen
            if ($null -eq $source) {
                $records += [ordered]@{
                    id = $step.id
                    kind = 'image'
                    index = $i
                    status = 'MISSING'
                    source = $null
                    canonical = $null
                    sha256 = $null
                    bytes = $null
                    lossless = $false
                }
                continue
            }

            [void]$seen.Add($source)
            $ext = [System.IO.Path]::GetExtension($source).ToLowerInvariant()
            if ([string]::IsNullOrWhiteSpace($ext)) { $ext = '.png' }
            $canonicalName = '{0}_{1:d2}{2}' -f $step.id, $i, $ext
            $canonicalPath = Join-Path $imagesDir $canonicalName
            Copy-Item -LiteralPath $source -Destination $canonicalPath -Force
            $item = Get-Item -LiteralPath $canonicalPath
            $records += [ordered]@{
                id = $step.id
                kind = 'image'
                index = $i
                status = 'CAPTURED'
                source = $source
                canonical = $canonicalPath
                sha256 = Get-Sha256 $canonicalPath
                bytes = [int64]$item.Length
                lossless = Get-LosslessImage $canonicalPath
            }
            Write-Host ('Captured ' + $canonicalName)
        }
    }
    else {
        $result = Read-CheckResult -StepId $step.id
        $note = Read-Host 'Short note / location / asset used'
        $records += [ordered]@{
            id = $step.id
            kind = 'check'
            status = $result
            note = $note
        }
    }
}

$imageRecords = @($records | Where-Object { $_.kind -eq 'image' })
$checkRecords = @($records | Where-Object { $_.kind -eq 'check' })
$missingImages = @($imageRecords | Where-Object { $_.status -ne 'CAPTURED' })
$lossyImages = @($imageRecords | Where-Object { $_.status -eq 'CAPTURED' -and -not $_.lossless })
$failedChecks = @($checkRecords | Where-Object { $_.status -ne 'PASS' })

# Deterministic canonical corpus description. Timestamps and free-form notes are intentionally
# excluded from this digest so the digest describes the frozen evidence, not JSON formatting.
$canonicalLines = @(
    'OMW_V4_CP0_VISUAL_CORPUS_1'
    ('EXE|' + $identityChecks.openmw_exe_sha256)
    ('WRAPPER|' + $identityChecks.wrapper_sha256)
    ('SETTINGS|' + $identityChecks.frozen_gaming_settings_sha256)
    ('SAVE|' + $identityChecks.canonical_save_sha256)
)
foreach ($record in $imageRecords | Sort-Object id, index) {
    $canonicalLines += ('IMAGE|{0}|{1:d2}|{2}|{3}' -f $record.id, $record.index, $record.status, $record.sha256)
}
foreach ($record in $checkRecords | Sort-Object id) {
    $canonicalLines += ('CHECK|{0}|{1}' -f $record.id, $record.status)
}
$canonicalPath = Join-Path $OutputDir 'visual-corpus-canonical.txt'
$canonicalLines | Set-Content -LiteralPath $canonicalPath -Encoding UTF8
$corpusHash = Get-Sha256 $canonicalPath

$accepted = (
    $identityChecks.openmw_exe_matches_final -and
    $identityChecks.effective_settings_matches_frozen_gaming_profile -and
    $identityChecks.freeze_capture_all_required_present -and
    $missingImages.Count -eq 0 -and
    $lossyImages.Count -eq 0 -and
    $failedChecks.Count -eq 0
)

$manifest = [ordered]@{
    schema = 'OMW_V4_CP0_VISUAL_CORPUS_1'
    generated_utc = (Get-Date).ToUniversalTime().ToString('o')
    event = 'evt.v4.0.cp0.visual_corpus.capture'
    control = 'V3.25 Mode151 final frozen gaming profile'
    output_dir = $OutputDir
    screenshot_dir = $ScreenshotDir
    identity = $identityChecks
    records = $records
    canonical_manifest = [ordered]@{
        path = $canonicalPath
        sha256 = $corpusHash
    }
    verification = [ordered]@{
        missing_image_count = $missingImages.Count
        lossy_image_count = $lossyImages.Count
        failed_or_skipped_check_count = $failedChecks.Count
        accepted = $accepted
    }
}

$manifestPath = Join-Path $OutputDir 'visual-corpus-manifest.json'
$manifest | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

$summaryPath = Join-Path $OutputDir 'visual-corpus-summary.txt'
$summary = @(
    'OpenMW Custom Build V4 CP0 visual corpus'
    ('Generated UTC: ' + $manifest.generated_utc)
    ('Corpus SHA256: ' + $corpusHash)
    ('Final V3.25 executable match: ' + $identityChecks.openmw_exe_matches_final)
    ('Effective settings match frozen gaming profile: ' + $identityChecks.effective_settings_matches_frozen_gaming_profile)
    ('Freeze capture required files present: ' + $identityChecks.freeze_capture_all_required_present)
    ('Captured image records: ' + @($imageRecords | Where-Object { $_.status -eq 'CAPTURED' }).Count + '/' + $imageRecords.Count)
    ('Lossy images: ' + $lossyImages.Count)
    ('Semantic checks PASS: ' + @($checkRecords | Where-Object { $_.status -eq 'PASS' }).Count + '/' + $checkRecords.Count)
    ('CP0 visual corpus accepted: ' + $accepted)
    ''
    'If accepted=True, archive this entire directory unchanged. If false, repeat only missing/failed steps before using the corpus as the V4 parity oracle.'
)
$summary | Set-Content -LiteralPath $summaryPath -Encoding UTF8

Write-Host ''
Write-Host 'Visual corpus capture finished.'
Write-Host ('Manifest: ' + $manifestPath)
Write-Host ('Canonical digest: ' + $corpusHash)
Write-Host ('Accepted: ' + $accepted)
Write-Host ('Output: ' + $OutputDir)

if (-not $accepted) {
    exit 2
}
