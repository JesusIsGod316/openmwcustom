# Hermetic: synthetic files only; never starts a game, WPR, or UI.
param([switch]$FocusedOnly)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
foreach($name in @('OptimizedMW_Test.ps1','OptimizedMW_RunManifest.ps1','candidate-launcher-tests.ps1')){
    $tokens=$null;$errors=$null
    $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $root $name),[ref]$tokens,[ref]$errors)
    if($errors.Count){throw ($errors | Out-String)}
}
. (Join-Path $root 'OptimizedMW_RunManifest.ps1')
. (Join-Path $root 'OptimizedMW_ProfileArchive.ps1')
. (Join-Path $root 'OptimizedMW_AttackPresets.ps1')
. (Join-Path $root 'OptimizedMW_SettingNames.ps1')
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
    foreach($file in @('OptimizedMW_Test.ps1','OptimizedMW_QuickTest.ps1','OptimizedMW_AttackPresets.ps1','OptimizedMW_NumericMenu.ps1','OptimizedMW_MemoryProfile.ps1','OptimizedMW_SettingNames.ps1','OptimizedMW_Optimizations.cfg','OptimizedMW_ContextReport.ps1','OptimizedMW_RunManifest.ps1','OptimizedMW_HitchMatrix.ps1','OptimizedMW_ProfileArchive.ps1','OptimizedMW_AssetEngagement.ps1','verify-asset-engagement.py','OptimizedMW_HitchReport.ps1','OptimizedMW_SystemTraceLink.ps1')){
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $game $file) -Force
    }
    Copy-Item -LiteralPath (Join-Path $root '../../../files/optimization-settings-aliases.json') -Destination (Join-Path $game 'optimization-settings-aliases.json')
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
    $trials+=,@('1','CONTROL','0','Normal','1,2,1,1','0')
    $trials+=,@('1','CONTROL','15','Normal','8,2,1,1','0')
    $trials+=,@('1','CONTROL','4','Normal','3,1,1,1','0')
    $trials+=,@('1','CONTROL','4','Normal','3,2,1,1','1')
    $trials+=,@('1','CONTROL','4','Normal','3,3,1,1','129')
    $trials+=,@('1','CONTROL','4','Normal','8,5,1,1','449')
    $trials+=,@('1','CONTROL','4','Normal','12,2,1,1','1')
    $trials+=,@('1','CONTROL','0','Normal','13,1,1,1','0')
    $trials+=,@('3','CONTROL','0','Deep','13,1,2,1,1','0')
    $trials+=,@('3','CONTROL','4','Deep','12,9,2,1,1','1')
    $trials+=,@('1','CONTROL','0','Normal','', '4')
    $trials+=,@('3','CONTROL','0','Deep','', '16')
    $trials+=,@('1','CONTROL','0','Normal','', '320')
    $trials+=,@('3','CONTROL','4','Deep','', '449','defer')
    foreach($focused in @('CONTROL','CLEANUP','STEP','EARLY-STEP','TERRAIN-MIPS','COMBINED','COMBINED-MIPS')){
        foreach($depth in @('Normal','Deep')){
            $preset=Get-OptimizedMWRendererPreset $focused $depth
            $trials+=,@($(if($depth -eq 'Deep'){'3'}else{'1'}),'CONTROL','4',$depth,'',[string]$preset.attack_mask,'focused',$focused)
        }
    }
    $trials+=,@('1','CONTROL','4','Normal','2,4,1,2','261','focused','TERRAIN-MIPS','interactive')
    $trials+=,@('1','CONTROL','4','Normal','1,1,1,1','1','focused','CONTROL','interactive')
    $trials+=,@('3','CONTROL','4','Deep','2,4,2,1,2','261','focused','TERRAIN-MIPS','interactive')
    $trials+=,@('1','CONTROL','4','Normal','8,4,1,2','325','focused','COMBINED-MIPS','interactive')
    $trials+=,@('3','I1I2','5','Deep','8,1,1,10,11,12,16,4,2,1,1','321','numeric-custom','CUSTOM','interactive','custom')
    if($FocusedOnly){$trials=@($trials | Where-Object {$_.Count -ge 8})}
    foreach($trial in $trials){
        $matrixArguments=if($trial.Count -ge 4){' -HitchMask '+$trial[2]+' -TraceLevel '+$trial[3]}else{''}
        $launchArguments=' -Mode '+$trial[0]+' -Candidate '+$trial[1]+$matrixArguments+' -NoPrompt'
        if($trial.Count -ge 6 -and -not $trial[4]){$launchArguments+=' -AttackMask '+$trial[5]}
        if($trial.Count -eq 7){$launchArguments+=' -GpuQueryDefer'}
        if($trial.Count -eq 8){$launchArguments=' -FocusedPreset '+$trial[7]+' -TraceLevel '+$trial[3]+' -FrameCapLabel "fixture uncapped" -RouteLabel "fixture route" -SavePath "'+$save+'" -NoPrompt'}
        if($trial.Count -ge 5 -and $trial[4]){$launchArguments=''}
        $readHostFixture='function Read-Host { param($Prompt); return "" }'
        if($trial.Count -ge 5 -and $trial[4]){
            $answers=@($trial[4].Split(','))
            if($trial.Count -eq 9 -and $trial[7] -ne 'CONTROL'){$answers+=@($save,'fixture route','fixture uncapped')}
            $escaped=@($answers | ForEach-Object {"'"+$_.Replace("'","''")+"'"}) -join ','
            $readHostFixture='$global:fixtureAnswers=[Collections.Generic.Queue[string]]::new(); foreach($answer in @('+$escaped+')){$global:fixtureAnswers.Enqueue($answer)}; function Read-Host { param($Prompt); if($global:fixtureAnswers.Count){return $global:fixtureAnswers.Dequeue()}; if($Prompt -eq "Press Enter to close"){return ""}; throw ("Unexpected interactive prompt: "+$Prompt) }'
        }
        $driver=@(
            '$ErrorActionPreference=''Stop''',
            ('$env:USERPROFILE='''+$fakeUser.Replace("'","''")+''''),
            '$env:OPENMW_FIXTURE_KEEP=''original child value''',
            '$env:OPENMW_HITCH_COMPOSITE_PROGRESS=''inherited original progress''',
            '$env:OPENMW_HITCH_COMPOSITE_CPU_MIPS=''inherited original mips''',
            'function Get-Process { param($Name,$Id,$ErrorAction); if($Id){return Microsoft.PowerShell.Management\Get-Process -Id $Id -ErrorAction Stop}; return $null }',
            'function Start-Process { param($FilePath,$WorkingDirectory,[switch]$PassThru) throw ''Fixture intercepted game launch'' }',
            $readHostFixture,
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
        if($trial.Count -ge 6){$namePattern='HITCHV3-.+-A'+$trial[5]+'-'+$trial[3].ToUpperInvariant()+'-IO-CONTROL_'}
        if($trial.Count -ge 8 -and $trial[7] -ne 'CUSTOM'){$namePattern='RENDER1-'+$trial[7]+'-'+$trial[3].ToUpperInvariant()+'-IO-CONTROL_'}
        if($trial.Count -eq 10){$namePattern='HITCHV3-.+-A321-DEEP-IO-I1I2-PROGRESS1-MIPS1_'}
        if($record.candidate.name -ne $trial[1] -or $record.phase9_mode.Trace -ne $(if($trial[0] -eq '3'){'1'}else{'0'}) -or
            $record.profile_name -notmatch $namePattern -or $record.completion.state -ne 'launcher_failed' -or
            $record.completion.launcher_error -notmatch 'Fixture intercepted' -or -not $record.completion.settings_restore_verified -or
            -not $record.completion.environment_restore_verified -or $null -ne $record.process.pid){throw ('Complete launcher failed parameter/identity/restoration proof for '+($trial -join '|')+': '+($output | Out-String)+' '+($record | Select-Object candidate,phase9_mode,profile_name,completion,process | ConvertTo-Json -Depth 5 -Compress))}
        $expectedRefill=if($trial[1] -in @('I1','I1I2')){'65536'}else{'8192'}
        if($record.launcher_applied_openmw_environment.OPENMW_P9_FILE_REFILL_BYTES -ne $expectedRefill -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_FILE_RESIDENT_SEEK -ne $(if($trial[1] -in @('I2','I1I2')){'1'}else{'0'}) -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_CATALOG_OPTIMIZED -ne $(if($trial[1] -eq 'D1'){'1'}else{'0'}) -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_COMPILE_ATTRIBUTION -ne $(if($trial[0] -eq '3'){'1'}else{'0'}) -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_FILE_POLICY_ACK -ne '1'){throw 'Full launcher effective switches differ from profile'}
        if($trial[0] -eq '3' -and $record.launcher_applied_openmw_environment.OPENMW_P9_COMPILE_ATTRIBUTION_FILE -ne (Join-Path $run.FullName 'compile-attribution.csv')){throw 'Attribution sidecar path missing'}
        $expectedMask=if($trial.Count -ge 4){[int]$trial[2]}else{0}
        if($record.gameplay_features.hitch_mechanisms.mask -ne $expectedMask){throw 'Full launcher lost combination identity'}
        if($trial.Count -ge 6){
            if($record.trial_revision -ne $(if($trial.Count -ge 8 -and $trial[7] -ne 'CUSTOM'){'terrain-first-use-v1'}else{'hitch-reduction-v3'}) -or $record.gameplay_features.hitch_mechanisms.attack.mask -ne [int]$trial[5] -or
                $record.gameplay_features.composite_preparation -ne [bool]([int]$trial[5] -band 260) -or
                $record.launcher_applied_openmw_environment.OPENMW_HITCH_ANIMATION_LIFETIME -ne [string][int][bool]([int]$trial[5] -band 1)){
                throw 'Actual new launcher prerequisites/vector/version lost'
            }
            foreach($entry in @(@(64,'OPENMW_P9_DELETE_ADMISSION'),@(128,'OPENMW_HITCH_ANIMATION_WARM_LEASE'),@(256,'OPENMW_HITCH_COMPOSITE_STEP'))){
                $value=$record.launcher_applied_openmw_environment.PSObject.Properties[$entry[1]].Value
                if($value -cne [string][int][bool]([int]$trial[5] -band $entry[0])){throw 'Actual repair flag identity lost'}
            }
            # Numeric Deep choices explicitly enable deferred GPU collection;
            # legacy command-line rows enable it only through their switch.
            $interactiveDeep=$trial.Count -ge 5 -and $trial[4] -and $trial[3] -eq 'Deep'
            $expectedDeferred=$trial.Count -eq 7 -or $interactiveDeep -or ($trial.Count -ge 8 -and $trial[3] -eq 'Deep')
            if($record.launcher_applied_openmw_environment.OPENMW_P9_GPU_QUERY_DEFER -cne [string][int][bool]$expectedDeferred){
                throw ('Actual deferred query selection inherited another mode: '+($trial -join '|'))
            }
        }
        $progress=$false;$mips=$false
        if($trial.Count -eq 10){
            $progress=$true;$mips=$true
            if($record.gameplay_features.hitch_mechanisms.renderer.preset -ne 'CUSTOM' -or $record.launcher_selection.selection_id -ne '8.1'){throw 'Actual custom vector lost selection identity'}
        }elseif($trial.Count -ge 8){
            $focused=Get-OptimizedMWRendererPreset $trial[7] $trial[3]
            $progress=$focused.composite_progress;$mips=$focused.composite_cpu_mips
            if($record.gameplay_features.hitch_mechanisms.renderer.preset -ne $trial[7] -or
                -not $record.comparison_protocol.requested -or $record.comparison_protocol.fixed_view_seconds -ne 45){throw 'Focused renderer declaration/protocol identity lost'}
            if($trial.Count -eq 9 -and $trial[7] -eq 'CONTROL'){
                if($record.configuration.frame_cap_label -ne 'UNSPECIFIED' -or $null -ne $record.configuration.save -or
                    $record.configuration.save_identity_status -ne 'UNSPECIFIED'){throw 'Blank protocol declarations failed or invented save/cap identity'}
            }elseif($record.configuration.frame_cap_label -ne 'fixture uncapped' -or
                $record.configuration.save.sha256 -ne (Get-FileHash -LiteralPath $save).Hash.ToLowerInvariant() -or
                $record.configuration.save_identity_status -notmatch 'not_actual_load_acknowledged'){throw 'Declared save/cap identity lost'}
        }
        if($record.launcher_applied_openmw_environment.OPENMW_HITCH_COMPOSITE_PROGRESS -cne [string][int][bool]$progress -or
            $record.launcher_applied_openmw_environment.OPENMW_HITCH_COMPOSITE_CPU_MIPS -cne [string][int][bool]$mips -or
            $record.gameplay_features.composite_progress -ne [bool]$progress -or $record.gameplay_features.composite_cpu_mips -ne [bool]$mips){throw 'Actual renderer flags leaked or differ from manifest'}
        $keys=@('UI_REFLOW','COMPOSITE_BATCH','ANIMATION_PRELOAD','TERRAIN_CURRENT_FIRST')
        foreach($bit in 0..3){
            $actual=$record.launcher_applied_openmw_environment.PSObject.Properties['OPENMW_HITCH_'+$keys[$bit]].Value
            if($actual -cne [string][int][bool]($expectedMask -band (1 -shl $bit))){throw 'Full launcher applied an incorrect feature combination'}
        }
        if($trial.Count -ge 5 -and $trial[4] -and ($record.launcher_selection.menu_revision -ne 4 -or $record.launcher_selection.selection_id -notmatch '^(?:[1-9]|1[0-3])\.(?:[1-9]|10|11)$')){throw 'Numeric path identity missing from actual run manifest'}
        $custom=$trial.Count -ge 5 -and $trial[4] -in @('13,1,1,1','13,1,2,1,1')
        if($record.gameplay_features.memory_policy.name -ne $(if($custom){'Configured'}else{'Headroom'}) -or
            $record.gameplay_features.ram_mask -ne $(if($custom){0}else{4}) -or
            $record.launcher_applied_openmw_environment.OPENMW_RAM_BUDGET_V2 -ne '1' -or
            $record.profile_name -notmatch $(if($custom){'MEM2-CONFIGURED-'}else{'MEM2-HEADROOM-'})){throw 'Actual launcher lost the effective matched memory cohort'}
        if($custom){
            $sourceHash=(Get-FileHash -LiteralPath (Join-Path $game 'OptimizedMW_Optimizations.cfg')).Hash.ToLowerInvariant()
            if($record.gameplay_features.configuration_mode -ne 'Custom' -or
                $record.gameplay_features.optimization_configuration.sha256 -ne $sourceHash -or
                -not (Test-Path -LiteralPath (Join-Path $run.FullName 'optimization-configuration.cfg'))){throw 'Custom launcher lost its exact editable configuration receipt'}
            $effective=[IO.File]::ReadAllText((Join-Path $run.FullName 'settings-effective-test.cfg'))
            if($effective -notmatch 'legacy performance profile = false' -or $effective -match 'v3.6 performance profile'){throw 'Custom direct controls retained a hidden compatibility override'}
            if($effective -notmatch ('async gpu profiler = '+$(if($trial[3] -eq 'Deep'){'true'}else{'false'}))){throw 'Custom run depth did not set the observer explicitly'}
            $modeText=[IO.File]::ReadAllText((Join-Path $run.FullName 'TEST_MODE.txt'))
            if($modeText -match '(?m)^p4_compile_scheduler_mode=' -or $modeText -notmatch 'individual_settings_authority='){throw 'Custom run falsely claimed frozen benchmark settings'}
        }
        $expectedAsset=if($trial.Count -ge 5 -and $trial[4] -eq '12,2,1,1'){1}elseif($trial.Count -ge 5 -and $trial[4] -eq '12,9,2,1,1'){31}else{0}
        if($record.gameplay_features.asset_mask -ne $expectedAsset -or
            $record.families.asset_pipeline.mask -ne $expectedAsset -or
            $record.launcher_applied_openmw_environment.OPENMW_P9_ASSET_MASK -cne [string]$expectedAsset){throw 'Asset switches differ from actual run manifest'}
        if(-not (Test-Path -LiteralPath (Join-Path (Join-Path $game 'Benchmarks') ($run.Name+'.zip')))){throw 'Failed launcher discarded its raw ZIP or placed it outside Benchmarks'}
        if(Test-Path -LiteralPath (Join-Path $game ($run.Name+'.zip'))){throw 'Completed benchmark ZIP cluttered the installed game directory'}
    }
    Write-Host "PASS candidate launcher: $($trials.Count) isolated launch paths; exact switches, focused/legacy control identities, save/cap declarations, restoration, hashes and retained failure ZIPs; no game/WPR/UI."
}finally{
    if($fixtureLock){try{if($ownsFixtureLock){$fixtureLock.ReleaseMutex()}}finally{$fixtureLock.Dispose()}}
    $restored=Restore-Phase9Environment $before
    $absolute=[IO.Path]::GetFullPath($fixture)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected candidate fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
    if(-not $restored){throw 'Host environment did not restore after fixture'}
}
exit 0
