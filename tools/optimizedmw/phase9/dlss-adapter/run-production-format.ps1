param(
    [Parameter(Mandatory=$true)][string]$BinaryPin,
    [Parameter(Mandatory=$true)][string]$BinaryPath,
    [switch]$NegativeControl,
    [string]$ValidationDirectory='C:\OptimizedMW_Test\Validation\ImplementationBatch_20261010\OwnershipRepair\BuildC_20261011\DlssFormatImport'
)
$ErrorActionPreference='Stop'
$worktree=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\..')).Path
$coordinationRoot='C:\OptimizedMW_Test\Validation\ImplementationBatch_20261010\OwnershipRepair'
$mutex=New-Object System.Threading.Mutex($false,'Local\OptimizedMW-Test-Profile')
$held=$false
$priorPath=$env:PATH
function Get-TaskHash([string]$path){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Assert-TaskInputs($pin){
    if((Get-TaskHash $pin.compiled_inputs) -ne $pin.compiled_inputs_sha256){throw 'Compiled input receipt changed'}
    foreach($file in @($pin.source_files)+@($pin.binary_files)){
        if((Get-TaskHash $file.path) -ne $file.sha256){throw ('Compiled source/binary changed: '+$file.path)}
    }
}
try{
    $held=$mutex.WaitOne(0)
    if(!$held){throw 'Benchmark profile occupied; defer format fixture'}
    $checkpoint=Get-Content -LiteralPath (Join-Path $coordinationRoot 'OVERNIGHT-CHECKPOINT.json') -Raw|ConvertFrom-Json
    if($checkpoint.runtime_reservation.launch_policy -notlike 'Released*'){throw 'Human game reservation; defer native fixture'}
    if(Test-Path -LiteralPath (Join-Path $coordinationRoot 'OVERNIGHT-HOLD.txt')){throw 'Human runtime hold; defer native fixture'}
    $occupied=@(Get-CimInstance Win32_Process|Where-Object {
        $_.Name -match '^(openmw|morrowind|cl|link|ninja|cmake|ctest|MSBuild)\.exe$' -or $_.Name -match '^p9-.*\.exe$'
    })
    if($occupied.Count){throw 'Game, build or native fixture occupied; defer actual-format proof'}
    $pinPath=(Resolve-Path -LiteralPath $BinaryPin).Path
    $pin=Get-Content -LiteralPath $pinPath -Raw|ConvertFrom-Json
    if($pin.baseline_commit -ne '403cc61199979766c6d73c50f9bfece93ec8024e'){throw 'Unexpected audited adapter ancestry'}
    & git -C $worktree merge-base --is-ancestor $pin.baseline_commit HEAD
    if($LASTEXITCODE -ne 0){throw 'Source does not descend from audited adapter baseline'}
    Assert-TaskInputs $pin
    $native=(Resolve-Path -LiteralPath $BinaryPath).Path
    if((Split-Path -Leaf $native) -ne 'p9-dlss-production-format.exe'){throw 'Wrong independent fixture executable'}
    $nativePin=@($pin.binary_files|Where-Object {$_.path -eq $native})
    if($nativePin.Count -ne 1 -or (Get-TaskHash $native) -ne $nativePin[0].sha256){throw 'Native format executable absent from pin'}
    $required=@('production-format-tests.cpp','source-textures.hpp','borrowed-gl-state.hpp','gpu-transfers.hpp',
        'shared-d3d11-textures.hpp','synchronousinputlease.hpp','CMakeLists.txt')
    foreach($name in $required){
        if(@($pin.source_files|Where-Object {(Split-Path -Leaf $_.path) -eq $name}).Count -ne 1){throw ('Missing unique source dependency: '+$name)}
    }
    $runtime=@($pin.binary_files|Where-Object {(Split-Path -Leaf $_.path) -in @('osg.dll','OpenThreads.dll')})
    if($runtime.Count -ne 2 -or @($runtime|Where-Object {(Split-Path -Leaf $_.path) -eq 'osg.dll'}).Count -ne 1){throw 'Exact pinned OSG/OpenThreads runtime pair missing'}
    $runtimeFolders=@($runtime|ForEach-Object {Split-Path -Parent $_.path}|Select-Object -Unique)
    $env:PATH=($runtimeFolders -join ';')+';'+$priorPath
    $stamp=[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
    $output=Join-Path $ValidationDirectory ('Run_'+$stamp)
    if(Test-Path -LiteralPath $output){throw 'Preserve existing native evidence'}
    New-Item -ItemType Directory -Path $output|Out-Null
    $argumentFile=Join-Path $output 'arguments.json'
    $nativeArguments=@('--output',(Join-Path $output 'format-evidence.jsonl'))
    if($NegativeControl){$nativeArguments+='--negative-corrupt-copy'}
    $nativeArguments|ConvertTo-Json|Set-Content -LiteralPath $argumentFile -Encoding UTF8
    $desktopRunner=Join-Path $worktree 'tools\optimizedmw\phase9\benchmark_desktop.py'
    $identity=[ordered]@{
        schema=1;fixture_path=$native;fixture_sha256=(Get-TaskHash $native);osg_runtime_files=$runtime;
        production_format_fixture=$true;source_worktree=$worktree;source_commit=(& git -C $worktree rev-parse HEAD).Trim();
        game_package_changed=$false;profile_mutex_held=$true;started_utc=[DateTime]::UtcNow.ToString('o');
        negative_control=[bool]$NegativeControl;source_files=$pin.source_files;
        compiled_binary_pin=$pinPath;compiled_binary_pin_sha256=(Get-TaskHash $pinPath);
        host_files=@([ordered]@{path=$PSCommandPath;sha256=(Get-TaskHash $PSCommandPath)},
            [ordered]@{path=$desktopRunner;sha256=(Get-TaskHash $desktopRunner)})
    }
    $identity|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $output 'INPUTS.json') -Encoding UTF8
    $py='C:\Users\LSCha\AppData\Local\Programs\Python\Python313\python.exe'
    & $py $desktopRunner --executable $native --arguments-file $argumentFile --cwd (Split-Path -Parent $native) --output (Join-Path $output 'Desktop') --timeout 120 *> (Join-Path $output 'host.log')
    $code=$LASTEXITCODE
    $receipt=Get-Content -LiteralPath (Join-Path $output 'Desktop\DESKTOP-RECEIPT.json') -Raw|ConvertFrom-Json
    $nativeOutput=Get-Content -LiteralPath (Join-Path $output 'Desktop\stdout.log') -Raw
    if($code -eq 0){
        $marker=if($NegativeControl){'"check":"production_format_negative","status":"pass"'}else{'"check":"production_format_complete","status":"pass"'}
        if($nativeOutput -notlike ('*'+$marker+'*')){throw 'Native exit lacked actual-format/corruption proof marker'}
    }
    if(!$receipt.input_desktop_unchanged -or $receipt.desktop_switched -or @($receipt.input_desktop_owned_visible_windows).Count){throw 'Native fixture desktop isolation failed'}
    if($receipt.exit_code -ne $code){throw 'Native runner/child exit disagreement'}
    Assert-TaskInputs $pin
    foreach($hostInput in $identity.host_files){
        if((Get-TaskHash $hostInput.path) -ne $hostInput.sha256){throw 'Native host controls changed during proof'}
    }
    Get-Content -LiteralPath (Join-Path $output 'Desktop\stdout.log') -Tail 3
    Get-Content -LiteralPath (Join-Path $output 'Desktop\stderr.log') -Tail 4
    Write-Output ('Actual-format native exit '+$code+'; evidence='+$output)
    exit $code
}finally{
    $env:PATH=$priorPath
    if($held){$mutex.ReleaseMutex()}
    $mutex.Dispose()
}
