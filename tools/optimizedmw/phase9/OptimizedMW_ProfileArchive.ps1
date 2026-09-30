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
          [ValidateRange(1,120)][int]$TimeoutSeconds=90)
    if(-not [IO.File]::Exists($ReportScript)){throw 'Offline report script is missing'}
    $hostExe=(Get-Process -Id $PID).Path
    # Windows file names cannot contain quotes. Quote paths containing spaces or brackets.
    if($ReportScript.Contains('"') -or $ProfileDir.Contains('"')){throw 'Unsupported quoted report path'}
    $args=@('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"'+$ReportScript+'"'),
        '-ProfileDir',('"'+$ProfileDir+'"'))
    # ProcessStartInfo owns the live handle and literal .NET output paths avoid
    # Start-Process's Windows PowerShell wildcard handling for [bracket] paths.
    $info=[Diagnostics.ProcessStartInfo]::new()
    $info.FileName=$hostExe;$info.Arguments=$args -join ' '
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $report=[Diagnostics.Process]::new();$report.StartInfo=$info
    $stdout=$null;$stderr=$null;$stdoutTask=$null;$stderrTask=$null;$started=$false
    try{
        $stdout=[IO.File]::Create((Join-Path $ProfileDir 'report-stdout.txt'))
        $stderr=[IO.File]::Create((Join-Path $ProfileDir 'report-stderr.txt'))
        $started=$report.Start()
        if(-not $started){throw 'Offline report process did not start'}
        $null=$report.Handle
        # Drain both pipes concurrently using bounded stream buffers; waiting
        # before draining would deadlock a child whose error output fills a pipe.
        $stdoutTask=$report.StandardOutput.BaseStream.CopyToAsync($stdout)
        $stderrTask=$report.StandardError.BaseStream.CopyToAsync($stderr)
        if(-not $report.WaitForExit($TimeoutSeconds*1000)){
            $report.Kill();$report.WaitForExit()
            throw "Offline report exceeded $TimeoutSeconds seconds; verified raw ZIP is retained"
        }
        $report.WaitForExit()
        $stdoutTask.GetAwaiter().GetResult();$stderrTask.GetAwaiter().GetResult()
        if($report.ExitCode -ne 0){throw "Offline report exited with code $($report.ExitCode); verified raw ZIP is retained"}
    }finally{
        if($started -and -not $report.HasExited){$report.Kill();$report.WaitForExit()}
        if($stdoutTask){$stdoutTask.GetAwaiter().GetResult()}
        if($stderrTask){$stderrTask.GetAwaiter().GetResult()}
        if($stdout){$stdout.Dispose()};if($stderr){$stderr.Dispose()}
        $report.Dispose()
    }
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

# Presence of a ZIP is not proof the renderer tracer was attached or complete.
function Test-Phase9TraceCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir,[bool]$TraceRequested)
    $reasons=[Collections.Generic.List[string]]::new()
    $valid=$null
    $streamLoss=[ordered]@{}
    $resourceRows=0;$resourceIncoherent=0;$resourceReported=$null;$resourceCapacity=32768
    if($TraceRequested){
        $status=Join-Path $ProfileDir 'p9-draw-phases.csv.status.txt'
        $frames=Join-Path $ProfileDir 'p9-draw-phases.csv.frames.csv'
        if(-not (Test-Path -LiteralPath $status)){[void]$reasons.Add('Missing renderer coverage status')}
        else{
            $content=Get-Content -Raw -LiteralPath $status
            if($content -notmatch '(?m)^visitor_instances=[1-9][0-9]*\r?$'){
                [void]$reasons.Add('No instrumented cull visitors')
            }
            if($content -notmatch '(?m)^valid_leaf_capture=1\r?$'){
                [void]$reasons.Add('No live leaves or trace overflow: inspect coverage status')
            }
            foreach($field in @('rows_dropped','frame_rows_dropped','renderer_rows_dropped','uninstrumented_pool_overflow','resource_catalog_dropped_attempts')){
                if($content -match ('(?m)^'+[regex]::Escape($field)+'=(\d+)\r?$')){
                    $streamLoss[$field]=[long]$matches[1]
                    if($streamLoss[$field] -gt 0){[void]$reasons.Add("Trace loss: $field=$($streamLoss[$field])")}
                }else{[void]$reasons.Add("Missing explicit trace loss: $field")}
            }
            if($content -match '(?m)^resource_catalog_capacity=(\d+)\r?$'){
                $number=0L
                if([long]::TryParse($matches[1],[ref]$number) -and $number -ge 32768 -and $number -le 262144){$resourceCapacity=$number}
                else{[void]$reasons.Add('Invalid resource catalog capacity')}
                if($content -notmatch '(?m)^resource_catalog_allocation_failures=0\r?$'){
                    [void]$reasons.Add('Resource catalog allocation failed or its outcome is missing')
                }
            }elseif($content -match '(?m)^resource_catalog_capacity='){
                [void]$reasons.Add('Malformed resource catalog capacity')
            }
            if($content -match '(?m)^resource_catalog_rows=(\d+)\r?$'){
                $number=0L
                if([long]::TryParse($matches[1],[ref]$number) -and $number -le $resourceCapacity){$resourceReported=$number}
                else{[void]$reasons.Add('Invalid resource catalog row count')}
            }else{[void]$reasons.Add('Missing resource catalog row count')}
        }
        $glStatus=Join-Path $ProfileDir 'p9-draw-phases.csv.gl-status.txt'
        if(Test-Path -LiteralPath $glStatus){
            $content=Get-Content -Raw -LiteralPath $glStatus
            foreach($field in @('rows_dropped','frame_rows_dropped')){
                if($content -match ('(?m)^'+[regex]::Escape($field)+'=(\d+)\r?$')){
                    $streamLoss['gl_'+$field]=[long]$matches[1]
                    if([long]$matches[1] -gt 0){[void]$reasons.Add("GL trace loss: $field=$($matches[1])")}
                }else{[void]$reasons.Add("Missing explicit GL trace loss: $field")}
            }
        }else{[void]$reasons.Add('Missing selected GL coverage status')}
        try {
            $capture=Read-Phase9CaptureCsv $frames @('frame','context','leaves')
            if($capture.Rows.Count -eq 0 -or $capture.Malformed -gt 0 -or $capture.Loss -gt 0){
                [void]$reasons.Add('Missing or incomplete per-frame leaf evidence')
            }
            foreach($row in $capture.Rows){
                $frame=0L;$context=0L;$leaves=0L
                if(-not [long]::TryParse([string]$row.frame,[ref]$frame) -or $frame -lt 0 -or
                    -not [long]::TryParse([string]$row.context,[ref]$context) -or $context -lt 0 -or
                    -not [long]::TryParse([string]$row.leaves,[ref]$leaves) -or $leaves -le 0){
                    [void]$reasons.Add('Incoherent per-frame leaf evidence');break
                }
            }
        } catch {[void]$reasons.Add('Invalid per-frame leaf evidence')}
        try {
            $catalog=Join-Path $ProfileDir 'p9-draw-phases.csv.resources.csv'
            $capture=Read-Phase9CaptureCsv $catalog @('first_frame','last_frame','context','texture_unit','texture',
                'image','image_revision','stateset','submit_camera','bytes','scope','texture_class','filename','semantic_role') -StrictShape
            $resourceRows=$capture.Rows.Count
            if($capture.Malformed -gt 0 -or $capture.Loss -gt 0 -or $resourceRows -gt $resourceCapacity){
                [void]$reasons.Add('Malformed, incomplete or oversized resource catalog')
            }
            if($null -ne $resourceReported -and $resourceRows -ne $resourceReported){
                [void]$reasons.Add('Resource catalog row count differs from coverage status')
            }
            foreach($row in $capture.Rows){
                $values=@{};$coherent=$true
                foreach($column in @('first_frame','last_frame','context','texture_unit','image_revision','scope')){
                    $number=[uint32]0
                    if([string]$row.$column -notmatch '^\d+$' -or -not [uint32]::TryParse([string]$row.$column,[ref]$number)){$coherent=$false}
                    $values[$column]=$number
                }
                foreach($column in @('texture','image','stateset','submit_camera','bytes')){
                    $number=[uint64]0
                    if([string]$row.$column -notmatch '^\d+$' -or -not [uint64]::TryParse([string]$row.$column,[ref]$number)){$coherent=$false}
                    $values[$column]=$number
                }
                if($values.last_frame -lt $values.first_frame -or
                    ($values.context -gt 15 -and $values.context -ne [uint32]::MaxValue) -or
                    $values.scope -notin @(1,2) -or $values.texture -eq 0 -or $values.stateset -eq 0 -or
                    ($values.image -eq 0 -and ($values.image_revision -ne 0 -or $values.bytes -ne 0))){$coherent=$false}
                foreach($limit in @(@('texture_class',31),@('filename',255),@('semantic_role',127))){
                    if([Text.Encoding]::UTF8.GetByteCount([string]$row.($limit[0])) -gt $limit[1]){$coherent=$false}
                }
                if([string]::IsNullOrEmpty([string]$row.texture_class) -or [string]::IsNullOrEmpty([string]$row.semantic_role)){$coherent=$false}
                if(-not $coherent){$resourceIncoherent++}
            }
            if($resourceIncoherent){[void]$reasons.Add('Resource catalog has incoherent numeric identities, extents or labels')}
        } catch {[void]$reasons.Add('Missing or invalid resource catalog evidence')}
        $valid=($reasons.Count -eq 0)
        if(-not $valid){Write-Warning ('INVALID ROOT-CAUSE CAPTURE: '+($reasons -join '; ')+'. Raw evidence will still be zipped.')}
    }
    $result=[pscustomobject]@{schema=6;trace_requested=$TraceRequested;valid_leaf_capture=$valid;stream_loss=$streamLoss;
        resource_catalog_capacity=$resourceCapacity;
        resource_catalog_rows=$resourceRows;resource_catalog_incoherent_rows=$resourceIncoherent;reasons=$reasons.ToArray();
        scope='CPU leaf envelopes plus selected OSG GL dispatch; not complete GPU/driver profiling'}
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'ROOT-CAUSE-CAPTURE.json') -Encoding UTF8
    return $result
}


# Temporal input modes are useful only if the render-owned consumer contract and
# hardware capability probe actually emitted evidence. Missing rows are preserved
# in the raw archive but are never treated as a successful temporal capture.
function Read-Phase9WriterStatus {
    param([Parameter(Mandatory=$true)][string]$Path)
    $fields=@{}
    foreach($line in [IO.File]::ReadAllLines($Path)){
        if($line -notmatch '^([a-z_]+)=(\d+)$' -or $fields.ContainsKey($matches[1])){throw 'Malformed or duplicate writer status field'}
        $number=0L
        if(-not [long]::TryParse($matches[2],[ref]$number)){throw 'Writer status integer overflow'}
        $fields[$matches[1]]=$number
    }
    foreach($key in @('normal_finish','output_ok','rows_dropped')){
        if(-not $fields.ContainsKey($key)){throw "Missing writer status field: $key"}
    }
    if($fields.normal_finish -notin @(0,1) -or $fields.output_ok -notin @(0,1)){throw 'Invalid writer finish/output flag'}
    if($fields.output_ok -eq 1){
        foreach($bound in @(@('queue_capacity',32768),@('byte_capacity',33554432),@('row_capacity',65536))){
            if(-not $fields.ContainsKey($bound[0]) -or $fields[$bound[0]] -le 0 -or $fields[$bound[0]] -gt $bound[1]){
                throw 'Writer exceeds its declared item, byte or row bound'
            }
        }
        $sum=0L
        foreach($key in @('capacity_dropped','contention_dropped','oversized_dropped','allocation_dropped','io_dropped','stopped_dropped')){
            if(-not $fields.ContainsKey($key)){throw "Missing writer loss reason: $key"}
            $sum += $fields[$key]
        }
        if($sum -ne $fields.rows_dropped -or $fields.initialization_failed -ne 0){throw 'Writer loss reasons or initialization outcome are incoherent'}
    }
    return [pscustomobject]@{OutputOk=($fields.normal_finish -eq 1 -and $fields.output_ok -eq 1);
        Loss=$fields.rows_dropped;Fields=$fields}
}

function Test-Phase9WriterCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $modePath=Join-Path $ProfileDir 'TEST_MODE.txt'
    $mode=if(Test-Path -LiteralPath $modePath){Get-Content -Raw -LiteralPath $modePath}else{''}
    $requested=$mode -match '(?m)^phase9_capture_transport=bounded_mpsc_v1\r?$'
    $reasons=[Collections.Generic.List[string]]::new();$channels=[ordered]@{};$loss=0L
    if($requested){
        # Separate from deferred CPU/native trace channels. Failure in GPU or
        # auxiliary transport does not erase an independently complete CPU file.
        $names=@('v3-paging.csv','v3-render.csv','v3-events.csv','v3-transition.csv','v3-resource.csv',
            'v3-streaming.csv','v3-shadow.csv','v36-batching.csv','v3-gpu-memory.csv','p4-compile.csv',
            'p6-render-phase.csv','p6-render-traversal.csv','p9-temporal-inputs.csv','p9-dlss-capabilities.csv',
            'p9-gpu-passes.csv','p9-terrain-composite.csv')
        $required=@('v3-render.csv','p6-render-traversal.csv')
        if($mode -match '(?m)^phase9_temporal_inputs=1\r?$'){$required+=@('p9-temporal-inputs.csv','p9-dlss-capabilities.csv')}
        if($mode -match '(?m)^phase9_(temporal_inputs|draw_trace)=1\r?$'){$required+='p9-gpu-passes.csv'}
        foreach($name in $names){
            $csv=Join-Path $ProfileDir $name
            if(-not (Test-Path -LiteralPath $csv)){
                if($required -contains $name){[void]$reasons.Add("Missing requested writer channel: $name")}
                continue
            }
            try{
                $status=Read-Phase9WriterStatus ($csv+'.writer-status.txt')
                $channels[$name]=$status.Fields;$loss+=$status.Loss
                if(-not $status.OutputOk){[void]$reasons.Add("Writer output did not finish: $name")}
                if($status.Loss -gt 0){[void]$reasons.Add("Writer loss: $name=$($status.Loss)")}
                # Footer is within the last bounded 4 KiB, independent of CSV
                # volume. Do not materialize renderer/GPU streams just to check
                # that their final completion marker reached disk.
                $stream=[IO.File]::OpenRead($csv);$reader=$null
                try{
                    [void]$stream.Seek([Math]::Max(0,$stream.Length-4096),[IO.SeekOrigin]::Begin)
                    $reader=[IO.StreamReader]::new($stream);$tail=$reader.ReadToEnd()
                }finally{if($reader){$reader.Dispose()}else{$stream.Dispose()}}
                if($tail -notmatch '(?m)^# p9_capture_transport=bounded_mpsc_v1\r?$' -or
                    $tail -notmatch '(?m)^# v3_async_diagnostics_dropped_lines=(\d+)\r?$' -or
                    [long]$matches[1] -ne $status.Loss){[void]$reasons.Add("Missing or incoherent writer footer: $name")}
            }catch{[void]$reasons.Add("Invalid writer completion for ${name}: $_")}
        }
    }
    $valid=if($requested){$reasons.Count -eq 0}else{$null}
    $result=[pscustomobject]@{schema=1;bounded_transport_requested=$requested;valid_writer_capture=$valid;
        csv_writer_dropped=$loss;channels=$channels;reasons=$reasons.ToArray();
        scope='Shared diagnostic transport only; CPU frame/native trace validity is assessed separately'}
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $ProfileDir 'WRITER-CAPTURE.json') -Encoding UTF8
    if($requested -and -not $valid){Write-Warning ('INVALID DIAGNOSTIC WRITER CAPTURE: '+($reasons -join '; ')+'. Raw evidence will still be zipped.')}
    return $result
}

function Read-Phase9CaptureCsv {
    param([string]$Path,[string[]]$Required,[switch]$StrictShape,[switch]$RequireWriterStatus)
    $lines=[Collections.Generic.List[string]]::new()
    $loss=0L;$malformed=0L;$transport=$false;$lossFooter=$false
    $reader=[IO.StreamReader]::new($Path)
    try {
        while($null -ne ($line=$reader.ReadLine())) {
            if($line.StartsWith('#')) {
                if($line -match '^# v3_async_diagnostics_dropped_lines=(\d+)\s*$'){$loss += [long]$matches[1];$lossFooter=$true}
                if($line -eq '# p9_capture_transport=bounded_mpsc_v1'){$transport=$true}
                continue
            }
            if([string]::IsNullOrWhiteSpace($line)){continue}
            if($lines.Count -ge 2000000){throw 'Capture exceeds two million row safety bound'}
            [void]$lines.Add($line)
        }
    } finally {$reader.Dispose()}
    if($RequireWriterStatus -or $transport){
        if(-not $transport -or -not $lossFooter){throw 'Missing bounded writer transport/completeness footer'}
        $writerStatus=Read-Phase9WriterStatus ($Path+'.writer-status.txt')
        if(-not $writerStatus.OutputOk){throw 'CSV output did not finish successfully'}
        if($writerStatus.Loss -ne $loss){throw 'CSV footer differs from independent writer loss status'}
    }
    if($lines.Count -eq 0){throw 'CSV has no header'}
    $headers=@($lines[0].Split(','))
    if(@($headers | Select-Object -Unique).Count -ne $headers.Count){throw 'Duplicate CSV columns'}
    foreach($column in $Required){if($headers -notcontains $column){throw "Missing CSV column: $column"}}
    if($StrictShape){
        # ConvertFrom-Csv tolerates extra fields and some malformed quoting.
        # Resource identity evidence requires an exact CSV record shape.
        $record='^(?:[^,"\r\n]*|"(?:[^"]|"")*")(?:,(?:[^,"\r\n]*|"(?:[^"]|"")*"))*$'
        $field='(?:^|,)(?:"(?:[^"]|"")*"|[^,"\r\n]*)'
        for($i=1;$i -lt $lines.Count;$i++){
            if(-not [regex]::IsMatch($lines[$i],$record) -or [regex]::Matches($lines[$i],$field).Count -ne $headers.Count){$malformed++}
        }
    }
    $rows=@($lines.ToArray() | ConvertFrom-Csv)
    foreach($row in $rows){
        if($null -eq $row.PSObject.Properties[$headers[-1]].Value){$malformed++}
        if($headers -contains 'writer_dropped_total'){
            $reported=0L
            if(-not [long]::TryParse([string]$row.writer_dropped_total,[ref]$reported) -or $reported -lt 0){$malformed++}
            else{$loss=[Math]::Max($loss,$reported)}
        }
    }
    return [pscustomobject]@{Rows=$rows;Loss=$loss;Malformed=$malformed;Columns=$headers}
}

function Test-Phase9TemporalCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir,[bool]$TemporalRequested)
    $reasons=[Collections.Generic.List[string]]::new()
    $inputRows=0;$capabilityRows=0;$unexpectedReady=0;$loss=0L;$incoherent=0
    $ownershipRequestedRows=0;$ownershipActiveRows=0;$ownershipFallbacks=[ordered]@{};$ownershipFields=$false
    $modePath=Join-Path $ProfileDir 'TEST_MODE.txt'
    $mode=if(Test-Path -LiteralPath $modePath){Get-Content -Raw -LiteralPath $modePath}else{''}
    $requireWriter=$mode -match '(?m)^phase9_capture_transport=bounded_mpsc_v1\r?$'
    $requireOwnership=$mode -match '(?m)^phase9_temporal_contract=consumer_frame_v2_ownership\r?$'
    $ownershipControl=$null
    if($mode -match '(?m)^phase9_temporal_ownership=([01])\r?$'){$ownershipControl=[long]$matches[1]}
    if($TemporalRequested){
        $input=Join-Path $ProfileDir 'p9-temporal-inputs.csv'
        $caps=Join-Path $ProfileDir 'p9-dlss-capabilities.csv'
        try {
            $capture=Read-Phase9CaptureCsv $input @('frame','context','submitted','history_valid','previous_frame',
                'render_w','render_h','output_w','output_h','color_ptr','depth_ptr','motion_ptr','input_mask','dlss_ready') -RequireWriterStatus:$requireWriter
            $inputRows=$capture.Rows.Count;$loss=$capture.Loss
            $ownershipFields=@('ownership_requested','ownership_active','ownership_fallback_reason' | Where-Object {$capture.Columns -contains $_}).Count -eq 3
            if($requireOwnership -and -not $ownershipFields){[void]$reasons.Add('Missing effective ownership columns required by this launcher')}
            if($inputRows -eq 0){[void]$reasons.Add('Temporal input telemetry has no frame rows')}
            if($capture.Malformed){[void]$reasons.Add('Malformed temporal input rows')}
            foreach($row in $capture.Rows){
                $ready=0L;$frame=0L;$context=0L;$submitted=0L;$history=0L;$previous=0L;$mask=0L
                $valid=[long]::TryParse([string]$row.dlss_ready,[ref]$ready)
                $valid=([long]::TryParse([string]$row.frame,[ref]$frame) -and $valid)
                $valid=([long]::TryParse([string]$row.context,[ref]$context) -and $valid)
                $valid=([long]::TryParse([string]$row.submitted,[ref]$submitted) -and $valid)
                $valid=([long]::TryParse([string]$row.history_valid,[ref]$history) -and $valid)
                $valid=([long]::TryParse([string]$row.previous_frame,[ref]$previous) -and $valid)
                $valid=([long]::TryParse([string]$row.input_mask,[ref]$mask) -and $valid)
                if($ready -eq 1){$unexpectedReady++}
                if($ready -notin @(0,1) -or $submitted -notin @(0,1) -or $history -notin @(0,1) -or $frame -lt 0 -or $context -lt 0 -or $previous -lt 0 -or $mask -lt 0 -or $mask -gt 127){$valid=$false}
                if($history -eq 1 -and ($submitted -ne 1 -or $previous -ne ($frame-1))){$valid=$false}
                if((($mask -band 32) -ne 0) -ne ($history -eq 1)){$valid=$false}
                if($ownershipFields){
                    $requested=0L;$active=0L;$fallback=[string]$row.ownership_fallback_reason
                    if(-not [long]::TryParse([string]$row.ownership_requested,[ref]$requested) -or $requested -notin @(0,1) -or
                        -not [long]::TryParse([string]$row.ownership_active,[ref]$active) -or $active -notin @(0,1)){$valid=$false}
                    if($requireOwnership -and $null -ne $ownershipControl -and $requested -ne $ownershipControl){$valid=$false}
                    if($requested -eq 1){$ownershipRequestedRows++}
                    if($active -eq 1){
                        $ownershipActiveRows++
                        if($requested -ne 1 -or $fallback -ne 'active'){$valid=$false}
                    }else{
                        if($requested -eq 0 -and $fallback -ne 'not_requested'){$valid=$false}
                        if($requested -eq 1 -and $fallback -notin @('stereo','no_temporal_input','unsupported_osg',
                            'unsupported_renderer','unsupported_visitor','unsupported_threading','multiple_contexts',
                            'unknown_sceneview','unsupported_fx_state','unsupported_fx_targets','owner_slots_exhausted')){$valid=$false}
                        if(-not $ownershipFallbacks.Contains($fallback)){$ownershipFallbacks[$fallback]=0}
                        $ownershipFallbacks[$fallback]++
                    }
                }
                if($submitted -eq 1){
                    foreach($column in @('render_w','render_h','output_w','output_h','color_ptr','depth_ptr','motion_ptr')){
                        $number=0L
                        if(-not [long]::TryParse([string]$row.$column,[ref]$number) -or $number -le 0){$valid=$false}
                    }
                    if(($mask -band 31) -ne 31){$valid=$false}
                } elseif(($mask -band 16) -ne 0){$valid=$false}
                if(-not $valid){$incoherent++}
            }
            if($incoherent){[void]$reasons.Add('Temporal rows have incoherent submission, history, inputs or extents')}
            if($unexpectedReady){[void]$reasons.Add('DLSS-ready became true before runtime and complete motion integration')}
        } catch {[void]$reasons.Add("Invalid temporal input telemetry: $_")}
        try {
            $capture=Read-Phase9CaptureCsv $caps @('context','gl_vulkan_bridge_candidate') -RequireWriterStatus:$requireWriter
            $capabilityRows=$capture.Rows.Count;$loss += $capture.Loss
            if($capabilityRows -eq 0){[void]$reasons.Add('Interop capability telemetry has no context row')}
            if($capture.Malformed){[void]$reasons.Add('Malformed interop capability rows')}
            foreach($row in $capture.Rows){
                $context=0L;$candidate=0L
                if(-not [long]::TryParse([string]$row.context,[ref]$context) -or $context -lt 0 -or
                    -not [long]::TryParse([string]$row.gl_vulkan_bridge_candidate,[ref]$candidate) -or $candidate -notin @(0,1)){
                    [void]$reasons.Add('Invalid interop capability context or flag')
                }
            }
        } catch {[void]$reasons.Add("Invalid GL/Vulkan capability telemetry: $_")}
        if($loss){[void]$reasons.Add('CSV writer loss makes the capture incomplete')}
    }
    $valid=if($TemporalRequested){$reasons.Count -eq 0}else{$null}
    if($TemporalRequested -and -not $valid){Write-Warning ('INVALID TEMPORAL CAPTURE: '+($reasons -join '; ')+'. Raw evidence will still be zipped.')}
    $ownershipValid=if($ownershipRequestedRows -gt 0){$valid -and $ownershipActiveRows -eq $ownershipRequestedRows}else{$null}
    $result=[pscustomobject]@{schema=3;temporal_requested=$TemporalRequested;valid_temporal_capture=$valid;
        effective_ownership_columns=$ownershipFields;ownership_requested_rows=$ownershipRequestedRows;
        ownership_active_rows=$ownershipActiveRows;ownership_fallbacks=$ownershipFallbacks;valid_ownership_capture=$ownershipValid;
        temporal_input_rows=$inputRows;interop_capability_rows=$capabilityRows;unexpected_dlss_ready_rows=$unexpectedReady;
        incoherent_rows=$incoherent;csv_writer_dropped=$loss;dense_dynamic_motion_expected=$false;
        ngx_evaluation_expected=$false;reasons=$reasons.ToArray()}
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'TEMPORAL-CAPTURE.json') -Encoding UTF8
    return $result
}
