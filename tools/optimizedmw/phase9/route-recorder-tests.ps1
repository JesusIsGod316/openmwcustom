# Hermetic recorder host checks. No game, UI, ETW, or original profile writes.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_FrozenConfig.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RouteRecorder.ps1') -LibraryOnly
$checks=0
function Check-Route {param([bool]$Condition,[string]$Message) if(-not $Condition){throw $Message};$script:checks++}
function Reject-Route {param([scriptblock]$Action,[string]$Message) $rejected=$false;try{$null=&$Action}catch{$rejected=$true};Check-Route $rejected $Message}
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('Route recorder space '+[Guid]::NewGuid().ToString('N'))
$fixture=[IO.Path]::GetFullPath($fixture)
[void][IO.Directory]::CreateDirectory($fixture)
$game=Join-Path $fixture 'game';$live=Join-Path $fixture 'ordinary';$locked=Join-Path $fixture 'locked';$package=Join-Path $fixture 'package'
try{
    foreach($directory in @($game,(Join-Path $game 'resources/vfs-mw'),$live,(Join-Path $locked 'saves/Character'),(Join-Path $package 'scripts/optimizedmw_route_recorder'))){[void][IO.Directory]::CreateDirectory($directory)}
    foreach($name in @('OptimizedMW_RouteRecorder.ps1','route-recorder-tests.ps1')){
        $tokens=$null;$parseErrors=$null;$null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$parseErrors)
        Check-Route ($parseErrors.Count -eq 0) ('PowerShell source parse failed: '+$name)
    }
    $saint=Join-Path $live 'saintjs/Core';[void][IO.Directory]::CreateDirectory($saint)
    $payload=Join-Path $saint 'fixture.dds';[IO.File]::WriteAllBytes($payload,[byte[]]@(1,2,3))
    $asset=Get-OptimizedMWFrozenFile $payload
    Write-OptimizedMWRouteJson (Join-Path $locked 'SAINTJS-ASSETS.json') ([ordered]@{schema=1;ordered_data_paths=@($saint);files=@([ordered]@{path=$payload;data_directory=$saint;relative_path='fixture.dds';bytes=$asset.bytes;sha256=$asset.sha256})})
    [IO.File]::WriteAllText((Join-Path $locked 'openmw.cfg'),('data='+(ConvertTo-OptimizedMWConfigPath $saint)+"`ncontent=Morrowind.esm`n"))
    [IO.File]::WriteAllText((Join-Path $locked 'settings.cfg'),"[Video]`nfullscreen = true`nresolution x = 1920`nresolution y = 1080`n[Optimizations]`nrender handoff attribution = true`nasync gpu profiler = true`n")
    [IO.File]::WriteAllText((Join-Path $locked 'Optimizations.cfg'),"[Optimizations]`nrender threading mode = 0`n")
    $xml='<Controller><Control name="9"><KeyBinder key="63" direction="INCREASE"/><KeyBinder key="26" direction="INCREASE"/></Control><Control name="26"><KeyBinder key="66" direction="INCREASE"/><JoystickButtonBinder button="2"/></Control><Control name="42"><KeyBinder key="68" direction="INCREASE"/></Control><Control name="43"><KeyBinder key="67" direction="INCREASE"/></Control><Control name="52"><KeyBinder key="65" direction="INCREASE"/></Control></Controller>'
    [IO.File]::WriteAllText((Join-Path $locked 'input_v3.xml'),$xml)
    foreach($name in @('global_storage.bin','player_storage.bin','shaders.yaml','saves/Character/save.omwsave')){[IO.File]::WriteAllBytes((Join-Path $locked $name),[byte[]]@(4,5,6))}
    $files=@();foreach($name in @('openmw.cfg','settings.cfg','Optimizations.cfg','SAINTJS-ASSETS.json','input_v3.xml','global_storage.bin','player_storage.bin','shaders.yaml','saves/Character/save.omwsave')){
        $identity=Get-OptimizedMWFrozenFile (Join-Path $locked $name);$files+=@([ordered]@{relative_path=$name;bytes=$identity.bytes;sha256=$identity.sha256})
        if($name -notin @('SAINTJS-ASSETS.json','Optimizations.cfg')){[void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName((Join-Path $live $name)));[IO.File]::Copy($identity.path,(Join-Path $live $name))}
    }
    Write-OptimizedMWRouteJson (Join-Path $locked 'LOCK.json') ([ordered]@{schema=1;files=$files;source_configuration_directory=$live;source_user_data_directory=$live})
    [IO.File]::WriteAllText((Join-Path $game 'openmw.exe'),'Never execute this fixture')
    [IO.File]::WriteAllText((Join-Path $package 'optimizedmw_route_recorder.omwscripts'),'PLAYER: scripts/optimizedmw_route_recorder/player.lua')
    [IO.File]::WriteAllText((Join-Path $package 'scripts/optimizedmw_route_recorder/player.lua'),'return {}')
    $encounterPolicyPath=Join-Path $package 'encounter-policy.json'
    $encounterPolicy=[ordered]@{schema=1;frozen_config_sha256=(Get-OptimizedMWFrozenFile (Join-Path $locked 'openmw.cfg')).sha256;content_files=@($asset);hostile_record_ids=@('rat','kwama forager','scrib','nix-hound');scope='Hermetic fixture encounter group, not installed mod definitions'}
    Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    $baseline=Read-OptimizedMWFrozenLock $locked;$before=Get-OptimizedMWRouteProtectedInputs $baseline
    $existing=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing
    Check-Route ($existing.arguments -contains '--load-savegame' -and $existing.arguments -notcontains '--script-run') 'Existing route startup unexpectedly teleports'
    Check-Route (($existing.arguments -join ' ').Contains('"'+$existing.runtime.save_path+'"')) 'Spaced copied save path is not quoted'
    Check-Route ([IO.File]::ReadAllText($existing.runtime.config_path).EndsWith("content=optimizedmw_route_recorder.omwscripts`n")) 'Recorder is not last content source'
    Check-Route ([IO.File]::ReadAllText((Join-Path $existing.runtime.directory 'input_v3.xml')) -notmatch 'key="(63|66|67|68)"') 'Native recorder hotkeys were not removed'
    Check-Route ([IO.File]::ReadAllText((Join-Path $existing.runtime.directory 'input_v3.xml')) -match 'key="26"' -and [IO.File]::ReadAllText((Join-Path $existing.runtime.directory 'input_v3.xml')) -match 'JoystickButtonBinder' -and [IO.File]::ReadAllText((Join-Path $existing.runtime.directory 'input_v3.xml')) -match 'key="65"') 'Unrelated bindings changed'
    Check-Route ($existing.manifest.native_recorder_bindings_parked.Count -eq 4 -and [IO.File]::ReadAllText((Join-Path $existing.runtime.directory 'input_v3.xml')) -match 'key="113"') 'Native bindings are not parked or documented; default QuickLoad could return'
    Check-Route ($existing.manifest.frozen_configuration.effective_config.sha256 -cne $existing.manifest.derived_private_inputs[0].sha256) 'Private derivative was not distinguished from frozen baseline'
    Check-Route ([IO.File]::ReadAllText($existing.runtime.settings_path) -match 'render handoff attribution = false' -and [IO.File]::ReadAllText($existing.runtime.settings_path) -match 'async gpu profiler = false') 'Route observers remain enabled'
    Check-Route ($existing.manifest.validation_window -eq $false -and [IO.File]::ReadAllText($existing.runtime.settings_path) -match 'resolution x = 1920') 'Default route recording display changed'
    Check-Route ([IO.File]::ReadAllText($existing.manifest.options.path) -match 'require_ctrl_shift=false') 'Recorder requires native sneak/run modifier keys'
    Check-Route ([IO.File]::ReadAllText($existing.manifest.options.path) -match "hostile_record_ids=\{'rat','kwama forager','scrib','nix-hound'\}") 'Alternative encounter records are not admitted independently of Fight'
    Check-Route ($existing.manifest.encounter_policy.explicit_record_count -eq 4 -and $existing.manifest.encounter_policy.identity.sha256 -ceq (Get-OptimizedMWFrozenFile $encounterPolicyPath).sha256) 'Encounter policy identity or count is not frozen in the session'
    Assert-OptimizedMWRouteSessionInputs $existing;Check-Route $true 'Unchanged recorder inputs failed prelaunch qualification'
    $native=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing -PlaybackCompatible -NativeMovement
    Check-Route ($native.manifest.playback_compatible -and $native.manifest.maximum_acquisition_simulation_seconds -eq 180 -and $native.manifest.movement_cohort.private_override -eq $true -and $native.manifest.movement_cohort.model -ceq 'native_input_displacement_with_visual_animations') 'Outdoor recording receipt does not identify its bounded native movement cohort'
    Check-Route ([IO.File]::ReadAllText($native.runtime.settings_path) -match 'player movement ignores animation = true' -and [IO.File]::ReadAllText($native.manifest.options.path) -match 'playback_compatible=true' -and [IO.File]::ReadAllText($native.manifest.options.path) -match 'max_seconds=180' -and [IO.File]::ReadAllText($native.manifest.options.path).Contains("movement_cohort={model='native_input_displacement_with_visual_animations',private_override=true}")) 'Private recording setting, Lua metadata and duration disagree'
    Check-Route ($native.arguments -notcontains '--script-run' -and $native.manifest.starting_location_mode -ceq 'load_saved_location') 'Outdoor guard preparation relocated the saved world'
    Assert-OptimizedMWRouteSessionInputs $native;Check-Route $true 'Native derivative identities were captured before settings writes finished'
    $animation=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing -PlaybackCompatible
    Check-Route ($animation.manifest.movement_cohort.model -ceq 'animation_displacement' -and $animation.manifest.movement_cohort.private_override -eq $false -and [IO.File]::ReadAllText($animation.runtime.settings_path) -match 'player movement ignores animation = false') 'Explicit animation displacement control silently uses native movement'
    Check-Route ($existing.manifest.movement_cohort -eq $null -and $existing.manifest.playback_compatible -eq $false -and $existing.manifest.maximum_acquisition_simulation_seconds -eq $null -and $existing.manifest.maximum_acquisition_wall_seconds -eq 600) 'Legacy recording constructor inherited outdoor/native restrictions'
    Check-Route ($native.manifest.maximum_acquisition_wall_seconds -eq 300 -and [IO.File]::ReadAllText($native.manifest.options.path).Contains('max_wall_seconds=300')) 'Compatible acquisition wallclock bound differs from compiler admission'
    Check-Route (Compare-OptimizedMWRouteProtectedInputs $before (Get-OptimizedMWRouteProtectedInputs $baseline)) 'Native recording override altered frozen/normal originals'
    [IO.File]::AppendAllText($existing.runtime.config_path,'# mutation')
    Reject-Route {Assert-OptimizedMWRouteSessionInputs $existing} 'Changed private recorder configuration accepted'
    $balmora=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName balmora -SmokeRun -SmokeDuration 5 -SmallValidationWindow
    Check-Route ($balmora.arguments -contains '--script-run' -and [IO.File]::ReadAllText($balmora.manifest.setup_script.path) -ceq ('coc "Balmora"'+"`n")) 'Balmora setup is not a quoted named-cell COC'
    Check-Route ($balmora.runtime.directory -cne $existing.runtime.directory) 'Route profiles are not isolated'
    Check-Route ([IO.File]::ReadAllText($balmora.manifest.options.path) -match "smoke_autostart=true" -and [IO.File]::ReadAllText($balmora.manifest.options.path) -match "route_name='balmora'") 'Smoke options are not bounded and explicit'
    Check-Route ([IO.File]::ReadAllText($balmora.manifest.options.path) -match "hostile_record_ids=\{'rat','kwama forager','scrib','nix-hound'\}") 'Balmora private policy does not retain the audited encounter group'
    Check-Route ($balmora.manifest.validation_window -and [IO.File]::ReadAllText($balmora.runtime.settings_path) -match 'resolution x = 1280' -and [IO.File]::ReadAllText($balmora.runtime.settings_path) -match 'resolution y = 720') 'Validation display is not marked separately'
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName balmora} 'Normal Balmora acquisition accepted an unbound seed and implicit teleport'
    $balmoraLock=Get-Content -LiteralPath (Join-Path $locked 'LOCK.json') -Raw | ConvertFrom-Json
    $balmoraLock | Add-Member -NotePropertyName route_start -NotePropertyValue ([pscustomobject]@{schema=1;route='balmora';mode='load_saved_location';save_relative_path=$baseline.save.relative_path;save_sha256=$baseline.save.sha256;saved_cell="Balmora, Caius Cosades' House"})
    Write-OptimizedMWRouteJson (Join-Path $locked 'LOCK.json') $balmoraLock
    $boundBalmora=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName balmora
    Check-Route ($boundBalmora.arguments -notcontains '--script-run' -and $null -eq $boundBalmora.manifest.setup_script -and $boundBalmora.manifest.starting_location_mode -ceq 'load_saved_location') 'Bound Balmora seed was displaced by a COC setup script'
    Check-Route ($boundBalmora.manifest.route_start.save_sha256 -ceq (Get-OptimizedMWFrozenFile $boundBalmora.runtime.save_path).sha256 -and $boundBalmora.manifest.route_start.saved_cell -ceq "Balmora, Caius Cosades' House") 'Balmora starting-save identity/location binding not retained'
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Balmora starting-save binding accepted for the Seyda route'
    $balmoraLock.route_start.save_sha256=('0'*64);Write-OptimizedMWRouteJson (Join-Path $locked 'LOCK.json') $balmoraLock
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName balmora} 'Mismatched starting-save binding accepted'
    $balmoraLock.PSObject.Properties.Remove('route_start');Write-OptimizedMWRouteJson (Join-Path $locked 'LOCK.json') $balmoraLock
    $seydaSmoke=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing -SmokeRun
    Check-Route ([IO.File]::ReadAllText($seydaSmoke.manifest.setup_script.path) -ceq ('coc "Seyda Neen"'+"`n")) 'Multiword smoke cell name is not quoted as one console argument'
    Check-Route (Compare-OptimizedMWRouteProtectedInputs $before (Get-OptimizedMWRouteProtectedInputs $baseline)) 'Original profile changed during recording setup'
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName elsewhere} 'Unknown route accepted'
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing -SmokeDuration 0} 'Unbounded smoke duration accepted'
    $originalPolicyHash=$encounterPolicy.frozen_config_sha256;$encounterPolicy.frozen_config_sha256=('0'*64);Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Encounter policy for another frozen mod configuration accepted'
    $encounterPolicy.frozen_config_sha256=$originalPolicyHash
    $encounterPolicy.content_files=@();Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Encounter policy without content identities accepted'
    $encounterPolicy.content_files=@([ordered]@{path=$asset.path;bytes=$asset.bytes;sha256=('0'*64)});Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Encounter policy with changed source content accepted'
    $encounterPolicy.content_files=@($asset,$asset);Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Duplicate encounter source content identity accepted'
    $encounterPolicy.content_files=@($asset)
    $developmentPath=Join-Path $live 'Development.esp';[IO.File]::WriteAllText($developmentPath,'version one')
    $developmentFile=[ordered]@{content='Development.esp';path=$developmentPath}
    $encounterPolicy.mutable_development_content_files=@($developmentFile);Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    $mutableFirst=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing
    Check-Route ($mutableFirst.manifest.encounter_policy.mutable_development_content.Count -eq 1 -and $mutableFirst.manifest.encounter_policy.verified_content_files -eq 1) 'Mutable development entry weakened unrelated content binding'
    [IO.File]::WriteAllText($developmentPath,'version two has different bytes')
    $mutableSecond=New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing
    Check-Route ($mutableSecond.manifest.encounter_policy.mutable_development_content[0].sha256 -ceq (Get-OptimizedMWFrozenFile $developmentPath).sha256 -and $mutableSecond.manifest.encounter_policy.mutable_development_content[0].sha256 -cne $mutableFirst.manifest.encounter_policy.mutable_development_content[0].sha256) 'Changed development content blocked preparation or lost its observed identity'
    $encounterPolicy.mutable_development_content_files=@($developmentFile,$developmentFile);Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Duplicate mutable development source accepted'
    $encounterPolicy.mutable_development_content_files=@([ordered]@{content='fixture.dds';path=$asset.path});Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Mutable development entry bypassed an already bound file'
    $encounterPolicy.mutable_development_content_files=@([ordered]@{content='Wrong.esp';path=$developmentPath});Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Mutable development content filename mismatch accepted'
    $encounterPolicy.mutable_development_content_files=@();Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    foreach($invalidList in @(@(),@('rat','rat'),@("rat'"),@('rat','Scrib'),@($null))){
        $encounterPolicy.hostile_record_ids=$invalidList;Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
        Reject-Route {New-OptimizedMWRouteSession -GameDirectory $game -FrozenDirectory $locked -PackageDirectory $package -RouteName existing} 'Invalid, duplicate or unsafe encounter list accepted'
    }
    $encounterPolicy.hostile_record_ids=@('rat','kwama forager','scrib','nix-hound');Write-OptimizedMWRouteJson $encounterPolicyPath $encounterPolicy
    $unsafe=Join-Path $fixture 'unsafe.xml';[IO.File]::WriteAllText($unsafe,'<!DOCTYPE Controller [<!ENTITY ex SYSTEM "file:///C:/Windows/win.ini">]><Controller>&ex;</Controller>')
    Reject-Route {Protect-OptimizedMWRouteNativeBindings $unsafe} 'DTD in native input accepted'
    [IO.File]::WriteAllText($unsafe,'<Controller><Control name="1"><KeyBinder key="113"/></Control></Controller>')
    Reject-Route {Protect-OptimizedMWRouteNativeBindings $unsafe} 'Occupied native parking key accepted'
    $sessionId='0123456789abcdef0123456789abcdef';$log=Join-Path $fixture 'log.txt';$out=Join-Path $fixture 'route.jsonl'
    $records=@(
        @{schema=1;session=$sessionId;event='ready';seq=1;t=0},
        @{schema=1;session=$sessionId;event='start';seq=2;t=1;checkpoint_save=@{directory='Character';filename='route.omwsave'}},
        @{schema=1;session=$sessionId;event='control';seq=3;t=1.01;controls=@{movement=0;side_movement=0;yaw_change=0;pitch_change=0;run=$false;sneak=$false;jump=$false;use=0}},
        @{schema=1;session=$sessionId;event='sample';seq=4;t=1.05;f=1;control_sequence=3;simulation=0.05;wall_since_start=0.05;game=0.05;dt_real=0.05;position=@{x=1;y=2;z=3};rotation=@{x=0;y=0;z=0};camera=@{yaw=0;pitch=0;extra_yaw=0;extra_pitch=0;roll=0;fov=60};cell=@{name='Balmora';exterior=$true}},
        @{schema=1;session=$sessionId;event='checkpoint';seq=5;t=1.1},
        @{schema=1;session=$sessionId;event='sample';seq=6;t=1.15;f=2;control_sequence=3;simulation=0.15;wall_since_start=0.15;game=0.15;dt_real=0.05;position=@{x=2;y=3;z=4};rotation=@{x=0;y=0;z=0};camera=@{yaw=0;pitch=0;extra_yaw=0;extra_pitch=0;roll=0;fov=60};cell=@{name='Balmora';exterior=$true}},
        @{schema=1;session=$sessionId;event='finish';seq=7;t=2;expected_records=7;counts=@{ready=1;start=1;control=1;sample=2;checkpoint=1};flags=@{complete=$true;door_observer_ready=$true;policy_ready=$true;policy_qualified=$true};policy=@{qualified=$true;ready=$true;enabled=$true;failed=$false;combat_during_recording=0;pending_disable=0};private_camera_setup=@{source='private_setup_vanity_control_switch';vanity_disabled=$true;verified=$true};reason='user_finished'})
    function Write-RouteFixture {param([object[]]$Entries) [IO.File]::WriteAllLines($log,@($Entries | ForEach-Object {'[12:34:56] OMWROUTE|'+($_ | ConvertTo-Json -Depth 8 -Compress)}),[Text.UTF8Encoding]::new($false))}
    Write-RouteFixture $records;$summary=Export-OptimizedMWRouteLog $log $out $sessionId 0
    Check-Route $summary.complete 'Valid route fixture rejected'
    Check-Route ($summary.record_count -eq 7 -and $summary.events.sample -eq 2 -and $summary.route_file.sha256 -match '^[a-f0-9]{64}$') 'Route count or checksum absent'
    $records[6].policy.combat_during_recording=1;Write-RouteFixture $records;$combatSummary=Export-OptimizedMWRouteLog $log $out $sessionId 0
    Check-Route (-not $combatSummary.complete -and $combatSummary.errors -contains 'Combat/Pursue was observed during recording (events=1)' -and -not ($combatSummary.errors -match 'pending|unclosed')) 'Observed combat is accepted or misreported as pending suppression'
    $records[6].policy.combat_during_recording=0;$records[6].policy.pending_disable=1;Write-RouteFixture $records;$pendingSummary=Export-OptimizedMWRouteLog $log $out $sessionId 0
    Check-Route (-not $pendingSummary.complete -and $pendingSummary.errors -contains 'Actor suppressions still pending at export (actors=1)' -and -not ($pendingSummary.errors -match 'was observed')) 'Pending disable is accepted or misreported as observed combat'
    foreach($invalidCounter in @($null,-1,0.5,'0')){
        $records[6].policy.pending_disable=$invalidCounter;Write-RouteFixture $records;$invalidSummary=Export-OptimizedMWRouteLog $log $out $sessionId 0
        Check-Route (-not $invalidSummary.complete -and $invalidSummary.errors -contains 'Actor policy counter missing or invalid: pending_disable') 'Missing or invalid pending-disable counter accepted'
    }
    $records[6].policy.pending_disable=0
    Write-RouteFixture $records[0..5];Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Missing finish accepted'
    Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 1).complete) 'Game crash accepted'
    $records[3].position.x='NaN';Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Nonfinite sample accepted';$records[3].position.x=1
    $records[5].t=0.5;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Clock reversal accepted';$records[5].t=1.15
    $records[6].expected_records=8;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Finish count mismatch accepted';$records[6].expected_records=7
    $records[6].flags.complete=$false;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Lua incomplete flag accepted';$records[6].flags.complete=$true
    $records[6].flags.door_observer_ready=$false;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Missing door observer accepted';$records[6].flags.door_observer_ready=$true
    $records[4].event='combat';Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Combat route accepted';$records[4].event='checkpoint'
    $records[3].session='abcdef0123456789abcdef0123456789';Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Foreign session accepted';$records[3].session=$sessionId
    $records[3].control_sequence=5;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Pose joining a future/non-control record accepted';$records[3].control_sequence=3
    $records[6].flags.policy_qualified=$false;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Unqualified actor-policy flag accepted';$records[6].flags.policy_qualified=$true
    $records[6].flags.policy_ready='true';Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Non-boolean policy flag accepted';$records[6].flags.policy_ready=$true
    $savedPolicy=$records[6].policy;$records[6].Remove('policy');Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Missing actor-policy metadata accepted';$records[6].policy=$savedPolicy
    $records[6].policy.pending_disable=1;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Unclosed actor suppression work accepted';$records[6].policy.pending_disable=0
    foreach($badFlag in @('combat_invalid','reloaded','budget_exceeded','checkpoint_failed')){$records[6].flags[$badFlag]=$true;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) ('Failure flag accepted: '+$badFlag);$records[6].flags.Remove($badFlag)}
    $records[2].controls.Remove('run');Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Missing movement mode boolean accepted';$records[2].controls.run=$false
    $records[2].controls.use=1.5;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Noninteger native attack/use enum accepted';$records[2].controls.use=0
    $records[4]=@{schema=1;session=$sessionId;event='activation';seq=5;t=1.1;actual_request=$true;observed=$true;source='engine_onActivate_actual_target';target=@{id='fixture-ref';record_id='fixture-door'}}
    $records[6].counts.Remove('checkpoint');$records[6].counts.activation=1
    Write-RouteFixture $records;Check-Route (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete 'Observed actual activation rejected'
    $records[4].observed=$false;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Unknown actual activation accepted';$records[4].observed=$true
    $records[4].Remove('actual_request');Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Actual activation missing boolean qualification accepted'
    $records[4]=@{schema=1;session=$sessionId;event='trigger';seq=5;t=1.1;name='ActivateTargetSnapshot';actual_request=$false;observed=$false;hit=$false}
    $records[6].counts.Remove('activation');$records[6].counts.trigger=1
    Write-RouteFixture $records;Check-Route (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete 'Visual activation intent with no ray hit rejected'
    $records[6].flags.activation_target_unresolved=$true;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Unresolved actual activation failure flag accepted';$records[6].flags.Remove('activation_target_unresolved')
    $savedCamera=$records[6].private_camera_setup;$records[6].Remove('private_camera_setup');Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Missing private idle-camera verification accepted';$records[6].private_camera_setup=$savedCamera
    $records[6].private_camera_setup.vanity_disabled=$false;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Enabled idle Vanity accepted';$records[6].private_camera_setup.vanity_disabled=$true
    $records[6].private_camera_setup.verified='true';Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Nonboolean idle-camera verification accepted';$records[6].private_camera_setup.verified=$true
    $records[6].flags.vanity_setup_failed=$true;Write-RouteFixture $records;Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Idle-camera failure flag accepted';$records[6].flags.Remove('vanity_setup_failed')
    [IO.File]::WriteAllText($log,'OMWROUTE|{invalid}');Check-Route (-not (Export-OptimizedMWRouteLog $log $out $sessionId 0).complete) 'Malformed JSON accepted'
    foreach($expectedExit in @(0,23)){
        $child=Start-OptimizedMWRouteOwnedProcess -Executable (Join-Path $env:WINDIR 'System32\WindowsPowerShell\v1.0\powershell.exe') -Arguments @('-NoProfile','-NonInteractive','-Command',('"Start-Sleep -Milliseconds 200; exit '+$expectedExit+'"')) -WorkingDirectory $fixture -StandardOutput (Join-Path $fixture ('child-'+$expectedExit+'.stdout')) -StandardError (Join-Path $fixture ('child-'+$expectedExit+'.stderr'))
        if(-not $child.process.WaitForExit(30000)){throw 'Hermetic OS-exit child did not finish'}
        Check-Route ($null -ne $child.process.ExitCode -and $child.process.ExitCode -eq $expectedExit -and $child.handle -ne [IntPtr]::Zero) ('Actual child OS exit code was not retained: '+$expectedExit)
        $child.process.Dispose()
    }
    Write-RouteFixture $records;$summary=Export-OptimizedMWRouteLog $log $out $sessionId 0
    $summary.checkpoint_save | Add-Member -NotePropertyName source -NotePropertyValue 'private_menu_new_save_slot'
    $summary.checkpoint_save | Add-Member -NotePropertyName request -NotePropertyValue ($existing.id+':start')
    $summary.checkpoint_save | Add-Member -NotePropertyName description -NotePropertyValue ('OptimizedMW route existing '+$existing.id)
    [void][IO.Directory]::CreateDirectory((Join-Path $existing.runtime.user_data_directory 'saves/Character'))
    [IO.File]::WriteAllBytes((Join-Path $existing.runtime.user_data_directory 'saves/Character/route.omwsave'),[byte[]]@(8,9,10))
    $startCopy=Copy-OptimizedMWRouteStartSave $summary $existing
    Check-Route ($startCopy.source.sha256 -ceq $startCopy.route_copy.sha256) 'Starting checkpoint identity changed during copying'
    $retryOutput=Join-Path $fixture 'retry-output';[void][IO.Directory]::CreateDirectory($retryOutput)
    $retrySession=[pscustomobject]@{id=$existing.id;manifest=$existing.manifest;runtime=$existing.runtime;directory=$retryOutput}
    $summary.checkpoint_save.request=$existing.id+':start:2'
    $retryCopy=Copy-OptimizedMWRouteStartSave $summary $retrySession
    Check-Route ($retryCopy.source.sha256 -ceq $retryCopy.route_copy.sha256) 'Fresh authenticated checkpoint retry was rejected'
    foreach($badRequest in @($existing.id+':start:1',$existing.id+':start:02',$existing.id+':start:1000000',('b'*32)+':start:2')){
        $summary.checkpoint_save.request=$badRequest
        Reject-Route {Copy-OptimizedMWRouteStartSave $summary $retrySession} 'Noncanonical or foreign checkpoint retry accepted'
    }
    $summary.checkpoint_save.request=$existing.id+':start'
    $summary.checkpoint_save.filename='save.omwsave';Reject-Route {Copy-OptimizedMWRouteStartSave $summary $existing} 'Initial copied save accepted as a new checkpoint';$summary.checkpoint_save.filename='route.omwsave'
    $summary.checkpoint_save.source='forged';Reject-Route {Copy-OptimizedMWRouteStartSave $summary $existing} 'Unauthenticated starting checkpoint source accepted';$summary.checkpoint_save.source='private_menu_new_save_slot'
    $summary.checkpoint_save.directory='..';Reject-Route {Copy-OptimizedMWRouteStartSave $summary $existing} 'Traversal starting checkpoint accepted'
    Write-Host ("PASS: $checks route recorder checks; no game launched")
}finally{
    # This unique hermetic fixture alone is disposable. Verify the exact
    # resolved target remains under TEMP before the one-shell removal.
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\','/')+'\'
    if(-not $fixture.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or [IO.Path]::GetFileName($fixture) -notmatch '^Route recorder space [a-f0-9]{32}$'){throw 'Refusing unsafe fixture cleanup'}
    Remove-Item -LiteralPath $fixture -Recurse -Force
}
