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
function New-AssetWrapperInterpreter {
    param([string]$Name, [string]$Source)
    $sourcePath = Join-Path $testRoot ($Name + '.cs')
    $outputPath = Join-Path $testRoot ($Name + '.exe')
    [IO.File]::WriteAllText($sourcePath, $Source)
    if($PSVersionTable.PSVersion.Major -le 5) {
        Add-Type -Path $sourcePath -OutputAssembly $outputPath -OutputType ConsoleApplication
    } else {
        # .NET Core cannot emit a runnable ConsoleApplication with Add-Type.
        # Build these two hermetic Windows fixtures with the installed PS5.1.
        $compiler = Join-Path $testRoot 'compile-interpreter.ps1'
        [IO.File]::WriteAllText($compiler, 'param($SourcePath,$OutputPath); $ErrorActionPreference="Stop"; Add-Type -Path $SourcePath -OutputAssembly $OutputPath -OutputType ConsoleApplication')
        & (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') -NoProfile -ExecutionPolicy Bypass `
            -File $compiler -SourcePath $sourcePath -OutputPath $outputPath
        if($LASTEXITCODE -ne 0) { throw 'Hermetic interpreter compiler failed.' }
    }
    return $outputPath
}
function Assert-AssetWrapperIncomplete {
    param($Result, [string]$CaseName)
    if($Result.status -ne 'INCOMPLETE_EVIDENCE' -or $Result.exit_code -ne 3 -or $Result.qualified) {
        throw ($CaseName + ' did not run the real verifier to its expected missing-evidence result.')
    }
    $receipt = [IO.File]::ReadAllText($Result.report_json) | ConvertFrom-Json
    if($receipt.status -ne 'INCOMPLETE_EVIDENCE' -or $receipt.exit_code -ne 3 -or $receipt.actually_engaged -or
       -not $receipt.run_token -or $receipt.run_token -eq $script:previousDiscoveryToken) {
        throw ($CaseName + ' reused stale proof or claimed engagement.')
    }
    $script:previousDiscoveryToken = $receipt.run_token
    if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $before) {
        throw ($CaseName + ' altered the retained capture.')
    }
    Write-Host ('PASS automatic interpreter: ' + $CaseName + '; real verifier, fresh report, raw capture preserved')
}
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
    $slowPython = New-AssetWrapperInterpreter -Name 'slow interpreter' -Source @'
using System;
using System.Threading;
public class SlowAssetInterpreter {
    public static void Main(string[] args) {
        Console.WriteLine("hermetic slow interpreter entered"); Console.Out.Flush();
        Console.Error.WriteLine("hermetic stderr entered"); Console.Error.Flush();
        Thread.Sleep(120000);
    }
}
'@
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
    $silentPython = New-AssetWrapperInterpreter -Name 'silent interpreter' `
        -Source 'public class SilentAssetInterpreter { public static void Main(string[] args) {} }'
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
    $realPython = $PythonPath
    if(-not $realPython) {
        $pythonCommand = Microsoft.PowerShell.Core\Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        if($pythonCommand -and $pythonCommand.Source -notmatch '[\\/]Microsoft[\\/]WindowsApps[\\/]') { $realPython = $pythonCommand.Source }
    }
    if($realPython) {
        Assert-AssetWrapperIncomplete (Test-Phase9AssetEngagement -ProfileDir $testRoot) 'real automatic discovery'
        $launcherCommand = Microsoft.PowerShell.Core\Get-Command py -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        $realLauncher = $null
        if($launcherCommand -and [IO.File]::Exists($launcherCommand.Source) -and
           [Diagnostics.FileVersionInfo]::GetVersionInfo($launcherCommand.Source).OriginalFilename -eq 'py.exe') {
            $realLauncher = $launcherCommand.Source
        }
        $script:mockPythonDiscovery = @{py=$realPython}
        function Get-Command {
            param($Name, $CommandType, $ErrorAction)
            if($script:mockPythonDiscovery.ContainsKey([string]$Name)) {
                [pscustomobject]@{Source=$script:mockPythonDiscovery[[string]$Name]}
            }
        }
        try {
            Assert-AssetWrapperIncomplete (Test-Phase9AssetEngagement -ProfileDir $testRoot) 'py resolves directly to Python'
            $script:mockPythonDiscovery = @{py=$silentPython; python=$realPython}
            Assert-AssetWrapperIncomplete (Test-Phase9AssetEngagement -ProfileDir $testRoot) 'invalid probe falls through to Python'
            if($realLauncher) {
                $script:mockPythonDiscovery = @{py=$realLauncher}
                Assert-AssetWrapperIncomplete (Test-Phase9AssetEngagement -ProfileDir $testRoot) 'genuine py launcher'
                $priorDefault = [Environment]::GetEnvironmentVariable('PY_PYTHON','Process')
                try {
                    # Force the launcher to need its explicit Python 3 selector,
                    # regardless of whether Python 2 is installed on this host.
                    $env:PY_PYTHON='2'
                    Assert-AssetWrapperIncomplete (Test-Phase9AssetEngagement -ProfileDir $testRoot) 'launcher Python 3 selector fallback'
                } finally { [Environment]::SetEnvironmentVariable('PY_PYTHON',$priorDefault,'Process') }
            }
            $script:mockPythonDiscovery = @{py=$slowPython; python=$realPython}
            $timer = [Diagnostics.Stopwatch]::StartNew()
            $discoveryTimeout = Test-Phase9AssetEngagement -ProfileDir $testRoot -TimeoutSeconds 1
            $timer.Stop()
            if($discoveryTimeout.status -ne 'VERIFIER_UNAVAILABLE' -or $discoveryTimeout.qualified -or $timer.Elapsed.TotalSeconds -gt 6) {
                throw 'Automatic discovery escaped the overall interpreter timeout.'
            }
            if((Get-FileHash -LiteralPath $sentinel -Algorithm SHA256).Hash -ne $before) { throw 'Discovery timeout changed the retained capture.' }
            Write-Host 'PASS automatic interpreter: shared timeout bound preserves raw capture'
        } finally { Remove-Item -LiteralPath 'Function:Get-Command' }
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
