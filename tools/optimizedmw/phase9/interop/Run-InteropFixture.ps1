param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP 'optimizedmw-phase9-interop-evidence'),
    [ValidateRange(1,64)][int]$Frames = 4,
    [ValidateRange(1,10000)][int]$GpuTimeoutMs = 1500,
    [ValidateRange(1,60)][int]$ProcessTimeoutSeconds = 30,
    [switch]$RequireSupport
)
$ErrorActionPreference = 'Stop'
$native = (Resolve-Path -LiteralPath $Executable).ProviderPath
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$destination = (Resolve-Path -LiteralPath $OutputDirectory).ProviderPath
$evidence = Join-Path $destination 'interop-evidence.jsonl'
$stdout = Join-Path $destination 'interop-stdout.jsonl'
$stderr = Join-Path $destination 'interop-stderr.txt'
$run = [ordered]@{
    schema=1; executable=$native
    executable_sha256=(Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash.ToLowerInvariant()
    frames_per_generation=$Frames; gpu_timeout_ms=$GpuTimeoutMs
    process_timeout_seconds=$ProcessTimeoutSeconds; started_utc=[DateTime]::UtcNow.ToString('o')
    require_support=[bool]$RequireSupport; exit_code=$null; result='running'
}
$arguments = @('--output',('"' + $evidence + '"'),'--frames',"$Frames",'--timeout-ms',"$GpuTimeoutMs")
$process = Start-Process -FilePath $native -ArgumentList $arguments -WindowStyle Hidden -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
# PowerShell 5.1 can lose ExitCode when the Process handle is first acquired
# after its WaitForExit path closes it. Hold it before waiting on the child.
$null = $process.Handle
if (-not $process.WaitForExit($ProcessTimeoutSeconds * 1000)) {
    Stop-Process -Id $process.Id -Force
    $run.result='failed_process_deadline'
    $run.exit_code=1
    [ordered]@{ schema=1; status='fail'; check='process_deadline'; detail='Standalone driver fixture exceeded its outer deadline' } |
        ConvertTo-Json -Compress | Add-Content -LiteralPath $evidence -Encoding UTF8
} else {
    if ($null -eq $process.ExitCode) { throw 'Native process exit code unavailable; cannot validate fixture result' }
    $run.exit_code=$process.ExitCode
    if ($process.ExitCode -eq 0) { $run.result='passed_pixels_and_protocol' }
    elseif ($process.ExitCode -eq 77) { $run.result='unsupported_skipped' }
    else { $run.result='failed' }
}
$run.completed_utc=[DateTime]::UtcNow.ToString('o')
$run | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'interop-run.json') -Encoding UTF8
Get-Content -LiteralPath $evidence
Write-Output "Fixture result: $($run.result); evidence: $destination"
if ($run.exit_code -eq 77 -and $RequireSupport) { throw 'Required same-device interop support unavailable; a skipped fixture does not satisfy the hardware gate' }
exit $run.exit_code
