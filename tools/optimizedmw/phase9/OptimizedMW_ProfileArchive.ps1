# Post-game only. Keep a verified raw archive before attempting any report.
$ErrorActionPreference = 'Stop'

function New-VerifiedProfileZip {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$SourceDir,
        [Parameter(Mandatory=$true)][string]$DestinationPath,
        [switch]$ReplaceExisting
    )
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $source = [IO.Path]::GetFullPath($SourceDir).TrimEnd([IO.Path]::DirectorySeparatorChar)
    $destination = [IO.Path]::GetFullPath($DestinationPath)
    if(-not [IO.Directory]::Exists($source)){throw "Profile directory is missing: $source"}
    if($destination.StartsWith($source+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
        throw 'Profile ZIP must be outside the profile directory'
    }
    if([IO.File]::Exists($destination) -and -not $ReplaceExisting){throw "Archive already exists: $destination"}
    if(-not [IO.File]::Exists((Join-Path $source 'TEST_MODE.txt'))){throw 'Profile has no TEST_MODE.txt identity'}

    $expected=@('TEST_MODE.txt','v3-frame.csv','p6-render-traversal.csv','openmw.log')
    $missing=@($expected | Where-Object {-not [IO.File]::Exists((Join-Path $source $_))})
    # Missing deferred capture after a crash is evidence, not a reason to lose the ZIP.
    [ordered]@{schema=1;expected_files=$expected;missing_expected_files=$missing;
        complete_required_file_set=($missing.Count -eq 0);
        note='File presence does not prove normal finish or complete telemetry. Inspect exit and capture status.'} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $source 'PROFILE-CAPTURE.json') -Encoding UTF8
    $files=@(Get-ChildItem -LiteralPath $source -Recurse -File -Force)
    if(@($files | Where-Object {($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0}).Count){
        throw 'Refusing to archive reparse-point files outside the owned profile'
    }
    $inventory=@($files | ForEach-Object {
        [pscustomobject]@{name=$_.FullName.Substring($source.Length+1).Replace('\','/');
            bytes=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
    })
    $temp=$destination+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    $lastError=$null
    for($attempt=1;$attempt -le 3;$attempt++){
        try{
            if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}
            # Windows PowerShell targets an older .NET runtime contract whose
            # CreateFromDirectory may write backslash entry names. Specify ZIP
            # entry names explicitly so Linux and Windows verify/extract alike.
            $output=[IO.File]::Open($temp,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
            $writer=$null
            try{
                $writer=[IO.Compression.ZipArchive]::new($output,[IO.Compression.ZipArchiveMode]::Create,$true)
                foreach($file in $inventory){
                    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($writer,
                        (Join-Path $source $file.name),$file.name,[IO.Compression.CompressionLevel]::Optimal)
                }
            }finally{
                if($null -ne $writer){$writer.Dispose()}
                $output.Dispose()
            }
            $zip=[IO.Compression.ZipFile]::OpenRead($temp)
            try{
                $entries=@{};foreach($entry in $zip.Entries){
                    if($entry.FullName.EndsWith('/')){continue}
                    if($entries.ContainsKey($entry.FullName)){throw 'Duplicate archive entry'}
                    $entries[$entry.FullName]=$entry
                }
                if($entries.Count -ne $inventory.Count){throw 'Archive inventory does not match the raw profile'}
                foreach($file in $inventory){
                    $entry=$entries[$file.name]
                    if($null -eq $entry -or $entry.Length -ne $file.bytes){throw ('Missing/truncated archive entry: '+$file.name)}
                    $stream=$entry.Open();$hasher=[Security.Cryptography.SHA256]::Create()
                    try{$hash=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','')}
                    finally{$stream.Dispose();$hasher.Dispose()}
                    if($hash -ne $file.sha256){throw ('Archive data failed SHA256 verification: '+$file.name)}
                }
            }finally{$zip.Dispose()}
            # Preserve any already-verified raw ZIP until its replacement is verified.
            if([IO.File]::Exists($destination)){
                if(-not $ReplaceExisting){throw 'Archive destination appeared during packaging'}
                # PowerShell 5.1 coerces ordinary $null to an empty string here.
                # Pass a true null backup path without deleting the verified raw ZIP.
                [IO.File]::Replace($temp,$destination,[System.Management.Automation.Language.NullString]::Value)
            }else{[IO.File]::Move($temp,$destination)}
            return [pscustomobject]@{Path=$destination;Files=$inventory.Count;Missing=$missing}
        }catch{
            $lastError=$_
            if($attempt -lt 3){Start-Sleep -Milliseconds (200*$attempt)}
        }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
    }
    throw "Profile archive failed after 3 attempts: $lastError"
}

function Invoke-BoundedOfflineReport {
    param([Parameter(Mandatory=$true)][string]$ReportScript,
          [Parameter(Mandatory=$true)][string]$ProfileDir,
          [ValidateRange(1,120)][int]$TimeoutSeconds=30)
    if(-not [IO.File]::Exists($ReportScript)){throw 'Offline report script is missing'}
    $hostExe=(Get-Process -Id $PID).Path
    # Windows file names cannot contain quotes. Quote paths containing spaces or brackets.
    if($ReportScript.Contains('"') -or $ProfileDir.Contains('"')){throw 'Unsupported quoted report path'}
    $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$ReportScript+'"'),
        '-ProfileDir',('"'+$ProfileDir+'"'))
    $report=Start-Process -FilePath $hostExe -ArgumentList $args -PassThru -NoNewWindow `
        -RedirectStandardOutput (Join-Path $ProfileDir 'report-stdout.txt') `
        -RedirectStandardError (Join-Path $ProfileDir 'report-stderr.txt')
    try{
        if(-not $report.WaitForExit($TimeoutSeconds*1000)){
            $report.Kill();$report.WaitForExit()
            throw "Offline report exceeded $TimeoutSeconds seconds; verified raw ZIP is retained"
        }
        $report.WaitForExit()
        if($report.ExitCode -ne 0){throw "Offline report exited with code $($report.ExitCode); verified raw ZIP is retained"}
    }finally{$report.Dispose()}
}

function Complete-Phase9Profile {
    param([Parameter(Mandatory=$true)][string]$ProfileDir,
          [Parameter(Mandatory=$true)][string]$GameDir)
    $zipName=(Split-Path -Leaf $ProfileDir)+'.zip'
    $zip=$null;$failures=[Collections.Generic.List[string]]::new()
    # Prefer beside the launcher; installations in protected folders use the profile parent.
    foreach($directory in @($GameDir,(Split-Path -Parent $ProfileDir))){
        try{$zip=New-VerifiedProfileZip -SourceDir $ProfileDir -DestinationPath (Join-Path $directory $zipName);break}
        catch{[void]$failures.Add([string]$_)}
    }
    if($null -eq $zip){
        $failures | Set-Content -LiteralPath (Join-Path $ProfileDir 'ZIP-ERROR.txt') -Encoding UTF8
        throw "ZIP CREATION FAILED. Raw evidence is safe at: $ProfileDir. $($failures -join '; ')"
    }
    Write-Host "Verified raw ZIP: $($zip.Path)" -ForegroundColor Green
    try{
        Invoke-BoundedOfflineReport -ReportScript (Join-Path $GameDir 'OptimizedMW_Benchmark_Report.ps1') -ProfileDir $ProfileDir
    }catch{
        "Offline report failed: $_" | Set-Content -LiteralPath (Join-Path $ProfileDir 'REPORT-ERROR.txt') -Encoding UTF8
        Write-Host 'Report unavailable; the raw benchmark evidence is already zipped.' -ForegroundColor Yellow
    }
    try{
        $zip=New-VerifiedProfileZip -SourceDir $ProfileDir -DestinationPath $zip.Path -ReplaceExisting
    }catch{
        "Report-enriched ZIP failed; original verified raw ZIP retained: $_" |
            Set-Content -LiteralPath (Join-Path $ProfileDir 'ZIP-REPORT-ERROR.txt') -Encoding UTF8
        Write-Host 'Keeping the already-verified raw ZIP; report files remain in the profile folder.' -ForegroundColor Yellow
    }
    return $zip
}

# This check is independent of the optional report and never deletes raw evidence.
function Test-Phase9TraceCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir,[bool]$Requested=$false)
    $errors=[Collections.Generic.List[string]]::new()
    $warnings=[Collections.Generic.List[string]]::new()
    if($Requested){
        foreach($spec in @(
            @('p9-draw-phases.csv.status.txt','total_calls'),
            @('p9-gl-calls.csv.status.txt','calls'))){
            $path=Join-Path $ProfileDir $spec[0]
            if(-not (Test-Path -LiteralPath $path)){[void]$errors.Add('Missing '+$spec[0]);continue}
            $status=Get-Content -Raw -LiteralPath $path
            $pattern='(?m)^'+[regex]::Escape($spec[1])+'=(\d+)\s*$'
            $match=[regex]::Match($status,$pattern)
            if(-not $match.Success -or [long]$match.Groups[1].Value -le 0){[void]$errors.Add('No live calls: '+$spec[0])}
            foreach($drop in @('rows_dropped','frames_dropped','uninstrumented_pool_overflow','unsupported_contexts')){
                $m=[regex]::Match($status,('(?m)^'+$drop+'=(\d+)\s*$'))
                if($m.Success -and [long]$m.Groups[1].Value -gt 0){[void]$warnings.Add($spec[0]+': '+$drop+'='+$m.Groups[1].Value)}
            }
        }
        foreach($name in @('p9-draw-phases.csv.frames.csv','p9-gl-calls.csv.frames.csv')){
            $path=Join-Path $ProfileDir $name
            if(-not (Test-Path -LiteralPath $path) -or @(Get-Content -LiteralPath $path -TotalCount 2 -ErrorAction SilentlyContinue).Count -lt 2){
                [void]$errors.Add('Missing/empty frame aggregates: '+$name)
            }
        }
    }
    $result=[pscustomobject]@{requested=$Requested;valid=($errors.Count -eq 0);complete=($errors.Count -eq 0 -and $warnings.Count -eq 0);
        errors=@($errors.ToArray());warnings=@($warnings.ToArray());scope='CPU leaf and selected extension calls; not all GL calls or GPU internals'}
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'TRACE-HEALTH.json') -Encoding UTF8
    return $result
}
