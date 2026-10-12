param(
    [Alias('ProfileDir')][string]$AssetProfileDir,
    [Alias('PythonPath')][string]$AssetPythonPath,
    [Alias('ExpectedSource')][string]$AssetExpectedSource,
    [Alias('ExpectedExe')][string]$AssetExpectedExe,
    [Alias('TimeoutSeconds')][ValidateRange(1,45)][int]$AssetTimeoutSeconds=45
)
# Dot-source this file and call after capture/report completion, before archiving.
# Never throws a qualification failure into the capture path or launches a game.
function Test-Phase9AssetEngagement {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][string]$ProfileDir,
        [string]$PythonPath,
        [string]$ExpectedSource,
        [string]$ExpectedExe,
        [ValidateRange(1,45)][int]$TimeoutSeconds=45
    )
    $reportJson = Join-Path $ProfileDir 'ASSET-ENGAGEMENT.json'
    $reportText = Join-Path $ProfileDir 'ASSET-ENGAGEMENT.txt'
    $verifier = Join-Path $PSScriptRoot 'verify-asset-engagement.py'
    $pythonExe = $null
    $reason = $null
    $verifierOutput = @()
    $verifierCode = $null
    $runToken = [Guid]::NewGuid().ToString('N')
    $interpreterClock = [Diagnostics.Stopwatch]::StartNew()
    $invokeInterpreter = {
        param([string]$Executable, [string[]]$Arguments, [int]$WaitMilliseconds, [string]$Selector)
        # .NET Framework (PowerShell 5.1) lacks ArgumentList. Quote ordinary
        # Windows argv, but py.exe parses its version selector before argv and
        # forwards a quoted "-3" to Python. Only this fixed selector is unquoted.
        if($Selector -and $Selector -cne '-3') { throw 'Unsupported interpreter selector.' }
        $quoted = foreach($argument in $Arguments) {
            $escaped = [regex]::Replace([string]$argument, '(\\*)"', {
                param($match) $match.Groups[1].Value + $match.Groups[1].Value + '\"'
            })
            $escaped = [regex]::Replace($escaped, '(\\+)$', '$1$1')
            '"' + $escaped + '"'
        }
        $argumentText = $quoted -join ' '
        if($Selector) { $argumentText = $Selector + ' ' + $argumentText }
        $start = New-Object System.Diagnostics.ProcessStartInfo
        $start.FileName = $Executable
        $start.Arguments = $argumentText
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $process = New-Object System.Diagnostics.Process
        $process.StartInfo = $start
        $code = $null
        $timedOut = $false
        $output = @()
        try {
            if(-not $process.Start()) { throw 'Interpreter process did not start.' }
            $stdoutTask = $process.StandardOutput.ReadToEndAsync()
            $stderrTask = $process.StandardError.ReadToEndAsync()
            if(-not $process.WaitForExit([Math]::Max(1, $WaitMilliseconds))) {
                $timedOut = $true
                try {
                    $treeKill = $process.GetType().GetMethods() | Where-Object {
                        $_.Name -eq 'Kill' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType -eq [bool]
                    } | Select-Object -First 1
                    if($treeKill) { $process.Kill($true) } else { $process.Kill() }
                } catch { }
                $null = $process.WaitForExit(1000)
            } else { $code = $process.ExitCode }
            # Never block capture completion on inherited/stalled pipe handles.
            $drained = $stdoutTask.Wait(1000) -and $stderrTask.Wait(1000)
            if($drained) { $output = @([string]$stdoutTask.Result, [string]$stderrTask.Result) }
            return [pscustomobject]@{code=$code; timed_out=$timedOut; drained=$drained; output=$output}
        } finally { $process.Dispose() }
    }
    try {
        if(-not [IO.Directory]::Exists($ProfileDir)) {
            throw 'Profile directory is absent; raw capture paths were not changed.'
        }
        if(-not [IO.File]::Exists($verifier)) {
            $reason = 'The packaged asset verifier is missing. Raw capture is preserved; requested switches are not actual engagement.'
        } elseif($PythonPath) {
            if([IO.File]::Exists($PythonPath)) { $pythonExe = $PythonPath }
            else { $reason = 'The requested Python executable is unavailable. Raw capture is preserved; qualification is unavailable.' }
        } else {
            $probeCode = 'import json,sys; print("P9_ASSET_PYTHON="+json.dumps({"major":sys.version_info[0],"executable":sys.executable}))'
            $seenExecutables = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
            foreach($candidate in @('py','python','python3')) {
                $command = Get-Command $candidate -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
                if(-not $command -or $command.Source -match '[\\/]Microsoft[\\/]WindowsApps[\\/]' -or
                   -not $seenExecutables.Add([string]$command.Source)) { continue }
                # A query named py may resolve to Python itself. Probe capability
                # first and use its actual interpreter, never infer flags from
                # that query name. A launcher defaulting to Python 2 gets one
                # explicit Python 3 probe before trying the next executable.
                $selectors = @('')
                if($candidate -eq 'py') { $selectors += '-3' }
                foreach($selector in $selectors) {
                    $remaining = $TimeoutSeconds * 1000 - [int]$interpreterClock.ElapsedMilliseconds
                    if($remaining -le 0) { break }
                    try {
                        $probe = & $invokeInterpreter -Executable $command.Source -Arguments @('-B','-c',$probeCode) `
                            -WaitMilliseconds ([Math]::Min(3000,$remaining)) -Selector $selector
                    } catch { continue }
                    if($probe.code -ne 0 -or $probe.timed_out -or -not $probe.drained) { continue }
                    $proof = @(([string]$probe.output[0] -split '\r?\n') | Where-Object {$_.StartsWith('P9_ASSET_PYTHON=')})
                    if($proof.Count -ne 1) { continue }
                    try { $identity = $proof[0].Substring('P9_ASSET_PYTHON='.Length) | ConvertFrom-Json } catch { continue }
                    $resolved = [string]$identity.executable
                    if($identity.major -eq 3 -and [IO.Path]::IsPathRooted($resolved) -and [IO.File]::Exists($resolved) -and
                       $resolved -notmatch '[\\/]Microsoft[\\/]WindowsApps[\\/]') {
                        $pythonExe = [IO.Path]::GetFullPath($resolved)
                        break
                    }
                }
                if($pythonExe) { break }
            }
            if(-not $pythonExe) {
                $reason = 'A working Python 3 interpreter could not be discovered within the timeout. Raw capture is preserved; run the packaged verifier later to qualify actual engagement.'
            }
        }
        if(-not $reason) {
            $arguments = @('-B', $verifier, $ProfileDir, '--output-dir', $ProfileDir, '--run-token', $runToken)
            if($ExpectedSource) { $arguments += @('--expected-source', $ExpectedSource) }
            if($ExpectedExe) { $arguments += @('--expected-exe', $ExpectedExe) }
            $remaining = $TimeoutSeconds * 1000 - [int]$interpreterClock.ElapsedMilliseconds
            if($remaining -le 0) {
                $reason = 'Asset verifier timed out after ' + $TimeoutSeconds + ' seconds. Raw capture is preserved; qualification is unavailable.'
            } else {
                $execution = & $invokeInterpreter -Executable $pythonExe -Arguments $arguments -WaitMilliseconds $remaining
                $verifierCode = $execution.code
                $verifierOutput = @($execution.output)
                if($execution.timed_out) {
                    $reason = 'Asset verifier timed out after ' + $TimeoutSeconds + ' seconds. Raw capture is preserved; qualification is unavailable.'
                } elseif(-not $execution.drained) {
                    $reason = 'Asset verifier output could not be drained within its bound. Raw capture is preserved; qualification is unavailable.'
                }
            }
            if($reason) { $verifierCode = $null }
            if($verifierCode -notin @(0,2,3,4) -or -not [IO.File]::Exists($reportJson) -or -not [IO.File]::Exists($reportText)) {
                if(-not $reason) { $reason = 'The asset verifier did not produce a complete report. Raw capture is preserved; qualification is unavailable.' }
            } else {
                $report = [IO.File]::ReadAllText($reportJson) | ConvertFrom-Json
                if($report.schema -ne 1 -or $report.exit_code -ne $verifierCode -or $report.run_token -cne $runToken) {
                    $reason = 'The verifier report/exit status is inconsistent. Raw capture is preserved; qualification is unavailable.'
                } else {
                    Write-Host ('Asset engagement: ' + $report.status + ' (code ' + $verifierCode + '). Report: ' + $reportText)
                    return [pscustomobject]@{
                        status=[string]$report.status; exit_code=[int]$verifierCode;
                        qualified=($verifierCode -eq 0); actually_engaged=[bool]$report.actually_engaged;
                        report_json=$reportJson; report_text=$reportText; raw_capture_preserved=$true;
                        note='Engagement only; fixture, image-quality, stability and performance gates remain separate.'
                    }
                }
            }
        }
    } catch {
        $reason = 'Asset engagement qualification is unavailable: ' + [string]$_ + ' Raw capture is preserved.'
    }
    $unavailable = [ordered]@{
        schema=1; status='VERIFIER_UNAVAILABLE'; exit_code=3; actually_engaged=$false;
        performance_acceptance='NOT_ASSESSED'; visual_acceptance='NOT_ASSESSED';
        fixture_acceptance='SEPARATE_RELEASE_GATE'; capture=$ProfileDir;
        problems=@($reason); verifier_exit_code=$verifierCode; verifier_output=@($verifierOutput);
        raw_capture_preserved=$true; run_token=$runToken
    }
    try {
        # Write only the two derived reports. Original files and ZIPs stay intact.
        $utf8 = New-Object System.Text.UTF8Encoding($false)
        [IO.File]::WriteAllText($reportJson, ($unavailable | ConvertTo-Json -Depth 8), $utf8)
        [IO.File]::WriteAllText($reportText, ('PHASE 9 ASSET ENGAGEMENT: VERIFIER_UNAVAILABLE' + [Environment]::NewLine +
            $reason + [Environment]::NewLine + 'Missing proof never becomes actual engagement. Keep this capture; do not run a broad benchmark matrix.' +
            [Environment]::NewLine), $utf8)
    } catch {
        Write-Host ('Asset engagement report could not be saved: ' + [string]$_ + '. Raw capture remains at ' + $ProfileDir)
    }
    Write-Host ('Asset engagement: VERIFIER_UNAVAILABLE (code 3). ' + $reason)
    return [pscustomobject]@{
        status='VERIFIER_UNAVAILABLE'; exit_code=3; qualified=$false; actually_engaged=$false;
        report_json=$reportJson; report_text=$reportText; raw_capture_preserved=$true; note=$reason
    }
}

if($MyInvocation.InvocationName -ne '.') {
    if(-not $AssetProfileDir) {
        Write-Host 'Asset engagement: ProfileDir is required. No capture files were changed.'
        exit 1
    }
    $null = Test-Phase9AssetEngagement -ProfileDir $AssetProfileDir -PythonPath $AssetPythonPath -ExpectedSource $AssetExpectedSource -ExpectedExe $AssetExpectedExe -TimeoutSeconds $AssetTimeoutSeconds
    # Qualification outcomes are retained in the JSON. They must not interrupt
    # the existing capture/ZIP completion path or masquerade as game exit codes.
    exit 0
}
