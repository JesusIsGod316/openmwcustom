# Hermetic: synthetic files only; never starts a game, WPR, or UI.
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
foreach($name in @('OptimizedMW_Test.ps1','OptimizedMW_RunManifest.ps1','candidate-launcher-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
. (Join-Path $root 'OptimizedMW_RunManifest.ps1')
. (Join-Path $root 'OptimizedMW_ProfileArchive.ps1')
if((Get-Phase9IdentityHash -Value @()) -notmatch '^[a-f0-9]{64}$'){throw 'Empty file inventory identity unsupported'}
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root 'OptimizedMW_Test.ps1'),[ref]$tokens,[ref]$errors)
foreach($name in @('Get-P9Mode','Set-IniValue')){
    $fn=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
    if(-not $fn){throw "Missing launcher function: $name"}
    . ([scriptblock]::Create($fn.Extent.Text))
}
$before=Get-Phase9EnvironmentSnapshot
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$fixture=Join-Path $tempRoot ('p9 [candidate] '+[Guid]::NewGuid().ToString('N'))
$game=Join-Path $fixture 'game';$profile=Join-Path $fixture 'profile'
$fixtureLock=$null;$ownsFixtureLock=$false
New-Item -ItemType Directory -Path $game,$profile | Out-Null
try{
    $env:OPENMW_FIXTURE_KEEP='prior value'
    $saved=Get-Phase9EnvironmentSnapshot
    foreach($name in @('CONTROL','I1','I2','I1I2','D1')){
        $candidate=Get-Phase9Candidate -Name $name
        $mode=Get-P9Mode $(if($name -eq 'D1'){'3'}else{'1'})
        Set-Phase9CandidateEnvironment $candidate $mode
        $expectedRefill=if($name -in @('I1','I1I2')){'65536'}else{'8192'}
        $expectedSeek=if($name -in @('I2','I1I2')){'1'}else{'0'}
        $expectedCatalog=if($name -eq 'D1'){'1'}else{'0'}
        if($env:OPENMW_P9_FILE_REFILL_BYTES -ne $expectedRefill -or $env:OPENMW_P9_FILE_RESIDENT_SEEK -ne $expectedSeek -or
            $env:OPENMW_P9_CATALOG_OPTIMIZED -ne $expectedCatalog -or $env:OPENMW_P9_FILE_BUFFER_BUDGET_BYTES -ne '33554432' -or
            $env:OPENMW_P9_COMPILE_ATTRIBUTION -ne $mode.Trace){throw "Candidate parameter isolation failed: $name"}
        if($env:OPENMW_P9_FILE_POLICY_ACK -ne '1'){throw 'Requested startup acknowledgement not propagated'}
    }
    Set-Phase9CandidateEnvironment (Get-Phase9Candidate CONTROL) (Get-P9Mode 3)
    Set-Phase9HitchRecorderEnvironment -Mode (Get-P9Mode 3) -Transport stream
    if($env:OPENMW_P9_HITCH_TRANSPORT -ne 'stream'){throw 'Deep streaming recorder was not selected'}
    Set-Phase9HitchRecorderEnvironment -Mode (Get-P9Mode 3) -Transport deferred
    if($env:OPENMW_P9_HITCH_TRANSPORT -ne 'deferred'){throw 'Deferred recorder control was not selected'}
    Set-Phase9HitchRecorderEnvironment -Mode (Get-P9Mode 1)
    if(Test-Path Env:OPENMW_P9_HITCH_TRANSPORT){throw 'Normal retained a Deep recorder transport request'}
    if($env:OPENMW_P9_FILE_REFILL_BYTES -ne '8192' -or $env:OPENMW_P9_FILE_RESIDENT_SEEK -ne '0' -or
        $env:OPENMW_P9_CATALOG_OPTIMIZED -ne '0'){throw 'Deep reference inherited a candidate'}
    $candidate=Get-Phase9Candidate I1 -BufferBudgetBytes 65536
    Set-Phase9CandidateEnvironment $candidate (Get-P9Mode 1)
    if($env:OPENMW_P9_FILE_BUFFER_BUDGET_BYTES -ne '65536'){throw 'Lower candidate budget not propagated'}
    $candidate=Get-Phase9Candidate I1 -BufferBudgetBytes 0
    Set-Phase9CandidateEnvironment $candidate (Get-P9Mode 1)
    if($env:OPENMW_P9_FILE_BUFFER_BUDGET_BYTES -ne '0'){throw 'Zero cap not propagated'}
    foreach($bad in @(-1,33554433)){
        $rejected=$false;try{$null=Get-Phase9Candidate I1 -BufferBudgetBytes $bad}catch{$rejected=$true}
        if(-not $rejected){throw 'Unsupported budget accepted'}
    }
    $rejected=$false;try{$null=Get-Phase9Candidate 'UNDEFINED'}catch{$rejected=$true}
    if(-not $rejected){throw 'Unknown candidate accepted'}
    $rejected=$false;try{Set-Phase9CandidateEnvironment (Get-Phase9Candidate D1) (Get-P9Mode 1)}catch{$rejected=$true}
    if(-not $rejected){throw 'Unarmed D1 accepted'}
    if(-not (Restore-Phase9Environment $saved) -or $env:OPENMW_FIXTURE_KEEP -cne 'prior value'){throw 'Environment restoration/read-back failed'}

    [IO.File]::WriteAllBytes((Join-Path $game 'openmw.exe'),[byte[]]@(1,2,3,4))
    [IO.File]::WriteAllBytes((Join-Path $game 'lua51.dll'),[byte[]]@(5,6,7,8))
    [IO.File]::WriteAllText((Join-Path $game 'launcher.ps1'),'# fixture, never executed')
    [IO.File]::WriteAllText((Join-Path $game 'CI-ID.txt'),"Ref fixture`nCommit $('a'*40)`n")
    $settings=Join-Path $fixture 'settings.cfg';$cfg=Join-Path $fixture 'openmw.cfg';$save=Join-Path $fixture 'save.omwsave'
    [IO.File]::WriteAllText($settings,"[Fixture]`r`nunknown = keep`r`n[Cells]`r`nretained = yes`r`n")
    [IO.File]::WriteAllText($cfg,'data="fixture assets"')
    [IO.File]::WriteAllBytes($save,[byte[]]@(11,12,13))
    $original=Get-FileHash -LiteralPath $settings
    Copy-Item -LiteralPath $settings -Destination (Join-Path $profile 'settings-before.cfg')
    Set-IniValue $settings 'Cells' 'retained' 'changed'
    Set-IniValue $settings 'New' 'candidate' '1'
    if([IO.File]::ReadAllText($settings) -notmatch 'retained = changed' -or [IO.File]::ReadAllText($settings) -notmatch 'unknown = keep'){throw 'Effective INI did not preserve unrelated settings'}
    $candidate=Get-Phase9Candidate D1
    $mode=Get-P9Mode 3
    Set-Phase9CandidateEnvironment $candidate $mode
    $manifest=New-Phase9RunManifest -GameDir $game -ProfileDir $profile -Mode $mode -Candidate $candidate `
        -SettingsPath $settings -OpenmwCfgPath $cfg -SavePath $save -RouteLabel 'fixture route' -Cohort fresh-process-first-visit
    if($manifest.executable.sha256 -ne (Get-FileHash -LiteralPath (Join-Path $game 'openmw.exe')).Hash.ToLowerInvariant() -or
        $manifest.runtime_dlls.Count -ne 1 -or $manifest.runtime_dlls[0].sha256 -ne (Get-FileHash -LiteralPath (Join-Path $game 'lua51.dll')).Hash.ToLowerInvariant() -or
        $manifest.engine_source_commit_from_build_identity -ne ('a'*40) -or $manifest.activation_evidence.state -ne 'pending' -or
        $manifest.configuration.settings_applied.sha256 -eq $original.Hash.ToLowerInvariant() -or
        $manifest.configuration.save.sha256 -ne (Get-FileHash -LiteralPath $save).Hash.ToLowerInvariant() -or
        $manifest.configuration.save_identity_status -notmatch 'not_actual_load_acknowledged' -or
        $manifest.families.io_candidates.i1_requested -or $manifest.families.io_candidates.i2_requested -or
        -not $manifest.families.catalog_candidate.d1_requested -or
        $manifest.launcher_applied_openmw_environment.OPENMW_P9_CATALOG_OPTIMIZED -ne '1'){throw 'Manifest identity/effective settings separation failed'}
    if($manifest.candidate_sha256 -ne (Get-Phase9IdentityHash $candidate) -or
        $manifest.family_sha256.io_candidates -ne (Get-Phase9IdentityHash $manifest.families.io_candidates)){throw 'Candidate/family hash mismatch'}
    $manifest.process.pid=1234;$manifest.process.creation_utc='2026-10-01T00:00:00.1234567Z'
    $manifest.process.creation_ticks='639264960001234567';$manifest.process.creation_source='fixture (not native)'
    $manifest.completion.state='launcher_failed';$manifest.completion.launcher_error='fixture Start-Process failure'
    Copy-Item -LiteralPath (Join-Path $profile 'settings-before.cfg') -Destination $settings -Force
    $manifest.completion.settings_restore_verified=((Get-FileHash -LiteralPath $settings).Hash -eq $original.Hash)
    $manifest.completion.environment_restore_verified=Restore-Phase9Environment $saved
    $manifest.completion.restored_settings_sha256=(Get-FileHash -LiteralPath $settings).Hash.ToLowerInvariant()
    Save-Phase9RunManifest $manifest $profile
    Complete-Phase9RunManifest $manifest $profile
    $roundtrip=Get-Content -Raw -LiteralPath (Join-Path $profile 'RUN-MANIFEST.json') | ConvertFrom-Json
    if($roundtrip.process.creation_ticks -cne '639264960001234567' -or -not $roundtrip.completion.settings_restore_verified -or
        -not $roundtrip.completion.environment_restore_verified -or $roundtrip.activation_evidence.state -ne 'unverified'){throw 'Failed-run identity/restoration/precision lost'}
    $status=Join-Path $profile 'p9-draw-phases.csv.status.txt'
    foreach($fallback in @(0,2)){
        @('resource_catalog_algorithm_requested=ordered_merge',"resource_catalog_candidate_scratch_allocation_failures=$fallback") | Set-Content -LiteralPath $status
        Complete-Phase9RunManifest $manifest $profile
        if(-not $manifest.activation_evidence.catalog.selection_consistent -or
            $manifest.activation_evidence.catalog.optimized_no_reported_scratch_fallback -ne ($fallback -eq 0)){throw 'D1 fallback proof hidden'}
    }
    'resource_catalog_algorithm_requested=legacy' | Set-Content -LiteralPath $status
    Complete-Phase9RunManifest $manifest $profile
    if($manifest.activation_evidence.catalog.selection_consistent -or $manifest.activation_evidence.catalog.optimized_no_reported_scratch_fallback){throw 'Legacy algorithm promoted as D1'}
    $log=Join-Path $profile 'openmw.log'
    $ack='[10:00:00.000 I] P9 file policy: refill_bytes=8192 resident_seek=0 budget_bytes=33554432'
    [IO.File]::WriteAllText($log,$ack)
    Complete-Phase9RunManifest $manifest $profile
    if(-not $manifest.activation_evidence.io_startup_policy.selection_consistent -or
        $manifest.activation_evidence.io_startup_policy.per_stream_effective_allocation_proved){throw 'Startup acknowledgement became per-stream allocation proof'}
    [IO.File]::WriteAllLines($log,@($ack,$ack))
    Complete-Phase9RunManifest $manifest $profile
    if($manifest.activation_evidence.io_startup_policy.selection_consistent){throw 'Ambiguous duplicate startup acknowledgements accepted'}
    [IO.File]::WriteAllText($log,$ack.Replace('8192','65536'))
    Complete-Phase9RunManifest $manifest $profile
    if($manifest.activation_evidence.io_startup_policy.selection_consistent){throw 'Mismatched engine requested policy accepted'}
    [IO.File]::WriteAllText($log,'P9 file policy: refill_bytes=invalid resident_seek=0 budget_bytes=33554432')
    Complete-Phase9RunManifest $manifest $profile
    if($manifest.activation_evidence.Contains('io_startup_policy')){throw 'Malformed log retained stale startup acknowledgement'}
    @('experiment=fixture','hitch_trial_revision=1') | Set-Content -LiteralPath (Join-Path $profile 'TEST_MODE.txt')
    $zipPath=Join-Path $fixture 'fixture.zip'
    $zip=New-VerifiedProfileZip -SourceDir $profile -DestinationPath $zipPath
    $zipReader=[IO.Compression.ZipFile]::OpenRead($zip.Path)
    try{
        $entry=$zipReader.GetEntry('RUN-MANIFEST.json');if(-not $entry){throw 'ZIP omitted run identity'}
        $stream=$entry.Open();$hasher=[Security.Cryptography.SHA256]::Create()
        try{$hash=[BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-','')}finally{$stream.Dispose();$hasher.Dispose()}
        if($hash -ne (Get-FileHash -LiteralPath (Join-Path $profile 'RUN-MANIFEST.json')).Hash){throw 'Archived run manifest differs from source'}
    }finally{$zipReader.Dispose()}
    $zipHash=(Get-FileHash -LiteralPath $zipPath).Hash
    $rejected=$false;try{$null=New-VerifiedProfileZip -SourceDir $profile -DestinationPath $zipPath}catch{$rejected=$true}
    if(-not $rejected -or (Get-FileHash -LiteralPath $zipPath).Hash -ne $zipHash){throw 'Existing capture archive overwritten'}
    $rawInventory=Get-Content -Raw -LiteralPath (Join-Path $profile 'RAW-FILE-MANIFEST.json') | ConvertFrom-Json
    $rawManifest=@($rawInventory.files | Where-Object {$_.name -eq 'RUN-MANIFEST.json'})
    if($rawManifest.Count -ne 1 -or $rawManifest[0].sha256 -ne (Get-FileHash -LiteralPath (Join-Path $profile 'RUN-MANIFEST.json')).Hash){throw 'Persisted raw inventory lost run identity'}

    # Exercise the complete launcher parameter/failure/restoration path in a child
    # PowerShell. A mock intercepts the only game Start-Process call before launch.
    # Production launchers intentionally share one mutex; serialize these fixture
    # children across PS5.1/PS7 test hosts without changing production locking.
    $fixtureLock=[Threading.Mutex]::new($false,'Local\OptimizedMW-Candidate-Fixture-Serial')
    try{$ownsFixtureLock=$fixtureLock.WaitOne(30000)}catch [Threading.AbandonedMutexException]{$ownsFixtureLock=$true}
    if(-not $ownsFixtureLock){throw 'Timed out waiting for independent launcher fixture host'}
    foreach($file in @('OptimizedMW_Test.ps1','OptimizedMW_QuickTest.ps1','OptimizedMW_AttackPresets.ps1','OptimizedMW_ContextReport.ps1','OptimizedMW_RunManifest.ps1','OptimizedMW_HitchMatrix.ps1','OptimizedMW_ProfileArchive.ps1','OptimizedMW_HitchReport.ps1','OptimizedMW_SystemTraceLink.ps1')){
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $game $file) -Force
    }
    [IO.File]::WriteAllText((Join-Path $game 'OptimizedMW_Benchmark_Report.ps1'),'# Empty hermetic report fixture; no game, WPR, or UI.')
    $fakeUser=Join-Path $fixture 'user';$userConfig=Join-Path $fakeUser 'Documents/My Games/OpenMW'
    New-Item -ItemType Directory -Path $userConfig -Force | Out-Null
    $userSettings=Join-Path $userConfig 'settings.cfg'
    [IO.File]::WriteAllText($userSettings,"[Fixture]`r`nretained = exact bytes`r`n")
    $userHash=(Get-FileHash -LiteralPath $userSettings).Hash
    $driverPath=Join-Path $fixture 'driver.ps1'
    $shell=[Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $trials=@(@('1','I1'),@('3','CONTROL'),@('3','D1'))
    foreach($mask in 0..15){
        $trials+=,@('1','CONTROL',[string]$mask,'Normal')
        $trials+=,@('3','CONTROL',[string]$mask,'Deep')
    }
    # Exercise the actual simplified first screen, not just its preset helper.
    $trials+=,@('1','CONTROL','0','Normal','2')
    $trials+=,@('1','CONTROL','15','Normal','3')
    $trials+=,@('1','CONTROL','4','Normal','5','0')
    $trials+=,@('1','CONTROL','4','Normal','6','1')
    $trials+=,@('1','CONTROL','0','Normal','', '4')
    $trials+=,@('3','CONTROL','0','Deep','', '16')
    foreach($trial in $trials){
        $matrixArguments=if($trial.Count -ge 4){' -HitchMask '+$trial[2]+' -TraceLevel '+$trial[3]}else{''}
        $launchArguments=' -Mode '+$trial[0]+' -Candidate '+$trial[1]+$matrixArguments+' -NoPrompt'
        if($trial.Count -eq 6 -and -not $trial[4]){$launchArguments+=' -AttackMask '+$trial[5]}
        if($trial.Count -ge 5 -and $trial[4]){$launchArguments=''}
        $driver=@(
            '$ErrorActionPreference=''Stop''',
            ('$env:USERPROFILE='''+$fakeUser.Replace("'","''")+''''),
            '$env:OPENMW_FIXTURE_KEEP=''original child value''',
            'function Get-Process { param($Name,$Id,$ErrorAction); if($Id){return Microsoft.PowerShell.Management\Get-Process -Id $Id -ErrorAction Stop}; return $null }',
            'function Start-Process { param($FilePath,$WorkingDirectory,[switch]$PassThru) throw ''Fixture intercepted game launch'' }',
            ('function Read-Host { param($Prompt); if($Prompt -like ''Test*''){return '''+$(if($trial.Count -ge 5){$trial[4]}else{''})+'''}; return '''' }'),
            ('& '''+(Join-Path $game 'OptimizedMW_Test.ps1').Replace("'","''")+''''+$launchArguments),
            'exit $LASTEXITCODE'
        )
        [IO.File]::WriteAllLines($driverPath,$driver)
        $output=& $shell -NoProfile -ExecutionPolicy Bypass -File $driverPath 2>&1
        if($LASTEXITCODE -ne 1){throw ('Mocked launcher did not return its controlled failure: '+($output | Out-String))}
        if((Get-FileHash -LiteralPath $userSettings).Hash -ne $userHash){throw 'Complete launcher did not restore fixture settings'}
        $profiles=Join-Path $userConfig 'OptimizedMW-Phase9-Profiles'
        if(-not (Test-Path -LiteralPath $profiles)){throw ('Mocked launcher profile missing: '+($output | Out-String))}
        $run=Get-ChildItem -LiteralPath $profiles -Directory | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
        $record=Get-Content -Raw -LiteralPath (Join-Path $run.FullName 'RUN-MANIFEST.json') | ConvertFrom-Json
        $namePattern=if($trial.Count -ge 4){'HITCHV2-.+-'+$trial[3].ToUpperInvariant()+'-IO-CONTROL_'}else{'-HITCHV1-'+$trial[1]+'_'}
        if($trial.Count -eq 6){$namePattern='HITCHV3-.+-A'+$trial[5]+'-'+$trial[3].ToUpperInvariant()+'-IO-CONTROL_'}
        if($record.candidate.name -ne $trial[1] -or $record.phase9_mode.Trace -ne $(if($trial[0] -eq '3'){'1'}else{'0'}) -or
            $record.profile_name -notmatch $namePattern -or $record.completion.state -ne 'launcher_failed' -or
            $record.completion.launcher_error -notmatch 'Fixture intercepted' -or -not $record.completion.settings_restore_verified -or
            -not $record.completion.environment_restore_verified -or $null -ne $record.process.pid){throw ('Complete launcher failed parameter/identity/restoration proof: '+($record | Select-Object candidate,phase9_mode,profile_name,completion,process | ConvertTo-Json -Depth 5 -Compress))}
        $expectedRefill=if($trial[1] -eq 'I1'){'65536'}else{'8192'}
        if($record.launcher_applied_openmw_environment.OPENMW_P9_FILE_REFILL_BYTES -ne $expectedRefill -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_FILE_RESIDENT_SEEK -ne '0' -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_CATALOG_OPTIMIZED -ne $(if($trial[1] -eq 'D1'){'1'}else{'0'}) -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_COMPILE_ATTRIBUTION -ne $(if($trial[0] -eq '3'){'1'}else{'0'}) -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_FILE_POLICY_ACK -ne '1'){throw 'Full launcher effective switches differ from profile'}
        if($trial[0] -eq '3' -and $record.launcher_applied_openmw_environment.OPENMW_P9_COMPILE_ATTRIBUTION_FILE -ne (Join-Path $run.FullName 'compile-attribution.csv')){throw 'Attribution sidecar path missing'}
        $expectedMask=if($trial.Count -ge 4){[int]$trial[2]}else{0}
        if($record.gameplay_features.hitch_mechanisms.mask -ne $expectedMask){throw 'Full launcher lost combination identity'}
        if($trial.Count -eq 6){
            if($record.trial_revision -ne 'hitch-reduction-v3' -or $record.gameplay_features.hitch_mechanisms.attack.mask -ne [int]$trial[5] -or
                $record.gameplay_features.composite_preparation -ne [bool]([int]$trial[5] -band 4) -or
                $record.launcher_applied_openmw_environment.OPENMW_HITCH_ANIMATION_LIFETIME -ne [string][int][bool]([int]$trial[5] -band 1)){
                throw 'Actual new launcher prerequisites/vector/version lost'
            }
        }
        $keys=@('UI_REFLOW','COMPOSITE_BATCH','ANIMATION_PRELOAD','TERRAIN_CURRENT_FIRST')
        foreach($bit in 0..3){
            $actual=$record.launcher_applied_openmw_environment.PSObject.Properties['OPENMW_HITCH_'+$keys[$bit]].Value
            if($actual -cne [string][int][bool]($expectedMask -band (1 -shl $bit))){throw 'Full launcher applied an incorrect feature combination'}
        }
        if(-not (Test-Path -LiteralPath (Join-Path $game ($run.Name+'.zip')))){throw 'Failed launcher discarded its raw ZIP'}
    }
    Write-Host 'PASS candidate launcher: 32 full Normal/Deep combination launch paths plus retained I/O/D1 modes; exact switches, control, caps, restoration, hashes, failure/raw archive and activation fallback proof; no game/WPR/UI.'
}finally{
    if($fixtureLock){try{if($ownsFixtureLock){$fixtureLock.ReleaseMutex()}}finally{$fixtureLock.Dispose()}}
    $restored=Restore-Phase9Environment $before
    $absolute=[IO.Path]::GetFullPath($fixture)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected candidate fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
    if(-not $restored){throw 'Host environment did not restore after fixture'}
}
exit 0
