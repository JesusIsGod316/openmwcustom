# Hermetic production selection/manifest tests; does not launch OpenMW or WPR.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchMatrix.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RunManifest.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'OptimizedMW_Test.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors | Out-String)}
$fn=$ast.Find({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Get-P9Mode'},$true)
. ([scriptblock]::Create($fn.Extent.Text))
$before=Get-Phase9EnvironmentSnapshot
$fixture=Join-Path ([IO.Path]::GetTempPath()) ('p9-hitch-matrix-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try{
    [IO.File]::WriteAllBytes((Join-Path $fixture 'openmw.exe'),[byte[]]@(1,2,3,4))
    $settings=Join-Path $fixture 'settings.cfg'
    [IO.File]::WriteAllText($settings,"[Fixture]`nkeep = unchanged`n")
    Copy-Item -LiteralPath $settings -Destination (Join-Path $fixture 'settings-before.cfg')
    $settingsHash=(Get-FileHash -LiteralPath $settings).Hash
    $seenNames=[Collections.Generic.HashSet[string]]::new()
    $cells=0
    foreach($mask in 0..15){
        foreach($io in @('CONTROL','I1','I2','I1I2')){
            $normal=Get-OptimizedMWHitchSelection $mask Normal $io
            $deep=Get-OptimizedMWHitchSelection $mask Deep $io
            if($normal.phase9_mode -ne '1' -or $deep.phase9_mode -ne '3'){throw 'Trace selection changes the reference foundation'}
            if((Get-Phase9IdentityHash $normal.features) -cne (Get-Phase9IdentityHash $deep.features)){throw 'Normal/deep feature vectors differ'}
            $featureHashes=@()
            foreach($selection in @($normal,$deep)){
                if(-not $seenNames.Add($selection.experiment)){throw 'Capture names collide'}
                # Start with deliberately contaminated inherited switches.
                foreach($key in @('UI_REFLOW','COMPOSITE_BATCH','ANIMATION_PRELOAD','TERRAIN_CURRENT_FIRST')){
                    Set-Item ('Env:OPENMW_HITCH_'+$key) '1'
                }
                Set-OptimizedMWHitchCombinationEnvironment $selection.features
                $actual=@($env:OPENMW_HITCH_UI_REFLOW,$env:OPENMW_HITCH_COMPOSITE_BATCH,
                    $env:OPENMW_HITCH_ANIMATION_PRELOAD,$env:OPENMW_HITCH_TERRAIN_CURRENT_FIRST)
                foreach($bit in 0..3){if($actual[$bit] -cne [string][int][bool]($mask -band (1 -shl $bit))){throw 'Combination inherited an unselected feature'}}
                $mode=Get-P9Mode $selection.phase9_mode;$mode.HitchTrialRevision=2
                $candidate=Get-Phase9Candidate $selection.candidate
                Set-Phase9CandidateEnvironment $candidate $mode
                $manifest=New-Phase9RunManifest -GameDir $fixture -ProfileDir $fixture -Mode $mode -Candidate $candidate `
                    -SettingsPath $settings -HitchCombination $selection.features
                Save-Phase9RunManifest $manifest $fixture
                Complete-Phase9RunManifest $manifest $fixture
                $saved=Get-Content -Raw -LiteralPath (Join-Path $fixture 'RUN-MANIFEST.json') | ConvertFrom-Json
                if($saved.gameplay_features.hitch_mechanisms.mask -ne $mask -or
                    $saved.trace_level -cne $selection.trace_level -or $saved.trial_revision -cne 'hitch-reduction-v2' -or
                    $saved.families.deep.requested -ne ($selection.trace_level -eq 'DEEP') -or
                    $saved.activation_evidence.state -cne 'unverified'){throw 'Selection/depth/qualification lost during packaging'}
                $featureHashes+=$saved.gameplay_features_sha256
                ++$cells
            }
            if($featureHashes[0] -cne $featureHashes[1]){throw 'Diagnostic depth changes the gameplay hash'}
        }
        $diagnostic=Get-OptimizedMWHitchSelection $mask Deep D1
        if($diagnostic.phase9_mode -ne '3'){throw 'D1 diagnostic mode lost'}
    }
    foreach($mask in @(-1,16)){
        $rejected=$false;try{$null=Get-OptimizedMWHitchCombination $mask}catch{$rejected=$true}
        if(-not $rejected){throw 'Invalid combination accepted'}
    }
    $rejected=$false;try{$null=Get-OptimizedMWHitchSelection 0 Normal D1}catch{$rejected=$true}
    if(-not $rejected){throw 'Diagnostic-only D1 accepted as Normal'}
    if((Get-FileHash -LiteralPath $settings).Hash -cne $settingsHash){throw 'Selection changes user settings'}
    # Exercise the actual interactive path, including invalid input and an explicit deep I1 trial.
    $script:matrixAnswers=[Collections.Generic.Queue[string]]::new()
    foreach($answer in @('16','7','Q','D','I1')){$script:matrixAnswers.Enqueue($answer)}
    function Read-Host {param([string]$Prompt) return $script:matrixAnswers.Dequeue()}
    $interactive=Read-OptimizedMWHitchSelection
    Remove-Item Function:Read-Host
    if($interactive.features.mask -ne 7 -or $interactive.trace_level -cne 'DEEP' -or $interactive.candidate -cne 'I1' -or
        $script:matrixAnswers.Count -ne 0){throw 'Interactive selection lost explicit trace depth'}
    Write-Host "PASS: $cells Normal/Deep/IO selection and manifest cells, 16 D1 cells, invalid inputs, interactive depth and restoration."
}finally{
    if(-not (Restore-Phase9Environment $before)){throw 'Matrix tests failed to restore environment'}
    $resolved=[IO.Path]::GetFullPath($fixture)
    $tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
    if(-not $resolved.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $resolved) -notlike 'p9-hitch-matrix-*'){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
