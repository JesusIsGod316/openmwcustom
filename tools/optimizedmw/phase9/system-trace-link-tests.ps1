$ErrorActionPreference='Stop'
foreach($name in @('OptimizedMW_SystemTraceLink.ps1','system-trace-link-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
. (Join-Path $PSScriptRoot 'OptimizedMW_SystemTraceLink.ps1')
function Assert-Link([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
$script:recorderAlive=$true;$script:recorderBirth=[DateTime]::Parse('2026-10-01T10:00:00.0000007Z')
function Get-Process {
    param([int]$Id,[string]$ErrorAction)
    Assert-Link ($Id -eq 42) 'Unexpected recorder process queried'
    if($script:recorderAlive){[pscustomobject]@{StartTime=$script:recorderBirth}}
}
function New-LinkFixtureZip {
    param([string]$Path,[object]$Link)
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    Add-Type -AssemblyName System.IO.Compression
    $output=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
    $archive=$null
    try{
        $archive=[IO.Compression.ZipArchive]::new($output,[IO.Compression.ZipArchiveMode]::Create,$true)
        $entry=$archive.CreateEntry('SYSTEM-TRACE-LINK.json')
        $writer=[IO.StreamWriter]::new($entry.Open())
        try{$writer.Write(($Link | ConvertTo-Json -Depth 8))}finally{$writer.Dispose()}
        $entry=$archive.CreateEntry('binary.bin');$stream=$entry.Open()
        try{$stream.Write([byte[]]@(0,1,255),0,3)}finally{$stream.Dispose()}
    }finally{if($archive){$archive.Dispose()};$output.Dispose()}
}
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$temp=Join-Path $tempRoot ('p9-system-link-tests-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temp | Out-Null
try{
    $game=Join-Path $temp 'game';$profile=Join-Path $temp 'profile'
    New-Item -ItemType Directory -Path $game,$profile | Out-Null
    $exe=Join-Path $game 'openmw.exe';[IO.File]::WriteAllBytes($exe,[byte[]]@(1,2,3))
    $hash=(Get-FileHash -LiteralPath $exe).Hash
    $capture=Join-Path $game 'SystemTrace\run';New-Item -ItemType Directory -Path $capture | Out-Null
    $activePath=Join-Path $game 'SYSTEM-TRACE-ACTIVE.json'
    Assert-Link ($null -eq (New-Phase9SystemTraceLink $game $profile $exe $hash)) 'Absent recorder changed normal launcher behavior'
    $active=[pscustomobject]@{schema=2;status='recording';started=$true;normal_stop=$false;
        capture_id='0123456789abcdef0123456789abcdef';instance='OptimizedMW-0123456789abcdef0123456789abcdef';
        recorder_process_id=42;recorder_creation_utc=$script:recorderBirth.ToUniversalTime().ToString('o');
        directory=$capture;game_directory=$game;executable_path=$exe;executable_sha256=$hash;
        recording_started_utc='2026-10-01T10:00:01.0000000Z'}
    foreach($field in @('status','instance','executable_path','executable_sha256','recorder_creation_utc')){
        $saved=$active.$field
        $active.$field=$(if($field -eq 'executable_path'){Join-Path $game 'other.exe'}elseif($field -eq 'recorder_creation_utc'){'2026-10-01T10:00:00.0000008Z'}else{'mismatch'})
        Write-Phase9SystemTraceLinkJson $activePath $active
        Assert-Link ($null -eq (New-Phase9SystemTraceLink $game $profile $exe $hash)) ('Stale/mismatched recorder accepted: '+$field)
        Assert-Link (-not [IO.File]::Exists((Join-Path $profile 'SYSTEM-TRACE-LINK.json'))) 'Rejected active recorder wrote a profile link'
        $active.$field=$saved
    }
    Write-Phase9SystemTraceLinkJson $activePath $active
    $script:recorderAlive=$false
    Assert-Link ($null -eq (New-Phase9SystemTraceLink $game $profile $exe $hash)) 'Dead recorder accepted'
    $script:recorderAlive=$true
    $link=New-Phase9SystemTraceLink $game $profile $exe $hash
    Assert-Link ($link.capture_id -eq $active.capture_id -and $link.recorder_creation_utc.EndsWith('0000007Z')) 'Live link lost capture or birth precision'
    $linkPath=Join-Path $profile 'SYSTEM-TRACE-LINK.json'
    $linkHash=(Get-FileHash -LiteralPath $linkPath).Hash
    $zipPath=Join-Path $temp 'final.zip';New-LinkFixtureZip $zipPath $link
    $zipHash=(Get-FileHash -LiteralPath $zipPath).Hash
    $state=[pscustomobject]@{schema=2;status='saving';normal_stop=$false;
        capture_id=$active.capture_id;instance=$active.instance;recorder_process_id=42;recorder_creation_utc=$active.recorder_creation_utc}
    Write-Phase9SystemTraceLinkJson (Join-Path $capture 'SYSTEM-TRACE-STATE.json') $state
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OptimizedMW_QualifySystemTrace.ps1') -Destination $game
    $birth='2026-10-01T10:00:02.0000009Z'
    $binding=Complete-Phase9SystemTraceLink $link $zipPath 77 $birth
    $bindingPath=Join-Path $capture 'SYSTEM-TRACE-ENGINE-BINDING.json'
    $read=Get-Content -Raw -LiteralPath $bindingPath | ConvertFrom-Json
    Assert-Link ($read.schema -eq 1 -and $read.capture_id -eq $link.capture_id -and $read.game_process_id -eq 77 -and
        (ConvertTo-Phase9SystemTraceUtcString $read.game_creation_utc) -eq $birth -and $read.engine_zip_bytes -eq (Get-Item -LiteralPath $zipPath).Length -and
        $read.engine_zip_sha256 -eq $zipHash -and (ConvertTo-Phase9SystemTraceUtcString $read.recorder_creation_utc) -eq $link.recorder_creation_utc) 'Final binding lost immutable ZIP or process identity'
    Assert-Link ([IO.File]::Exists((Join-Path $capture 'SYSTEM-TRACE-QUALIFICATION-PENDING.json'))) 'Saving-state handshake did not produce pending qualification'
    Assert-Link (-not [IO.File]::Exists((Join-Path $capture 'SYSTEM-TRACE-ENGINE-QUALIFICATION.json'))) 'Saving-state handshake claimed final qualification'
    Assert-Link ((Get-FileHash -LiteralPath $zipPath).Hash -eq $zipHash -and (Get-FileHash -LiteralPath $linkPath).Hash -eq $linkHash) 'Completion mutated final ZIP or profile'
    $bindingHash=(Get-FileHash -LiteralPath $bindingPath).Hash
    # Saved recorder invokes qualification without polling or rewriting the ZIP.
    $script:qualifierCalls=0
    function Invoke-Phase9SystemTraceQualification {
        param([string]$GameDir,[string]$CaptureDirectory,[string]$ProfileZip)
        ++$script:qualifierCalls
        Assert-Link ([IO.File]::Exists((Join-Path $CaptureDirectory 'SYSTEM-TRACE-ENGINE-BINDING.json'))) 'Qualifier ran before binding publication'
        Assert-Link ((Get-FileHash -LiteralPath $ProfileZip).Hash -eq $zipHash) 'Qualifier received an intermediate ZIP'
    }
    $state.status='saved';$state.normal_stop=$true
    Write-Phase9SystemTraceLinkJson (Join-Path $capture 'SYSTEM-TRACE-STATE.json') $state
    $null=Complete-Phase9SystemTraceLink $link $zipPath 77 $birth
    Assert-Link ($script:qualifierCalls -eq 1 -and (Get-FileHash -LiteralPath $bindingPath).Hash -eq $bindingHash) 'Idempotent final binding was rewritten or qualifier was not invoked'
    $null=Complete-Phase9SystemTraceLink $link $zipPath 78 $birth
    Assert-Link ($script:qualifierCalls -eq 1 -and (Get-FileHash -LiteralPath $bindingPath).Hash -eq $bindingHash) 'Another game replaced the published binding'
    function Invoke-Phase9SystemTraceQualification {param($GameDir,$CaptureDirectory,$ProfileZip);throw 'mock qualifier failure'}
    $result=Complete-Phase9SystemTraceLink $link $zipPath 77 $birth
    Assert-Link ($result.capture_id -eq $link.capture_id -and (Get-FileHash -LiteralPath $zipPath).Hash -eq $zipHash) 'Qualifier failure lost binding or changed final ZIP'
    Assert-Link ($null -eq (Complete-Phase9SystemTraceLink $null $zipPath 77 $birth)) 'No-link control did extra work'
    Write-Host 'PASS system trace linkage: no-active control, stale/status/path/hash/native birth rejection, pending/saved handshake, immutable ZIP binding, identity fields, idempotence and nonfatal qualification failure'
}finally{
    $resolved=[IO.Path]::GetFullPath($temp)
    Assert-Link ($resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)) 'Unsafe linkage fixture cleanup path'
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
