# Focused hermetic profile tests. No actual user profile, game, native build or UI automation.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_EditSettings.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_NumericMenu.ps1')
$script:checks=0
function Assert-Editor {
    param([bool]$Condition,[string]$Message)
    $script:checks++;if(-not $Condition){throw $Message}
}
function Assert-EditorReject {
    param([scriptblock]$Operation,[string]$Message)
    $rejected=$false;try{& $Operation | Out-Null}catch{$rejected=$true}
    Assert-Editor $rejected $Message
}
function Read-Host {
    param([string]$Prompt)
    if(-not $script:answers.Count){throw ('Fixture exhausted before '+$Prompt)}
    return $script:answers.Dequeue()
}
function Set-EditorAnswers {
    param([string[]]$Answers)
    $script:answers=[Collections.Generic.Queue[string]]::new()
    foreach($answer in $Answers){$script:answers.Enqueue($answer)}
}
$temporaryRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$fixture=Join-Path $temporaryRoot ('p9-settings-editor-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
try{
    $schema=Get-OptimizedMWSettingsSchema
    Assert-Editor ($schema.settings.Count -ge 202) 'Individual schema omitted existing configuration controls'
    $ids=@($schema.settings | ForEach-Object {$_.section+"`n"+$_.key})
    Assert-Editor (@($ids|Select-Object -Unique).Count -eq $ids.Count) 'Schema duplicates canonical keys'
    foreach($definition in $schema.settings){
        $canonical=Resolve-OptimizedMWSettingName $definition.section $definition.key
        Assert-Editor ($canonical.Section -ceq $definition.section -and $canonical.Key -ceq $definition.key) 'Schema retains a legacy alias'
        Assert-Editor ($definition.apply_class -ceq 'RestartRequired') 'Host editor advertised a live cached control'
        $null=ConvertTo-OptimizedMWSettingsValue $definition $definition.default
    }
    $roundtrip=Get-OptimizedMWSettingsSchemaJson | ConvertFrom-Json
    Assert-Editor ($roundtrip.settings.Count -eq $schema.settings.Count) 'Shared schema export lost controls'
    $defaultsPath=Join-Path $PSScriptRoot '../../../files/settings-default.cfg'
    if([IO.File]::Exists($defaultsPath)){
        $engine=Read-OptimizedMWSettingsValues ([IO.File]::ReadAllText($defaultsPath)) $schema
        foreach($definition in $schema.settings){
            Assert-Editor ($engine[$definition.section+"`n"+$definition.key] -ceq $definition.default) ('Schema default drift: '+$definition.key)
        }
    }
    $profile=Join-Path $fixture 'Preferences.cfg'
    $unicode='caf'+[char]0x00e9
    $original="[V3]`r`n# alias comment`r`nv3.9 frontload mode = 2`r`n[Cells]`r`n# canonical wins`r`ninitial multi-view preload mode = 1`r`n[Other Mod]`r`nprivate = untouched UTF8 $unicode`r`nduplicate = first`r`nduplicate = second`r`n"
    [IO.File]::WriteAllText($profile,$original,[Text.UTF8Encoding]::new($true))
    $originalBytes=[IO.File]::ReadAllBytes($profile)
    $session=New-OptimizedMWSettingsSession $profile
    $frontload=Get-OptimizedMWSettingsDefinition $schema 'V3' 'v3.9 frontload mode'
    Assert-Editor ((Get-OptimizedMWSettingsSessionValue $session $frontload) -ceq '1') 'Canonical setting did not beat its legacy alias'
    Set-OptimizedMWSettingsPendingValue $session 'V3' 'v3.9 frontload mode' '0'
    Assert-Editor ((Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($profile))) -ceq (Get-OptimizedMWSettingsBytesHash $originalBytes)) 'Pending edit wrote before Apply'
    $row=@(Get-OptimizedMWSettingsRows $session Benchmark | Where-Object {$_.section -ceq 'Cells' -and $_.key -ceq 'initial multi-view preload mode'})[0]
    Assert-Editor ($row.saved_value -ceq '1' -and $row.pending_value -ceq '0' -and $row.application -ceq 'SavedCustomIgnoredByBenchmark' -and $null -eq $row.effective_value) 'Editor conflated saved pending and effective settings'
    Close-OptimizedMWSettingsSession $session
    Assert-Editor ((Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($profile))) -ceq (Get-OptimizedMWSettingsBytesHash $originalBytes)) 'Cancelled edit altered profile'
    $session=New-OptimizedMWSettingsSession $profile
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'initial multi-view preload mode' 0
    $result=Save-OptimizedMWSettingsSession $session
    $written=[IO.File]::ReadAllText($profile)
    Assert-Editor ($result.saved -and $result.apply_class -ceq 'RestartRequired' -and $result.effective_state -ceq 'NotObserved') 'Apply receipt claimed live application'
    Assert-Editor ($written -notmatch 'v3.9 frontload mode' -and @([regex]::Matches($written,'initial multi-view preload mode = 0')).Count -eq 1) 'Edited aliases or duplicate canonical key survived'
    Assert-Editor ($written.Contains("# alias comment`r`n") -and $written.Contains("private = untouched UTF8 $unicode`r`nduplicate = first`r`nduplicate = second`r`n")) 'Unknown keys comments Unicode or line endings changed'
    Assert-Editor (([IO.File]::ReadAllBytes($profile))[0] -eq 239) 'Profile BOM changed'
    $snapshot=Test-OptimizedMWSettingsEffectiveSnapshot $session $profile
    Assert-Editor ($snapshot.qualified -and $snapshot.scope -match 'no running engine') 'Exact file readback was not qualified or overclaimed runtime application'
    [IO.File]::WriteAllText((Join-Path $fixture 'effective.cfg'),"[Cells]`ninitial multi-view preload mode = 2`n")
    $snapshot=Test-OptimizedMWSettingsEffectiveSnapshot $session (Join-Path $fixture 'effective.cfg')
    Assert-Editor (-not $snapshot.qualified -and $snapshot.mismatches.Count -eq 1) 'Requested/effective mismatch passed qualification'
    $snapshot=Test-OptimizedMWSettingsEffectiveSnapshot $session $profile Benchmark
    Assert-Editor (-not $snapshot.qualified -and $snapshot.status -ceq 'NotComparable_CustomIgnoredByBenchmark') 'Benchmark incorrectly consumed saved Custom settings'
    foreach($invalid in @('4','-1','1.5','1; exit','NaN','Infinity')){
        Assert-EditorReject {ConvertTo-OptimizedMWSettingsValue $frontload $invalid} ('Accepted invalid frontload '+$invalid)
    }
    $scale=Get-OptimizedMWSettingsDefinition $schema 'Video' 'render scale'
    foreach($invalid in @('0.49','1.01','NaN','1e309','0,75','0.5`nunsafe')){
        Assert-EditorReject {ConvertTo-OptimizedMWSettingsValue $scale $invalid} ('Accepted invalid scale '+$invalid)
    }
    $upscaler=Get-OptimizedMWSettingsDefinition $schema 'Video' 'upscaler'
    Assert-EditorReject {ConvertTo-OptimizedMWSettingsValue $upscaler 'dlss'} 'Exposed unimplemented playable DLSS option'
    $session=New-OptimizedMWSettingsSession $profile
    Set-OptimizedMWSettingsPendingValue $session 'Video' 'upscaler' 'nis'
    Set-OptimizedMWSettingsPendingValue $session 'Video' 'render scale' 1
    $sharp=@(Get-OptimizedMWSettingsRows $session | Where-Object {$_.key -ceq 'upscaler sharpness'})[0]
    Assert-Editor (-not $sharp.available -and $sharp.reason -match 'no full-resolution') 'Full-resolution NIS sharpening falsely advertised'
    Set-OptimizedMWSettingsPendingValue $session 'Video' 'render scale' 0.75
    $sharp=@(Get-OptimizedMWSettingsRows $session | Where-Object {$_.key -ceq 'upscaler sharpness'})[0]
    Assert-Editor $sharp.available 'Supported reduced-resolution NIS control remained unavailable'
    Close-OptimizedMWSettingsSession $session
    $session=New-OptimizedMWSettingsSession $profile
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'preload cell cache min' 100
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'preload cell cache max' 20
    Assert-EditorReject {Save-OptimizedMWSettingsSession $session} 'Invalid cross-setting cache limits committed'
    Close-OptimizedMWSettingsSession $session
    $session=New-OptimizedMWSettingsSession $profile
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'speculative memory budget' 'true'
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'host memory pressure' 'false'
    Assert-EditorReject {Save-OptimizedMWSettingsSession $session} 'Missing memory admission dependency committed'
    Close-OptimizedMWSettingsSession $session
    $before=[IO.File]::ReadAllBytes($profile)
    $session=New-OptimizedMWSettingsSession $profile
    Set-OptimizedMWSettingsPendingValue $session 'Cells' 'initial multi-view preload mode' 2
    Assert-EditorReject {Save-OptimizedMWSettingsSession $session -BeforePublish {throw 'injected publication interruption'}} 'Injected failed publication was accepted'
    Assert-Editor ((Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($profile))) -ceq (Get-OptimizedMWSettingsBytesHash $before)) 'Interrupted atomic write changed original'
    Assert-Editor (@(Get-ChildItem -LiteralPath $fixture -Filter '*.tmp').Count -eq 0) 'Failed atomic write left payload temporary file'
    [IO.File]::AppendAllText($profile,"# concurrent external writer`r`n")
    $externalHash=Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($profile))
    Assert-EditorReject {Save-OptimizedMWSettingsSession $session} 'Editor overwrote a concurrently changed profile'
    Assert-Editor ((Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($profile))) -ceq $externalHash) 'Concurrent change was discarded'
    Close-OptimizedMWSettingsSession $session
    $frozen=Join-Path $fixture 'TestSessions\FrozenConfig_fixture\Optimizations.cfg'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $frozen)) | Out-Null
    [IO.File]::WriteAllText($frozen,"[Cells]`ninitial multi-view preload mode = 2`n")
    $frozenHash=Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($frozen))
    Assert-EditorReject {New-OptimizedMWSettingsSession $frozen} 'Editor allowed frozen benchmark profile writes'
    $template=Join-Path $fixture 'OptimizedMW_Optimizations.cfg'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'OptimizedMW_Optimizations.cfg') -Destination $template
    $fresh=Join-Path $fixture 'Configuration\Optimizations.cfg'
    $session=New-OptimizedMWSettingsSession -Path $fresh -TemplatePath $template
    Close-OptimizedMWSettingsSession $session
    Assert-Editor (-not [IO.File]::Exists($fresh)) 'Opening/cancelling created a profile'
    $session=New-OptimizedMWSettingsSession -Path $fresh -TemplatePath $template
    Set-OptimizedMWSettingsPendingValue $session 'Camera' 'full body first person' 'false'
    $null=Save-OptimizedMWSettingsSession $session
    Assert-Editor ([IO.File]::Exists($fresh)) 'Apply did not create requested Custom preference file'
    Assert-Editor ((Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($frozen))) -ceq $frozenHash) 'Ordinary preference write changed frozen inputs'
    $session=New-OptimizedMWSettingsSession $fresh
    Reset-OptimizedMWSettingsPendingDefaults $session 'Camera'
    $body=Get-OptimizedMWSettingsDefinition $schema 'Camera' 'full body first person'
    Assert-Editor ((Get-OptimizedMWSettingsSessionValue $session $body) -ceq 'false') 'Restore Defaults used Custom profile instead of engine default'
    Close-OptimizedMWSettingsSession $session
    Set-EditorAnswers @('13','2','0','0','0')
    $selection=Read-OptimizedMWNumericTest -GameDirectory $fixture
    Assert-Editor ($null -eq $selection -and $script:answers.Count -eq 0) 'Numeric editor cancellation launched or returned a test selection'
    foreach($name in @('OptimizedMW_SettingsSchema.ps1','OptimizedMW_EditSettings.ps1','OptimizedMW_NumericMenu.ps1')){
        $tokens=$null;$errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $name),[ref]$tokens,[ref]$errors)
        Assert-Editor ($errors.Count -eq 0) ('Script syntax error: '+$name)
    }
    Write-Output ('PASS settings editor: '+$script:checks+' checks; '+$schema.settings.Count+' canonical controls; pending/Cancel/Apply, alias and unknown preservation, atomic failure/conflict, private inputs, numeric navigation; no game.')
}finally{
    $absolute=[IO.Path]::GetFullPath($fixture)
    if(-not $absolute.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected settings fixture path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
