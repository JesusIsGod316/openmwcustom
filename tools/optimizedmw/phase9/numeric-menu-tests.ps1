# Hermetic console/script tests. No game, WPR, real user settings or UI.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_NumericMenu.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_AttackPresets.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_HitchMatrix.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_RunManifest.ps1')
function Set-FixtureAnswers {
    param([string[]]$Answers)
    $script:answers=[Collections.Generic.Queue[string]]::new()
    foreach($answer in $Answers){$script:answers.Enqueue($answer)}
}
function Read-Host {
    param([string]$Prompt)
    if(-not $script:answers.Count){throw ('Fixture exhausted before '+$Prompt)}
    return $script:answers.Dequeue()
}
function Assert-Selection {
    param([object]$Actual,[object]$Expected,[string]$Depth)
    foreach($property in @('selection_id','mask','attack_mask','focused_preset','candidate','composite_progress','composite_cpu_mips','legacy_mode')){
        if($Actual.$property -cne $Expected.$property){throw ('Menu changed '+$property+' for '+$Expected.selection_id)}
    }
    if($Actual.trace_level -ne $Depth -or $Actual.system_trace -or $Actual.gpu_query_defer -ne ($Depth -eq 'Deep')){throw 'Trace depth changed a mechanism or recorder default'}
    if($script:answers.Count){throw 'Unused menu fixture answers'}
}
$before=Get-Phase9EnvironmentSnapshot
$temporaryRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$fixture=Join-Path $temporaryRoot ('p9-numeric-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture | Out-Null
try{
    $count=0
    foreach($category in 1..9){
        $candidates=@(Get-OptimizedMWNumericCandidates $category)
        foreach($index in 0..($candidates.Count-1)){
            if($category -eq 8 -and $index -eq 0){continue}
            $hashes=@()
            foreach($depth in @('Normal','Deep')){
                $replies=@([string]$category,[string]($index+1),$(if($depth -eq 'Normal'){'1'}else{'2'}))
                if($depth -eq 'Deep'){$replies+='1'}
                Set-FixtureAnswers $replies
                $actual=Read-OptimizedMWNumericTest
                Assert-Selection $actual $candidates[$index] $depth
                $hashes+=Get-Phase9IdentityHash ($actual | Select-Object mask,attack_mask,candidate,composite_progress,composite_cpu_mips,legacy_mode)
                $count++
            }
            if($hashes[0] -cne $hashes[1]){throw 'Normal/Deep gameplay vectors differ'}
        }
    }
    Set-FixtureAnswers @('letters','99','2','0','1','1','0','2','1')
    $actual=Read-OptimizedMWNumericTest
    Assert-Selection $actual (Get-OptimizedMWNumericCandidates 1)[1] Normal
    Set-FixtureAnswers @('2','4','2','0','1')
    $actual=Read-OptimizedMWNumericTest
    Assert-Selection $actual (Get-OptimizedMWNumericCandidates 2)[3] Normal
    Set-FixtureAnswers @('2','4','2','2')
    $actual=Read-OptimizedMWNumericTest
    if(-not $actual.system_trace -or $actual.trace_level -ne 'Deep' -or $actual.selection_id -ne '2.4'){throw 'Explicit numeric Windows selection lost'}
    Set-FixtureAnswers @('0');if($null -ne (Read-OptimizedMWNumericTest)){throw 'Exit returned a selection'}
    Set-FixtureAnswers @('8','1','0','0','0');if($null -ne (Read-OptimizedMWNumericTest)){throw 'Combined back/exit failed'}
    Set-FixtureAnswers @('8','1','18','7','10','11','12','16','4','2','1')
    $actual=Read-OptimizedMWNumericTest
    if($actual.mask -ne 4 -or $actual.attack_mask -ne 449 -or -not $actual.composite_progress -or
        -not $actual.composite_cpu_mips -or $actual.candidate -ne 'I1I2' -or $actual.trace_level -ne 'Deep'){throw 'Combined toggles/prerequisites/I-O lost'}
    foreach($selection in @((New-OptimizedMWNumericSelection '8.1' 'fixture' -Mask 0 -Attack 0),(New-OptimizedMWNumericSelection '8.1' 'fixture'))){
        Set-OptimizedMWCombinedToggle $selection 7
        if(-not ($selection.mask -band 4) -or ($selection.attack_mask -band 129) -ne 129){throw 'Warm prerequisite was not enabled'}
        Set-OptimizedMWCombinedToggle $selection 5
        if($selection.attack_mask -band 129){throw 'Removing retention left a dependent warm lease'}
        Set-OptimizedMWCombinedToggle $selection 10
        if(-not $selection.composite_progress -or -not ($selection.attack_mask -band 256)){throw 'STEP prerequisite was not enabled'}
        Set-OptimizedMWCombinedToggle $selection 9
        if($selection.composite_progress -or ($selection.attack_mask -band 256)){throw 'Removing STEP left progress on'}
    }
    $save=Join-Path $fixture 'save.omwsave';[IO.File]::WriteAllBytes($save,[byte[]]@(1,2,3))
    Set-FixtureAnswers @('1')
    $labels=Read-OptimizedMWComparisonDeclaration $fixture
    if($labels.route_label -ne 'UNSPECIFIED' -or $labels.save_path){throw 'Blank metadata invented a declaration'}
    Set-FixtureAnswers @('2',$save,'same route','uncapped')
    $labels=Read-OptimizedMWComparisonDeclaration $fixture
    Set-FixtureAnswers @('3')
    $reused=Read-OptimizedMWComparisonDeclaration $fixture
    if($reused.save_path -ne $save -or $reused.route_label -ne 'same route' -or $reused.frame_cap_label -ne 'uncapped'){throw 'Explicit label reuse lost declarations'}
    Set-FixtureAnswers @('0');if($null -ne (Read-OptimizedMWComparisonDeclaration $fixture)){throw 'Label cancellation failed'}
    if((Get-FileHash -LiteralPath $save).Hash -ne '039058C6F2C0CB492C533B0A4D14EF77CC0F78ABCCCED5287D84A1A2011CFB81'){throw 'Menu modified a declared save'}
    foreach($file in @('OptimizedMW_NumericMenu.ps1','OptimizedMW_Test.ps1','OptimizedMW_AttackPresets.ps1','OptimizedMW_HitchMatrix.ps1')){
        $tokens=$null;$errors=$null
        $null=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot $file),[ref]$tokens,[ref]$errors)
        if($errors.Count){throw ($errors | Out-String)}
        if([IO.File]::ReadAllText((Join-Path $PSScriptRoot $file)) -match "(?:N|D|W|F|A|C|H|X) = (?:Normal|Deep|Exit|show|include|Focused|Custom|Earlier|CURRENT)"){throw 'A letter-based menu remains'}
    }
    Write-Host "PASS numeric menu: $count category/depth paths; invalid input/back/exit; custom dependency removal and reset; explicit Windows selection; opt-in label reuse; no game/WPR/UI."
}finally{
    if(-not (Restore-Phase9Environment $before)){throw 'Menu tests changed host environment'}
    $absolute=[IO.Path]::GetFullPath($fixture)
    if(-not $absolute.StartsWith($temporaryRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected numeric fixture path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
