# Hermetic PS5.1/PS7 contract checks. Never invokes OpenMW, WPR, or a UI.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')
foreach($name in @('OptimizedMW_FrozenConfig.ps1','OptimizedMW_Test.ps1','frozen-config-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
$taskTempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$fixture=Join-Path $taskTempRoot ('p9 frozen [space] '+[Guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game';$live=Join-Path $fixture 'gameplay';$baseline=Join-Path $fixture 'locked'
$checks=0
function Assert-FrozenCheck {
    param([bool]$Condition,[string]$Reason)
    if(-not $Condition){throw $Reason};$script:checks++
}
function Write-FixtureLock {
    $files=@()
    foreach($name in @('openmw.cfg','settings.cfg','Optimizations.cfg','saves/Character/save.omwsave','SAINTJS-ASSETS.json',
        'global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml')){
        $id=Get-OptimizedMWFrozenFile (Join-Path $baseline $name)
        $files+=[pscustomobject]@{relative_path=$name;sha256=$id.sha256;bytes=$id.bytes}
    }
    $lock=[ordered]@{schema=1;files=$files;source_configuration_directory=$live;source_user_data_directory=$live}
    [IO.File]::WriteAllText((Join-Path $baseline 'LOCK.json'),($lock | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
function Assert-FrozenReject {
    param([scriptblock]$Action,[string]$Reason)
    $rejected=$false;try{$null=& $Action}catch{$rejected=$true}
    Assert-FrozenCheck $rejected $Reason
}
[void][IO.Directory]::CreateDirectory($game)
[void][IO.Directory]::CreateDirectory((Join-Path $game 'resources'))
[void][IO.Directory]::CreateDirectory((Join-Path $game 'resources/vfs-mw'))
[void][IO.Directory]::CreateDirectory($live)
[void][IO.Directory]::CreateDirectory((Join-Path $baseline 'saves/Character'))
try{
    $ordinary=Join-Path $live 'assets original';$other=Join-Path $live 'more'
    $saint=Join-Path $live 'saintjs/Core & Extra';$local=Join-Path $live 'local custom'
    foreach($directory in @($ordinary,$other,$saint,$local)){[void][IO.Directory]::CreateDirectory($directory)}
    $payload=Join-Path $saint 'model.nif';[IO.File]::WriteAllBytes($payload,[byte[]]@(11,12,13))
    $assetId=Get-OptimizedMWFrozenFile $payload
    [IO.File]::WriteAllText((Join-Path $baseline 'SAINTJS-ASSETS.json'),([ordered]@{schema=1;ordered_data_paths=@($saint);
        files=@([ordered]@{path=$payload;data_directory=$saint;relative_path='model.nif';bytes=$assetId.bytes;sha256=$assetId.sha256})} | ConvertTo-Json -Depth 6))
    $raw=@('config="ignored"','data="assets original"','data="?userdata?more"',
        ('data='+(ConvertTo-OptimizedMWConfigPath $saint)),'data="assets original"','data-local="?userconfig?local custom"',
        'content=Morrowind.esm','content=Fixture.esp','groundcover=FixtureGC.esp','fallback-archive=Morrowind.bsa',
        'fallback=Map_World_Red,202','replace=content','user-data="ignored"','resources="ignored"',
        'load-savegame="ignored"','script-run="ignored"','start=Somewhere','skip-menu=true','new-game=true') -join "`r`n"
    [IO.File]::WriteAllText((Join-Path $baseline 'openmw.cfg'),$raw)
    [IO.File]::WriteAllText((Join-Path $live 'openmw.cfg'),'# independent gameplay config')
    [IO.File]::WriteAllText((Join-Path $baseline 'settings.cfg'),"[Video]`r`nresolution x = 1920`r`nresolution y = 1080`r`n")
    [IO.File]::WriteAllText((Join-Path $baseline 'Optimizations.cfg'),"[Optimizations]`r`nrender threading mode = 0`r`n")
    [IO.File]::WriteAllBytes((Join-Path $baseline 'saves/Character/save.omwsave'),[byte[]]@(1,2,3))
    foreach($file in @('global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml')){[IO.File]::WriteAllBytes((Join-Path $baseline $file),[byte[]]@(4,5,6))}
    Write-FixtureLock
    $locked=Read-OptimizedMWFrozenLock $baseline
    Assert-FrozenCheck ($locked.files.Count -eq 9) 'Locked baseline inventory incomplete'
    $runtime=New-OptimizedMWFrozenRuntime -Directory $baseline -GameDir $game -VerifyAssetHashes
    $cfg=[IO.File]::ReadAllText($runtime.config_path)
    Assert-FrozenCheck ($runtime.directory.StartsWith((Join-Path $game 'TestSessions')+'\')) 'Runtime escaped installed TestSessions'
    Assert-FrozenCheck ($runtime.ordered_data_paths.Count -eq 6 -and $runtime.ordered_data_paths[0] -ceq (Join-Path $game 'resources/vfs-mw') -and
        $runtime.ordered_data_paths[1] -ceq $ordinary -and $runtime.ordered_data_paths[2] -ceq $other -and $runtime.ordered_data_paths[3] -ceq $saint -and
        $runtime.ordered_data_paths[4] -ceq $ordinary -and $runtime.ordered_data_paths[5] -ceq $local) 'Data order/duplicates/token or package/local-data priority changed'
    Assert-FrozenCheck ($cfg -notmatch '(?m)^config=|^script-run=|^load-savegame=|^start=|^skip-menu=|^new-game=' -and
        $cfg -match '(?m)^replace=config' -and $cfg -match '(?m)^replace=content' -and $cfg -match '(?m)^replace=data') 'Inherited config/startup options not removed or composition not reset'
    Assert-FrozenCheck ($cfg -notmatch '(?m)^replace=fallback\s*$' -and $cfg -match '(?m)^fallback=Map_World_Red,202') 'Package fallback defaults were disabled'
    Assert-FrozenCheck (@($cfg -split "`n" | Where-Object {$_ -match '^content='}).Count -eq 2) 'Content load order changed'
    Assert-FrozenCheck ($cfg.Contains('data-local='+(ConvertTo-OptimizedMWConfigPath (Join-Path $runtime.user_data_directory 'data'))) -and
        $cfg.Contains('user-data='+(ConvertTo-OptimizedMWConfigPath $runtime.user_data_directory))) 'Writes not directed to private local/user data'
    Assert-FrozenCheck ((Get-OptimizedMWFrozenFile $runtime.settings_path).sha256 -ceq (Get-OptimizedMWFrozenFile (Join-Path $baseline 'settings.cfg')).sha256) 'Captured display/settings rewritten during preparation'
    Assert-FrozenCheck ($runtime.save_path -cne $locked.save.path -and (Get-OptimizedMWFrozenFile $runtime.save_path).sha256 -ceq $locked.save.sha256) 'Save not copied or points at original'
    foreach($file in @('global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml')){
        Assert-FrozenCheck ((Get-OptimizedMWFrozenFile (Join-Path $runtime.directory $file)).sha256 -ceq (Get-OptimizedMWFrozenFile (Join-Path $baseline $file)).sha256) 'Mod storage/input/postprocess state not copied into private config'
    }
    $arguments=@(Get-OptimizedMWFrozenArguments $runtime)
    Assert-FrozenCheck ($arguments.Count -eq 7 -and $arguments[2] -ceq ('"'+$runtime.directory+'"') -and $arguments[4] -ceq ('"'+$runtime.user_data_directory+'"')) 'PS5.1 Start-Process spaced paths not quoted'
    Assert-FrozenCheck (($arguments -join ' ') -notmatch 'skip-menu|script-run|load-savegame|start=') 'Manual frozen launcher added automatic gameplay'
    [IO.File]::WriteAllText((Join-Path $live 'openmw.cfg'),'# user removed SaintJs from ordinary gameplay config')
    $second=New-OptimizedMWFrozenRuntime -Directory $baseline -GameDir $game
    Assert-FrozenCheck ($second.directory -cne $runtime.directory -and [IO.File]::ReadAllText($second.config_path).Contains((ConvertTo-OptimizedMWConfigPath $saint))) 'Allowed gameplay config drift broke immutable test cohort'
    Assert-FrozenCheck ([IO.File]::ReadAllText((Join-Path $live 'openmw.cfg')) -ceq '# user removed SaintJs from ordinary gameplay config') 'Gameplay config read/write restoration leaked into frozen tests'
    $manifest=[pscustomobject]@{configuration=[ordered]@{}}
    [IO.File]::AppendAllText($runtime.settings_path,"[Fixture]`r`nselected = true`r`n")
    $manifest=Add-OptimizedMWFrozenManifest $manifest $runtime
    Assert-FrozenCheck ($manifest.frozen_configuration.baseline_lock.sha256 -ceq $runtime.baseline_lock.sha256 -and
        $manifest.frozen_configuration.saintjs_assets_before_launch.verification -match '^path_and_length_only' -and
        $manifest.frozen_configuration.mode -match 'no_automatic_route') 'Frozen manifest lacks identities or honest asset/isolation scope'
    foreach($privateFile in @('shaders.yaml','global_storage.bin')){
        $privatePath=Join-Path $runtime.directory $privateFile
        [IO.File]::WriteAllBytes($privatePath,[byte[]]@(4,5,99))
        Assert-FrozenReject {Add-OptimizedMWFrozenManifest $manifest $runtime} 'Same-length private postprocess/mod-storage tamper accepted before launch'
        [IO.File]::Copy((Join-Path $baseline $privateFile),$privatePath,$true)
    }
    # Exercise the real launcher end-to-end in a child host, intercepting its
    # sole game launch. Deliberately provide no ordinary gameplay settings.
    foreach($file in (Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.ps1' -File)){
        if($file.Name -ne 'frozen-config-tests.ps1'){[IO.File]::Copy($file.FullName,(Join-Path $game $file.Name),$true)}
    }
    foreach($file in @('OptimizedMW_Optimizations.cfg','verify-asset-engagement.py')){[IO.File]::Copy((Join-Path $PSScriptRoot $file),(Join-Path $game $file),$true)}
    [IO.File]::Copy((Join-Path $PSScriptRoot '../../../files/optimization-settings-aliases.json'),(Join-Path $game 'optimization-settings-aliases.json'))
    [IO.File]::WriteAllBytes((Join-Path $game 'openmw.exe'),[byte[]]@(0))
    [IO.File]::WriteAllText((Join-Path $game 'CI-ID.txt'),"Ref fixture`nCommit $('a'*40)`n")
    [IO.File]::WriteAllText((Join-Path $game 'OptimizedMW_Benchmark_Report.ps1'),'# Synthetic offline report; never a game or UI.')
    $intercept=Join-Path $fixture 'intercept.json';$driver=Join-Path $fixture 'driver.ps1'
    $driverLines=@('$ErrorActionPreference=''Stop''',
        ('$env:USERPROFILE='''+(Join-Path $fixture 'missing gameplay profile').Replace("'","''")+''''),
        'function Get-Process {param($Name,$Id,$ErrorAction);if($Id){return Microsoft.PowerShell.Management\Get-Process -Id $Id -ErrorAction Stop};return $null}',
        'function Start-Process {param($FilePath,$WorkingDirectory,$ArgumentList,[switch]$PassThru,$WindowStyle)',
        'if([IO.Path]::GetFileName($FilePath) -ine ''openmw.exe''){throw ''Unexpected process request in hermetic fixture''}',
        ('$record=[ordered]@{file=$FilePath;arguments=$ArgumentList;working_directory=$WorkingDirectory};[IO.File]::WriteAllText('''+$intercept.Replace("'","''")+''',($record | ConvertTo-Json));throw ''Fixture intercepted game before execution''}'),
        ('& '''+(Join-Path $game 'OptimizedMW_Test.ps1').Replace("'","''")+''' -FrozenConfigDirectory '''+$baseline.Replace("'","''")+''' -Mode 1 -AssetMask 1 -NoPrompt'),
        'exit $LASTEXITCODE')
    [IO.File]::WriteAllLines($driver,$driverLines)
    $testHost=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    # All full-launcher fixtures use the production mutex. Coordinate with the
    # existing candidate suite, rather than making independent hosts collide.
    $fixtureMutex=[Threading.Mutex]::new($false,'Local\OptimizedMW-Candidate-Fixture-Serial')
    $ownsFixtureMutex=$false
    try{
        try{$ownsFixtureMutex=$fixtureMutex.WaitOne(60000)}catch [Threading.AbandonedMutexException]{$ownsFixtureMutex=$true}
        if(-not $ownsFixtureMutex){throw 'Run launcher fixture suites sequentially; another host still owns the fixture mutex'}
        $childOutput=& $testHost -NoProfile -ExecutionPolicy Bypass -File $driver 2>&1
    }finally{
        if($ownsFixtureMutex){$fixtureMutex.ReleaseMutex()};$fixtureMutex.Dispose()
    }
    Assert-FrozenCheck ([IO.File]::Exists($intercept)) ('Real frozen launcher failed before controlled interception: '+($childOutput | Out-String))
    $request=[IO.File]::ReadAllText($intercept) | ConvertFrom-Json
    Assert-FrozenCheck (@($request.arguments).Count -eq 7 -and $request.arguments[0] -ceq '--replace=config' -and
        $request.arguments[2].StartsWith('"') -and $request.arguments[2].EndsWith('"')) 'Real frozen launcher dropped isolated quoted arguments'
    $launchedConfig=$request.arguments[2].Trim('"')
    $captures=Join-Path $launchedConfig 'Captures'
    $run=@(Get-ChildItem -LiteralPath $captures -Directory)[0]
    $record=[IO.File]::ReadAllText((Join-Path $run.FullName 'RUN-MANIFEST.json')) | ConvertFrom-Json
    Assert-FrozenCheck ($record.completion.state -ceq 'launcher_failed' -and $record.completion.settings_restore_verified -and
        $record.configuration.settings_applied.path -ceq (Join-Path $launchedConfig 'settings.cfg') -and
        $record.frozen_configuration.baseline_lock.sha256 -ceq $runtime.baseline_lock.sha256) 'Actual frozen failure/restore manifest did not use isolated settings'
    Assert-FrozenCheck ($record.launcher_applied_openmw_environment.OPENMW_P9_DERIVED_CACHE_DIR -ceq (Join-Path $launchedConfig 'DerivedAssets') -and
        $record.configuration.save.path.StartsWith((Join-Path $launchedConfig 'UserData')+'\')) 'Real frozen launcher retained shared derived cache or original save'
    # PowerShell itself may initialize AppData/Roaming for the synthetic user.
    # Its OpenMW gameplay profile must remain absent throughout the launch.
    $unexpectedProfile=Join-Path $fixture 'missing gameplay profile/Documents/My Games/OpenMW'
    Assert-FrozenCheck (-not [IO.Directory]::Exists($unexpectedProfile)) 'Frozen launcher created an ordinary OpenMW gameplay profile'
    $restoredIdentity=Get-OptimizedMWFrozenFile (Join-Path $launchedConfig 'settings.cfg')
    $baselineSettingsIdentity=Get-OptimizedMWFrozenFile (Join-Path $baseline 'settings.cfg')
    Assert-FrozenCheck ($restoredIdentity.sha256 -ceq $baselineSettingsIdentity.sha256) ('Frozen private restoration differs: '+$restoredIdentity.sha256+' / '+$baselineSettingsIdentity.sha256)
    Assert-FrozenCheck (@(Get-ChildItem -LiteralPath (Join-Path $game 'Benchmarks') -Filter '*.zip' -File).Count -eq 1) 'Completed frozen evidence ZIP not placed in Benchmarks'
    [IO.File]::AppendAllText($runtime.config_path,'# tampered runtime')
    Assert-FrozenReject {Add-OptimizedMWFrozenManifest $manifest $runtime} 'Runtime config tamper accepted'
    $rawLock=[IO.File]::ReadAllText((Join-Path $baseline 'LOCK.json'))
    $bad=$rawLock | ConvertFrom-Json;$bad.files[0].relative_path='../outside.cfg'
    [IO.File]::WriteAllText((Join-Path $baseline 'LOCK.json'),($bad | ConvertTo-Json -Depth 8))
    Assert-FrozenReject {Read-OptimizedMWFrozenLock $baseline} 'Path traversal accepted'
    $bad=$rawLock | ConvertFrom-Json;$bad.files[0].relative_path='.. /outside.cfg'
    [IO.File]::WriteAllText((Join-Path $baseline 'LOCK.json'),($bad | ConvertTo-Json -Depth 8))
    Assert-FrozenReject {Read-OptimizedMWFrozenLock $baseline} 'Win32 trailing-space traversal alias accepted'
    [IO.File]::WriteAllText((Join-Path $baseline 'LOCK.json'),$rawLock)
    [IO.File]::AppendAllText((Join-Path $baseline 'settings.cfg'),'tamper')
    Assert-FrozenReject {New-OptimizedMWFrozenRuntime $baseline $game} 'Frozen content/hash tamper accepted'
    Write-FixtureLock
    [IO.File]::Move((Join-Path $baseline 'Optimizations.cfg'),(Join-Path $baseline 'Optimizations.saved'))
    Assert-FrozenReject {New-OptimizedMWFrozenRuntime $baseline $game} 'Missing locked file accepted'
    [IO.File]::Move((Join-Path $baseline 'Optimizations.saved'),(Join-Path $baseline 'Optimizations.cfg'))
    [IO.File]::WriteAllBytes($payload,[byte[]]@(99,12,13))
    Assert-FrozenReject {New-OptimizedMWFrozenRuntime $baseline $game -VerifyAssetHashes} 'Same-length SaintJs tamper accepted with full hashing'
    [IO.File]::WriteAllBytes($payload,[byte[]]@(11,12,13,14))
    Assert-FrozenReject {New-OptimizedMWFrozenRuntime $baseline $game} 'Changed SaintJs asset length accepted'
    [IO.File]::WriteAllBytes($payload,[byte[]]@(11,12,13))
    $junction=Join-Path $fixture 'baseline-link'
    $junctionTarget=if($PSVersionTable.PSVersion.Major -le 5){[WildcardPattern]::Escape($baseline)}else{$baseline}
    $null=New-Item -ItemType Junction -Path $junction -Target $junctionTarget
    Assert-FrozenReject {Read-OptimizedMWFrozenLock $junction} 'Reparse baseline accepted'
    # Delete the junction itself, never recurse through its target.
    [IO.Directory]::Delete($junction)
    Assert-FrozenCheck ((ConvertFrom-OptimizedMWConfigPath '"C:/A && B"') -ceq 'C:/A & B') 'OpenMW ampersand quote semantics changed'
    Assert-FrozenReject {ConvertFrom-OptimizedMWConfigPath '"C:/A" ignored'} 'Trailing ambiguous quoted path accepted'
    $source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot 'OptimizedMW_Test.ps1'))
    Assert-FrozenCheck ($source.Contains('[string]$FrozenConfigDirectory') -and $source.Contains('$UserOpenMW=$frozenRuntime.directory') -and
        $source.Contains('$SettingsPath=$frozenRuntime.settings_path;$OpenmwCfgPath=$frozenRuntime.config_path') -and
        $source.Contains('-ArgumentList $frozenArguments') -and $source.Contains('Add-OptimizedMWFrozenManifest')) 'Launcher frozen integration missing'
    Write-Output ("PASS frozen configuration: $checks checks including actual launcher interception; locked input identity, ordered assets, private state/save/display, later gameplay drift, quoted arguments, tamper/missing/traversal/reparse rejection; no game or automation invoked")
}finally{
    # One shell end-to-end, exact verified task-owned directory only.
    $resolved=[IO.Path]::GetFullPath($fixture)
    if(-not $resolved.StartsWith($taskTempRoot,[StringComparison]::OrdinalIgnoreCase) -or -not [IO.Path]::GetFileName($resolved).StartsWith('p9 frozen [space] ')){throw 'Refusing fixture cleanup outside exact temporary task root'}
    if([IO.Directory]::Exists($resolved)){Remove-Item -LiteralPath $resolved -Recurse -Force}
}
