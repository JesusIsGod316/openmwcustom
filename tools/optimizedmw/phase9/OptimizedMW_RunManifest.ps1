# Launcher/post-run only. Hashes describe files on disk; they do not prove loaded-module identity.
function Get-Phase9Candidate {
    param([ValidateSet('CONTROL','I1','I2','I1I2','D1')][string]$Name='CONTROL',
        [ValidateRange(0,33554432)][long]$BufferBudgetBytes=33554432)
    return [pscustomobject][ordered]@{name=$Name.ToUpperInvariant();
        i1=($Name -in @('I1','I1I2'));i2=($Name -in @('I2','I1I2'));d1=($Name -eq 'D1');
        refill_bytes=$(if($Name -in @('I1','I1I2')){65536}else{8192});buffer_budget_bytes=$BufferBudgetBytes}
}

function Set-Phase9CandidateEnvironment {
    param([Parameter(Mandatory=$true)][object]$Candidate,[Parameter(Mandatory=$true)][object]$Mode)
    if($Candidate.d1 -and $Mode.Trace -ne '1'){throw 'D1 requires a deep trace mode; use -Mode 3 -Candidate D1'}
    $env:OPENMW_P9_FILE_REFILL_BYTES=[string]$Candidate.refill_bytes
    $env:OPENMW_P9_FILE_RESIDENT_SEEK=if($Candidate.i2){'1'}else{'0'}
    $env:OPENMW_P9_FILE_BUFFER_BUDGET_BYTES=[string]$Candidate.buffer_budget_bytes
    $env:OPENMW_P9_CATALOG_OPTIMIZED=if($Candidate.d1){'1'}else{'0'}
    $env:OPENMW_P9_COMPILE_ATTRIBUTION=if($Mode.Trace -eq '1'){'1'}else{'0'}
    $env:OPENMW_P9_FILE_POLICY_ACK='1'
}

function Set-Phase9HitchRecorderEnvironment {
    param([Parameter(Mandatory=$true)][object]$Mode,
        [ValidateSet('stream','deferred')][string]$Transport='stream')
    if($Mode.Trace -eq '1'){$env:OPENMW_P9_HITCH_TRANSPORT=$Transport}
    else{Remove-Item Env:OPENMW_P9_HITCH_TRANSPORT -ErrorAction SilentlyContinue}
}

function Get-Phase9IdentityHash {
    param([Parameter(Mandatory=$true)][AllowEmptyCollection()][object]$Value)
    $json=ConvertTo-Json -InputObject $Value -Depth 12 -Compress
    $sha=[Security.Cryptography.SHA256]::Create()
    try{return [BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($json))).Replace('-','').ToLowerInvariant()}
    finally{$sha.Dispose()}
}

function Get-Phase9FileIdentity {
    param([Parameter(Mandatory=$true)][string]$Path,[string]$Name)
    $absolute=[IO.Path]::GetFullPath($Path)
    if(-not $Name){$Name=Split-Path -Leaf $absolute}
    if(-not [IO.File]::Exists($absolute)){return [pscustomobject][ordered]@{name=$Name;path=$absolute;present=$false;bytes=$null;sha256=$null}}
    $file=Get-Item -LiteralPath $absolute
    if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Manifest identity refuses a reparse-point file: $absolute"}
    return [pscustomobject][ordered]@{name=$Name;path=$absolute;present=$true;bytes=$file.Length;
        sha256=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant()}
}

function Get-Phase9EnvironmentSnapshot {
    $values=[ordered]@{}
    foreach($entry in (Get-ChildItem Env: | Where-Object {$_.Name -like 'OPENMW_*'} | Sort-Object Name)){$values[$entry.Name]=[string]$entry.Value}
    return $values
}

function Restore-Phase9Environment {
    param([Parameter(Mandatory=$true)][System.Collections.IDictionary]$Snapshot)
    Get-ChildItem Env: | Where-Object {$_.Name -like 'OPENMW_*'} | ForEach-Object {Remove-Item ('Env:'+$_.Name)}
    foreach($entry in $Snapshot.GetEnumerator()){Set-Item ('Env:'+$entry.Key) ([string]$entry.Value)}
    $restored=Get-Phase9EnvironmentSnapshot
    if($restored.Count -ne $Snapshot.Count){return $false}
    foreach($key in $Snapshot.Keys){if(-not $restored.Contains($key) -or [string]$restored[$key] -cne [string]$Snapshot[$key]){return $false}}
    return $true
}

function Save-Phase9RunManifest {
    param([Parameter(Mandatory=$true)][object]$Manifest,[Parameter(Mandatory=$true)][string]$ProfileDir)
    # The profile is unique and owned by this run. Never replace an older run's ZIP.
    $Manifest | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $ProfileDir 'RUN-MANIFEST.json') -Encoding UTF8
}

function New-Phase9RunManifest {
    param([Parameter(Mandatory=$true)][string]$GameDir,[Parameter(Mandatory=$true)][string]$ProfileDir,
        [Parameter(Mandatory=$true)][object]$Mode,[Parameter(Mandatory=$true)][object]$Candidate,
        [Parameter(Mandatory=$true)][string]$SettingsPath,[string]$OpenmwCfgPath,[string]$SavePath,
        [string]$RouteLabel='UNSPECIFIED',[string]$FrameCapLabel='UNSPECIFIED',
        [ValidateSet('unclassified','fresh-process-first-visit','same-process-revisit','later-fresh-launch')][string]$Cohort='unclassified',
        [object]$HitchCombination=$null)
    $runtime=[Collections.Generic.List[object]]::new()
    foreach($file in (Get-ChildItem -LiteralPath $GameDir -Filter '*.dll' -File | Sort-Object Name)){
        $runtime.Add((Get-Phase9FileIdentity $file.FullName $file.Name))
    }
    foreach($directory in (Get-ChildItem -LiteralPath $GameDir -Directory -Filter 'osgPlugins-*' | Sort-Object Name)){
        if(($directory.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Manifest refuses a reparse-point plugin directory'}
        foreach($file in (Get-ChildItem -LiteralPath $directory.FullName -Filter '*.dll' -File | Sort-Object Name)){
            $runtime.Add((Get-Phase9FileIdentity $file.FullName ($directory.Name+'/'+$file.Name)))
        }
    }
    $scripts=[Collections.Generic.List[object]]::new()
    foreach($file in (Get-ChildItem -LiteralPath $GameDir -File | Where-Object {$_.Extension -in @('.ps1','.bat','.wprp')} | Sort-Object Name)){
        $scripts.Add((Get-Phase9FileIdentity $file.FullName $file.Name))
    }
    $build=Get-Phase9FileIdentity (Join-Path $GameDir 'CI-ID.txt')
    $buildText=if($build.present){[IO.File]::ReadAllText($build.path)}else{$null}
    $commit=$null
    if($buildText -match '(?m)^Commit ([a-fA-F0-9]{40})\r?$'){$commit=$matches[1].ToLowerInvariant()}
    $configuration=[ordered]@{settings_before=Get-Phase9FileIdentity (Join-Path $ProfileDir 'settings-before.cfg');
        settings_applied=Get-Phase9FileIdentity $SettingsPath;openmw_cfg=$null;save=$null;
        route_label=$RouteLabel;cohort=$Cohort;frame_cap_label=$FrameCapLabel;
        frame_cap_identity_status='caller_declared_not_automatically_verified';
        asset_identity='UNVERIFIED: configuration identifies paths, not complete asset contents';
        save_identity_status='UNSPECIFIED';cache_state='OS/driver caches uncontrolled; fresh launch does not prove cold caches'}
    if($OpenmwCfgPath){$configuration.openmw_cfg=Get-Phase9FileIdentity $OpenmwCfgPath}
    if($SavePath){$configuration.save=Get-Phase9FileIdentity $SavePath;$configuration.save_identity_status=if($configuration.save.present){'caller_declared_file_hashed_not_actual_load_acknowledged'}else{'declared_file_missing'}}
    $gameplayFeatures=[ordered]@{io_i1=[bool]$Candidate.i1;io_i2=[bool]$Candidate.i2;
        refill_bytes=$Candidate.refill_bytes;buffer_budget_bytes=$Candidate.buffer_budget_bytes;
        dynamic_stream=($Mode.Stream -eq '1');temporal_inputs=($Mode.Temporal -eq '1');
        temporal_ownership=($Mode.Ownership -eq '1');composite_preparation=($Mode.Composite -eq '1');
        dynamic_motion=($Mode.DynamicMotion -eq '1');motion_view=($Mode.View -eq '1');
        composite_progress=($env:OPENMW_HITCH_COMPOSITE_PROGRESS -eq '1');
        composite_cpu_mips=($env:OPENMW_HITCH_COMPOSITE_CPU_MIPS -eq '1');
        hitch_mechanisms=$HitchCombination}
    $families=[ordered]@{
        basic=[ordered]@{requested=$true;coverage='existing basic frame/render/streaming measurements; not telemetry-free'};
        benchmark_context=[ordered]@{requested=[bool]$env:OPENMW_P9_BENCHMARK_CONTEXT_FILE;revision=1;
            channels=@('benchmark-context.csv','benchmark-context.csv.snapshots.csv','benchmark-context.csv.capture-status.txt');
            coverage='Observed main-entry/after-state/main-exit pause/menu/visibility/game-state flags and loading viewer advance. Low-rate camera/player/weather/time/cell snapshots; no automatic exact user-route attestation.'};
        deep=[ordered]@{requested=($Mode.Trace -eq '1');coverage='retained CPU spans/catalog/selected GL; driver/GPU coverage separately qualified'};
        temporal=[ordered]@{requested=($Mode.Temporal -eq '1');ownership_requested=($Mode.Ownership -eq '1');dynamic_motion_requested=($Mode.DynamicMotion -eq '1')};
        composite=[ordered]@{requested=($Mode.Composite -eq '1');
            progress_requested=($env:OPENMW_HITCH_COMPOSITE_PROGRESS -eq '1');
            cpu_mips_requested=($env:OPENMW_HITCH_COMPOSITE_CPU_MIPS -eq '1');
            coverage='Requested startup switches; exact readiness, consumption and mip capability/fallback require engine evidence'};
        io_candidates=[ordered]@{i1_requested=$Candidate.i1;i2_requested=$Candidate.i2;
            requested_refill_bytes=$Candidate.refill_bytes;budget_limit_bytes=$Candidate.buffer_budget_bytes};
        catalog_candidate=[ordered]@{d1_requested=$Candidate.d1};
        gpu_query_accounting=[ordered]@{requested=($Mode.Trace -eq '1' -or $Mode.Temporal -eq '1');revision=1;
            deferred_collection_requested=($env:OPENMW_P9_GPU_QUERY_DEFER -eq '1');
            policy=$(if($env:OPENMW_P9_GPU_QUERY_DEFER -eq '1'){'deferred_begin_min2_backoff8'}else{'legacy_begin_end'});
            channels=@('p9-gpu-passes.csv','p9-gpu-passes.csv.queries.csv','p9-gpu-passes.csv.pending.csv','p9-gpu-passes.csv.query-status.txt');
            coverage='Final query accounting after producer join; pending/active are missing results, not zero GPU time. Timestamp intervals include submission gaps/idle; no GPU-active proof.'};
        hitch_mechanisms=[ordered]@{requested=$HitchCombination;
            coverage='Independent startup switches; resource denial, invalidation and demand fallback remain possible. Selection alone is not execution proof.'};
        hitch_v2_attribution=[ordered]@{requested=($Mode.Trace -eq '1');channel='p9-hitch-spans.csv';
            coverage='Additional nested UI, animation preparation, terrain publication and composite pass/restore spans. Existing threshold and loss qualification apply; child wall times are not additive.'};
        hitch_diagnostics6=[ordered]@{requested=($Mode.Trace -eq '1');revision=2;channel='p9-hitch-spans.csv';
            offline_report='HITCH-SPIKE-REPORT.json';
            coverage='Exact engine-wall spike joins; preparation/cache retrieval lifecycle; model construction; physics/navigation; work queues/cache retirement; allocator service and explicit Lua GC. Automatic VM GC phases remain unavailable without a VM sampler; completeness and unknown provenance are reported separately.'};
        hitch_recorder=[ordered]@{requested=($Mode.Trace -eq '1');transport_schema=1;row_schema=2;
            requested_transport=$env:OPENMW_P9_HITCH_TRANSPORT;
            requested_storage_slots=$env:OPENMW_P9_HITCH_CAPACITY;
            coverage='Owned numeric row transport; written row count can exceed queue capacity. Actual drain, loss, output and join qualification are independent of the requested transport.'};
        diagnostic_observer=[ordered]@{requested=($Mode.Trace -eq '1');revision=1;
            local_aggregation_requested=($env:OPENMW_P9_OBSERVER_LOCAL -eq '1');
            channels=@('p9-hitch-spans.csv.observer.csv','p9-hitch-spans.csv.observer-status.txt');
            coverage='Fixed thread-local observer counters and measured observer regions plus writer batches. Inclusive diagnostic exposures can overlap; measuring them has its own cost and no estimated cost is subtracted from frame times.'};
        lifecycle_final_status=[ordered]@{requested=($Mode.Trace -eq '1');revision=1;
            channel='p9-hitch-spans.csv.lifecycle-status.txt';
            coverage='Final registry snapshot outside the row queue. Registry status cannot establish missing lifetime-event joins or first GPU use.'};
        lua_event_decode=[ordered]@{requested=($Mode.Trace -eq '1');revision=1;channel='p9-hitch-spans.csv';
            coverage='Existing decode walk shape, encoded bytes, outcome and event/container/handler ownership; no payload contents or extra content scan. Exact receipt joins require a complete CPU timeline.'};
        io_policy_acknowledgement=[ordered]@{requested=$true;coverage='one new startup policy log line, including basic CONTROL; not per-stream effective allocation proof'};
        compile_attribution=[ordered]@{requested=($Mode.Trace -eq '1');channel='compile-attribution.csv';schema=1;
            coverage='additional P0 ownership/selected-operation metadata; differs from old deep reference; original p4-compile.csv unchanged'};
        windows=[ordered]@{requested='external recorder link pending';coverage='not established by launcher or ZIP presence'}
    }
    $familyHashes=[ordered]@{}
    foreach($name in $families.Keys){$familyHashes[$name]=Get-Phase9IdentityHash $families[$name]}
    $builtinScripts=[Collections.Generic.List[object]]::new()
    foreach($relative in @('resources/vfs/builtin.omwscripts','resources/vfs/scripts/omw/camera/camera.lua',
        'resources/vfs/scripts/omw/camera/settings.lua','resources/vfs/scripts/omw/settings/common.lua')){
        $builtinScripts.Add((Get-Phase9FileIdentity (Join-Path $GameDir $relative) $relative))
    }
    return [pscustomobject][ordered]@{schema=1;trial_revision=$(if($Mode.FocusedRendererTrialRevision -eq 1){'terrain-first-use-v1'}elseif($Mode.HitchTrialRevision -eq 3){'hitch-reduction-v3'}elseif($Mode.HitchTrialRevision -eq 2){'hitch-reduction-v2'}else{'hitch-reduction-v1'});
        profile_name=(Split-Path -Leaf $ProfileDir);created_utc=[DateTime]::UtcNow.ToString('o');
        phase9_mode=$Mode;candidate=$Candidate;candidate_sha256=(Get-Phase9IdentityHash $Candidate);
        trace_level=$(if($Mode.Trace -eq '1'){'DEEP'}else{'NORMAL'});
        gameplay_features=$gameplayFeatures;gameplay_features_sha256=(Get-Phase9IdentityHash $gameplayFeatures);
        executable=(Get-Phase9FileIdentity (Join-Path $GameDir 'openmw.exe'));
        build_identity=$build;build_identity_text=$buildText;engine_source_commit_from_build_identity=$commit;
        runtime_dlls=$runtime.ToArray();runtime_dll_inventory_sha256=(Get-Phase9IdentityHash -Value @($runtime.ToArray()));
        scripts=$scripts.ToArray();script_inventory_sha256=(Get-Phase9IdentityHash -Value @($scripts.ToArray()));
        content_lua=(Get-Phase9FileIdentity (Join-Path $GameDir 'resources/lua_libs/content.lua'));
        builtin_script_files=$builtinScripts.ToArray();builtin_script_inventory_sha256=(Get-Phase9IdentityHash -Value @($builtinScripts.ToArray()));
        configuration=$configuration;
        comparison_protocol=[ordered]@{requested=($Mode.FocusedRendererTrialRevision -eq 1);
            first_block=@('CONTROL','TERRAIN-MIPS','TERRAIN-MIPS','CONTROL');first_block_trace_level='NORMAL';
            fixed_view_seconds=45;first_person_requested=$true;same_save_route_cap_requested=$true;
            observation_status='instructions_only_not_route_view_or_actual_save_load_attestation'};
        launcher_applied_openmw_environment=(Get-Phase9EnvironmentSnapshot);
        families=$families;family_sha256=$familyHashes;
        activation_evidence=[ordered]@{state='pending';note='Requested/applied switches do not establish runtime activation; inspect engine evidence and fallbacks'};
        process=[ordered]@{pid=$null;creation_utc=$null;creation_ticks=$null;creation_source='System.Diagnostics.Process.StartTime native';launch_utc=$null};
        completion=[ordered]@{state='prepared';exit_code=$null;launcher_error=$null;settings_restore_verified=$null;environment_restore_verified=$null;restored_settings_sha256=$null};
        clocks=[ordered]@{state='unverified';engine_mapping=$null};
        qualifications=[ordered]@{};identity_limit='On-disk DLL hashes do not prove module loading. No game-save/asset match or capture completeness is inferred from package identity.'}
}

function Complete-Phase9RunManifest {
    param([Parameter(Mandatory=$true)][object]$Manifest,[Parameter(Mandatory=$true)][string]$ProfileDir)
    $qualification=[ordered]@{}
    foreach($name in @('PROFILE-CAPTURE.json','ROOT-CAUSE-CAPTURE.json','HITCH-CAPTURE.json','HITCH-DIAGNOSTICS6-CAPTURE.json','TEMPORAL-CAPTURE.json','WRITER-CAPTURE.json','COMPILE-ATTRIBUTION-CAPTURE.json')){
        $path=Join-Path $ProfileDir $name
        if([IO.File]::Exists($path)){$qualification[$name]=Get-Phase9FileIdentity $path $name}
    }
    $Manifest.qualifications=$qualification
    if($Manifest.PSObject.Properties['families']){
        $linkPath=Join-Path $ProfileDir 'SYSTEM-TRACE-LINK.json'
        $linkPresent=[IO.File]::Exists($linkPath)
        $Manifest.families.windows=[ordered]@{linked_recorder_present=$linkPresent;
            link_identity=$(if($linkPresent){Get-Phase9FileIdentity $linkPath}else{$null});
            coverage='External scheduling/GPU capture completeness is separately qualified; absent link does not prove no external recorder activity'}
        foreach($name in $Manifest.families.Keys){$Manifest.family_sha256[$name]=Get-Phase9IdentityHash $Manifest.families[$name]}
    }
    $Manifest.activation_evidence=[ordered]@{state='unverified';note='Requested/applied switches and startup acknowledgement do not establish every per-stream allocation or observation completeness'}
    $logPath=Join-Path $ProfileDir 'openmw.log'
    if([IO.File]::Exists($logPath)){
        $acknowledgements=[Collections.Generic.List[object]]::new()
        foreach($line in [IO.File]::ReadLines($logPath)){
            if($line -match '(?:^|\s)P9 file policy: refill_bytes=(8192|65536) resident_seek=([01]) budget_bytes=(\d+)\s*$'){
                $budget=0L
                if([long]::TryParse($matches[3],[ref]$budget) -and $budget -ge 0 -and $budget -le 33554432){
                    $acknowledgements.Add([pscustomobject]@{refill_bytes=[long]$matches[1];resident_seek=[int]$matches[2];budget_bytes=$budget})
                }
            }
        }
        if($acknowledgements.Count -gt 0){
            $ack=$acknowledgements[0]
            $Manifest.activation_evidence['io_startup_policy']=[ordered]@{acknowledgement_count=$acknowledgements.Count;
                reported_requested_policy=$ack;selection_consistent=($acknowledgements.Count -eq 1 -and
                    $ack.refill_bytes -eq $Manifest.candidate.refill_bytes -and
                    $ack.resident_seek -eq [int]$Manifest.candidate.i2 -and
                    $ack.budget_bytes -eq $Manifest.candidate.buffer_budget_bytes);
                per_stream_effective_allocation_proved=$false;log_identity=(Get-Phase9FileIdentity $logPath);
                note='Retained startup requested-policy acknowledgement; does not prove that every stream received a candidate buffer'}
        }
    }
    $hitchPath=Join-Path $ProfileDir 'HITCH-CAPTURE.json'
    if([IO.File]::Exists($hitchPath)){
        $hitch=Get-Content -Raw -LiteralPath $hitchPath | ConvertFrom-Json
        if($Manifest.PSObject.Properties['clocks'] -and $hitch.clock_mapping){
            $Manifest.clocks.engine_mapping=$hitch.clock_mapping
            $Manifest.clocks.state='retained_mapping_not_independent_ETW_qualification'
        }
        if($hitch.analysis -and $hitch.analysis.logical_io -and $hitch.analysis.logical_io.candidate_metadata_available){
            $io=$hitch.analysis.logical_io
            $selectionConsistent=($io.policy_streams -gt 0 -and
                $io.refill_requested_streams -eq $(if($Manifest.candidate.i1){$io.policy_streams}else{0}) -and
                $io.resident_seek_requested_streams -eq $(if($Manifest.candidate.i2){$io.policy_streams}else{0}))
            $Manifest.activation_evidence['io']=[ordered]@{selection_consistent=$selectionConsistent;
                complete_candidate_stream_totals=($hitch.valid_span_capture -eq $true -and $io.complete_candidate_stream_totals);
                policy_streams=$io.policy_streams;refill_requested_streams=$io.refill_requested_streams;
                refill_effective_streams=$io.refill_effective_streams;refill_fallback_streams=$io.refill_fallback_streams;
                resident_seek_requested_streams=$io.resident_seek_requested_streams;
                resident_seek_effective_streams=$io.resident_seek_effective_streams;resident_seek_fallback_streams=$io.resident_seek_fallback_streams;
                buffer_cap_hits=$io.buffer_cap_hits;buffer_allocation_failures=$io.buffer_allocation_failures;
                observed_budget_peak_bytes_max=$io.observed_budget_peak_bytes_max;
                observed_budget_in_use_bytes_max=$io.observed_budget_in_use_bytes_max;
                observed_active_candidate_streams_max=$io.observed_active_candidate_streams_max;
                observed_active_candidate_buffers_max=$io.observed_active_candidate_buffers_max;
                status_identity=(Get-Phase9FileIdentity $hitchPath);
                note='Counts cover retained policy rows. Completeness/loss and actual per-stream fallback are separately qualified.'}
        }
    }
    $catalogPath=Join-Path $ProfileDir 'p9-draw-phases.csv.status.txt'
    if([IO.File]::Exists($catalogPath)){
        $text=[IO.File]::ReadAllText($catalogPath)
        if($text -match '(?m)^resource_catalog_algorithm_requested=(legacy|ordered_merge)\r?$'){
            $algorithm=$matches[1]
            $fallback=$null
            if($text -match '(?m)^resource_catalog_candidate_scratch_allocation_failures=(\d+)\r?$'){$fallback=[long]$matches[1]}
            $Manifest.activation_evidence['catalog']=[ordered]@{reported_requested_algorithm=$algorithm;
                scratch_allocation_failures=$fallback;status_identity=(Get-Phase9FileIdentity $catalogPath);
                selection_consistent=($algorithm -eq $(if($Manifest.candidate.d1){'ordered_merge'}else{'legacy'}));
                optimized_no_reported_scratch_fallback=($algorithm -eq 'ordered_merge' -and $null -ne $fallback -and $fallback -eq 0);
                note='Algorithm acknowledgement/fallback counters are separate from catalog observation completeness'}
        }
    }
    Save-Phase9RunManifest $Manifest $ProfileDir
}
