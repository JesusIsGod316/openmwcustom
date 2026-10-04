param([string]$PythonPath)
$ErrorActionPreference = 'Stop'
$wrapperSavedPython = $PythonPath
$ProfileDir = 'caller-profile-value-must-survive-dot-source'
. (Join-Path $PSScriptRoot 'OptimizedMW_AssetEngagement.ps1')
if($PythonPath -ne $wrapperSavedPython -or $ProfileDir -ne 'caller-profile-value-must-survive-dot-source') {
    throw 'Dot-sourcing verifier wrapper overwrote caller launcher variables.'
}
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('p9-engagement-wrapper-' + [Guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $testRoot
try {
    $sentinel = Join-Path $testRoot 'user-capture.bin'
    [IO.File]::WriteAllBytes($sentinel, [byte[]](1,2,3,4,8,16))
    $before = (Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash
    $result = Test-Phase9AssetEngagement -ProfileDir $testRoot -PythonPath (Join-Path $testRoot 'python-missing.exe')
    if($result.status -ne 'VERIFIER_UNAVAILABLE' -or $result.exit_code -ne 3 -or $result.qualified) {
        throw 'Missing Python incorrectly qualified candidate engagement.'
    }
    $receipt = Get-Content -Raw -LiteralPath $result.report_json | ConvertFrom-Json
    if($receipt.actually_engaged -or $receipt.performance_acceptance -ne 'NOT_ASSESSED') {
        throw 'Missing Python generated an engagement/performance claim.'
    }
    if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $before) {
        throw 'Missing Python altered the retained capture.'
    }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'OptimizedMW_AssetEngagement.ps1') `
        -ProfileDir $testRoot -PythonPath (Join-Path $testRoot 'python-missing.exe')
    if($LASTEXITCODE -ne 0) {
        throw 'Reportable unavailable outcome escaped as a capture-process failure.'
    }
    $slowPython = Join-Path $testRoot 'slow interpreter.exe'
    Add-Type -TypeDefinition @'
using System;
using System.Threading;
public class SlowAssetInterpreter {
    public static void Main(string[] args) {
        Console.WriteLine("hermetic slow interpreter entered"); Console.Out.Flush();
        Console.Error.WriteLine("hermetic stderr entered"); Console.Error.Flush();
        Thread.Sleep(120000);
    }
}
'@ -OutputAssembly $slowPython -OutputType ConsoleApplication
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $timeout = Test-Phase9AssetEngagement -ProfileDir $testRoot -PythonPath $slowPython -TimeoutSeconds 1
    $timer.Stop()
    if($timeout.status -ne 'VERIFIER_UNAVAILABLE' -or $timeout.qualified -or $timer.Elapsed.TotalSeconds -gt 6) {
        throw 'Slow interpreter escaped the qualification timeout bound.'
    }
    $receipt = Get-Content -Raw -LiteralPath $timeout.report_json | ConvertFrom-Json
    if($receipt.problems[0] -notmatch 'timed out' -or ($receipt.verifier_output -join '') -notmatch 'hermetic slow interpreter entered') {
        throw 'Slow interpreter did not retain explicit timeout/drained output proof.'
    }
    if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $before) {
        throw 'Interpreter timeout altered the raw capture.'
    }
    $silentPython = Join-Path $testRoot 'silent interpreter.exe'
    Add-Type -TypeDefinition 'public class SilentAssetInterpreter { public static void Main(string[] args) {} }' `
        -OutputAssembly $silentPython -OutputType ConsoleApplication
    [IO.File]::WriteAllText((Join-Path $testRoot 'ASSET-ENGAGEMENT.json'), '{"schema":1,"exit_code":0,"actually_engaged":true,"status":"ENGAGEMENT_QUALIFIED","run_token":"stale"}')
    $silent = Test-Phase9AssetEngagement -ProfileDir $testRoot -PythonPath $silentPython -TimeoutSeconds 1
    if($silent.qualified -or $silent.status -ne 'VERIFIER_UNAVAILABLE') {
        throw 'Silent interpreter reused a stale positive report.'
    }
    if($PythonPath) {
        # A running verifier with missing game evidence must still preserve files
        # and write an unavailable/incomplete report instead of losing the archive.
        $result = Test-Phase9AssetEngagement -ProfileDir $testRoot -PythonPath $PythonPath
        if($result.status -ne 'INCOMPLETE_EVIDENCE' -or $result.exit_code -ne 3 -or $result.qualified) {
            throw 'Missing manifest/state proof incorrectly qualified candidate engagement.'
        }
        if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $before) {
            throw 'Missing proof altered the retained capture.'
        }
    }
    Write-Host 'Asset engagement wrapper: missing interpreter/proof keeps capture and cannot claim engagement PASS'
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($testRoot)
    $resolvedTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if($resolvedRoot.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase) -and
       [IO.Path]::GetFileName($resolvedRoot).StartsWith('p9-engagement-wrapper-')) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}
