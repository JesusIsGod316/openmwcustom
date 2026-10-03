# Interactive numbers are navigation IDs, never engine bit masks. Existing CLI
# masks and preset names remain unchanged. All functions are side-effect-free
# except console input/output; no settings, game process or recorder is touched.
function Read-OptimizedMWNumber {
    param([string]$Prompt,[string[]]$Choices,[string]$Default='')
    while($true){
        $text=Read-Host $Prompt
        if($text -eq '' -and $Default -ne ''){$text=$Default}
        if($text -cmatch '^[0-9]+$' -and $text -cin $Choices){return $text}
        Write-Host ('Choose a displayed number: '+($Choices -join ', ')) -ForegroundColor Yellow
    }
}

function New-OptimizedMWNumericSelection {
    param([string]$Id,[string]$Label,[int]$Mask=4,[int]$Attack=1,[string]$Preset='',
        [string]$Candidate='CONTROL',[bool]$Progress=$false,[bool]$Mips=$false,[string]$Mode='')
    return [pscustomobject][ordered]@{menu_revision=1;selection_id=$Id;label=$Label;
        mask=$Mask;attack_mask=$Attack;focused_preset=$Preset;candidate=$Candidate;
        composite_progress=$Progress;composite_cpu_mips=$Mips;legacy_mode=$Mode;
        trace_level='Normal';system_trace=$false;gpu_query_defer=$false;name=$Label}
}

function Get-OptimizedMWNumericCategories {
    return @('Controls','Terrain and rendering','Animation','Resource cleanup',
        'User interface','Lua','File I/O','Combined tests','Advanced and earlier experiments')
}

function Get-OptimizedMWNumericCandidates {
    param([ValidateRange(1,9)][int]$Category)
    switch($Category){
        1 {return @((New-OptimizedMWNumericSelection '1.1' 'Retained control (current comparison)' -Preset CONTROL),
            (New-OptimizedMWNumericSelection '1.2' 'Original feature control (all candidates off)' -Mask 0 -Attack 0))}
        2 {return @((New-OptimizedMWNumericSelection '2.1' 'Retained control' -Preset CONTROL),
            (New-OptimizedMWNumericSelection '2.2' 'Original incremental terrain preparation' -Attack 257 -Preset STEP),
            (New-OptimizedMWNumericSelection '2.3' 'Early terrain preparation + progress repair' -Attack 261 -Preset EARLY-STEP -Progress $true),
            (New-OptimizedMWNumericSelection '2.4' 'Early terrain + progress repair + supplied mipmaps' -Attack 261 -Preset TERRAIN-MIPS -Progress $true -Mips $true),
            (New-OptimizedMWNumericSelection '2.5' 'Terrain state batching only' -Mask 6),
            (New-OptimizedMWNumericSelection '2.6' 'Demanded terrain readiness only' -Mask 12),
            (New-OptimizedMWNumericSelection '2.7' 'Water resource preparation only' -Attack 9),
            (New-OptimizedMWNumericSelection '2.8' 'Weather image preparation only' -Attack 17),
            (New-OptimizedMWNumericSelection '2.9' 'Supplied mipmaps only' -Mips $true))}
        3 {return @((New-OptimizedMWNumericSelection '3.1' 'Animation source preparation, retention off' -Attack 0),
            (New-OptimizedMWNumericSelection '3.2' 'Animation retention (current control)'),
            (New-OptimizedMWNumericSelection '3.3' 'Retention + warm keyframe reuse' -Attack 129),
            (New-OptimizedMWNumericSelection '3.4' 'Retention + source descriptor reuse' -Attack 3),
            (New-OptimizedMWNumericSelection '3.5' 'Retention + warm keyframes + descriptor reuse' -Attack 131))}
        4 {return @((New-OptimizedMWNumericSelection '4.1' 'Retained control' -Preset CONTROL),
            (New-OptimizedMWNumericSelection '4.2' 'Bounded deletion admission' -Attack 65 -Preset CLEANUP))}
        5 {return @((New-OptimizedMWNumericSelection '5.1' 'Retained control'),
            (New-OptimizedMWNumericSelection '5.2' 'Text and layout reuse' -Mask 5))}
        6 {return @((New-OptimizedMWNumericSelection '6.1' 'Retained control'),
            (New-OptimizedMWNumericSelection '6.2' 'Per-container package descriptor reuse' -Attack 33))}
        7 {return @((New-OptimizedMWNumericSelection '7.1' 'Original file I/O'),
            (New-OptimizedMWNumericSelection '7.2' '64 KiB refill candidate' -Candidate I1),
            (New-OptimizedMWNumericSelection '7.3' 'Resident seek candidate' -Candidate I2),
            (New-OptimizedMWNumericSelection '7.4' 'Refill + resident seek' -Candidate I1I2))}
        8 {return @((New-OptimizedMWNumericSelection '8.1' 'Build a custom combination'),
            (New-OptimizedMWNumericSelection '8.2' 'Original four hitch candidates' -Mask 15 -Attack 0),
            (New-OptimizedMWNumericSelection '8.3' 'Terrain progress + cleanup' -Attack 325 -Preset COMBINED -Progress $true),
            (New-OptimizedMWNumericSelection '8.4' 'Terrain progress + supplied mips + cleanup' -Attack 325 -Preset COMBINED-MIPS -Progress $true -Mips $true),
            (New-OptimizedMWNumericSelection '8.5' 'Earlier warm keyframe + STEP + cleanup combination' -Attack 449))}
        9 {return @((New-OptimizedMWNumericSelection '9.1' 'Camera/static temporal inputs' -Mask 0 -Attack 0 -Mode 2),
            (New-OptimizedMWNumericSelection '9.2' 'Motion vector visualization' -Mask 0 -Attack 0 -Mode 5),
            (New-OptimizedMWNumericSelection '9.3' 'Retained temporal ownership' -Mask 0 -Attack 0 -Mode 8),
            (New-OptimizedMWNumericSelection '9.4' 'Earlier terrain preparation' -Mask 0 -Attack 0 -Mode 9),
            (New-OptimizedMWNumericSelection '9.5' 'Temporal ownership + earlier terrain' -Mask 0 -Attack 0 -Mode 10),
            (New-OptimizedMWNumericSelection '9.6' 'Experimental dynamic motion' -Mask 0 -Attack 0 -Mode 11),
            (New-OptimizedMWNumericSelection '9.7' 'Earlier refresh experiment' -Mask 0 -Attack 0 -Mode 6))}
    }
}

function Get-OptimizedMWCombinedToggles {
    return @('Text/layout reuse','Terrain state batching','Animation source preparation',
        'Demanded terrain readiness','Animation retention','Animation descriptor reuse',
        'Warm keyframe reuse','Early terrain preparation','Incremental terrain STEP',
        'Terrain progress repair','Supplied terrain mipmaps','Deletion admission',
        'Water resource preparation','Weather image preparation','Lua package descriptors')
}

function Test-OptimizedMWCombinedToggle {
    param([object]$Selection,[ValidateRange(1,15)][int]$Choice)
    if($Choice -le 4){return [bool]($Selection.mask -band (1 -shl ($Choice-1)))}
    if($Choice -eq 10){return [bool]$Selection.composite_progress}
    if($Choice -eq 11){return [bool]$Selection.composite_cpu_mips}
    $bits=@{5=1;6=2;7=128;8=4;9=256;12=64;13=8;14=16;15=32}
    return [bool]($Selection.attack_mask -band $bits[$Choice])
}

function Set-OptimizedMWCombinedToggle {
    param([object]$Selection,[ValidateRange(1,15)][int]$Choice)
    $enabled=-not (Test-OptimizedMWCombinedToggle $Selection $Choice)
    if($Choice -le 4){$bit=1 -shl ($Choice-1);$Selection.mask=$Selection.mask -bxor $bit}
    elseif($Choice -eq 10){$Selection.composite_progress=$enabled}
    elseif($Choice -eq 11){$Selection.composite_cpu_mips=$enabled}
    else{$bits=@{5=1;6=2;7=128;8=4;9=256;12=64;13=8;14=16;15=32};$Selection.attack_mask=$Selection.attack_mask -bxor $bits[$Choice]}
    # Enabling a dependency makes prerequisites explicit. Disabling a prerequisite
    # disables its dependants, so the displayed and recorded vector cannot disagree.
    if($enabled -and $Choice -in @(5,7)){
        $Selection.mask=$Selection.mask -bor 4;$Selection.attack_mask=$Selection.attack_mask -bor 1
        if($Choice -eq 7){Write-Host 'Warm keyframes require animation retention and source preparation; both are enabled.' -ForegroundColor Yellow}
    }
    if(-not $enabled -and $Choice -in @(3,5)){
        $Selection.attack_mask=$Selection.attack_mask -band (-bnot 129)
        Write-Host 'Animation retention and warm keyframes are now off.' -ForegroundColor Yellow
    }
    if($enabled -and $Choice -eq 10){
        $Selection.attack_mask=$Selection.attack_mask -bor 256
        Write-Host 'Progress repair requires incremental STEP; STEP is enabled.' -ForegroundColor Yellow
    }
    if(-not $enabled -and $Choice -eq 9){$Selection.composite_progress=$false}
}

function Read-OptimizedMWCombinedSelection {
    $selection=New-OptimizedMWNumericSelection '8.1' 'Custom combination'
    $labels=Get-OptimizedMWCombinedToggles
    while($true){
        Write-Host 'Combined tests: toggle mechanisms; all start from the displayed retained control.' -ForegroundColor Cyan
        foreach($index in 0..($labels.Count-1)){
            Write-Host (' {0,2} = {1,-34} {2}' -f ($index+1),$labels[$index],$(if(Test-OptimizedMWCombinedToggle $selection ($index+1)){'ON'}else{'off'}))
        }
        Write-Host ' 16 = Continue | 17 = Reset to retained control | 18 = All candidates off | 0 = Back'
        $choice=Read-OptimizedMWNumber 'Combined option (0-18)' @('0','1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18')
        if($choice -eq '0'){return $null}
        if($choice -eq '16'){
            Write-Host 'File I/O: 1 = Original | 2 = Refill | 3 = Resident seek | 4 = Both | 0 = Back'
            $io=Read-OptimizedMWNumber 'Combined I/O (0-4; Enter = 1)' @('0','1','2','3','4') '1'
            if($io -eq '0'){continue}
            $selection.candidate=@('CONTROL','I1','I2','I1I2')[[int]$io-1]
            return $selection
        }
        if($choice -eq '17'){$selection=New-OptimizedMWNumericSelection '8.1' 'Custom combination';continue}
        if($choice -eq '18'){$selection=New-OptimizedMWNumericSelection '8.1' 'Custom combination' -Mask 0 -Attack 0;continue}
        Set-OptimizedMWCombinedToggle $selection ([int]$choice)
    }
}

function Write-OptimizedMWNumericSummary {
    param([object]$Selection)
    Write-Host ('Selected: '+$Selection.label+' ['+$Selection.selection_id+']') -ForegroundColor Green
    if($Selection.legacy_mode){Write-Host ('Earlier experiment mode: '+$Selection.legacy_mode);return}
    $enabled=[Collections.Generic.List[string]]::new()
    $labels=Get-OptimizedMWCombinedToggles
    foreach($index in 0..($labels.Count-1)){if(Test-OptimizedMWCombinedToggle $Selection ($index+1)){$enabled.Add($labels[$index])}}
    Write-Host ('Gameplay mechanisms: '+$(if($enabled.Count){$enabled -join '; '}else{'original control, candidates off'}))
    Write-Host ('File I/O: '+$Selection.candidate)
}

function Read-OptimizedMWNumericTest {
    $categories=Get-OptimizedMWNumericCategories
    while($true){
        Write-Host ''
        Write-Host 'OptimizedMW - select an optimization category' -ForegroundColor Cyan
        foreach($index in 0..($categories.Count-1)){Write-Host (' {0} = {1}' -f ($index+1),$categories[$index])}
        Write-Host ' 0 = Exit'
        $category=Read-OptimizedMWNumber 'Category (0-9; Enter = 1)' @('0','1','2','3','4','5','6','7','8','9') '1'
        if($category -eq '0'){return $null}
        $candidates=@(Get-OptimizedMWNumericCandidates ([int]$category))
        while($true){
            Write-Host ('Category: '+$categories[[int]$category-1]) -ForegroundColor Cyan
            foreach($index in 0..($candidates.Count-1)){Write-Host (' {0} = {1}' -f ($index+1),$candidates[$index].label)}
            Write-Host ' 0 = Back'
            $choices=@('0')+@(1..$candidates.Count | ForEach-Object {[string]$_})
            $choice=Read-OptimizedMWNumber 'Optimization (0 = back; Enter = 1)' $choices '1'
            if($choice -eq '0'){break}
            $selection=$candidates[[int]$choice-1]
            if($category -eq '8' -and $choice -eq '1'){$selection=Read-OptimizedMWCombinedSelection;if($null -eq $selection){continue}}
            Write-OptimizedMWNumericSummary $selection
            while($true){
                Write-Host ' 1 = Normal performance | 2 = Deep engine diagnostics | 0 = Back'
                $depth=Read-OptimizedMWNumber 'Run type (0-2; Enter = 1)' @('0','1','2') '1'
                if($depth -eq '0'){break}
                $selection.trace_level=$(if($depth -eq '2'){'Deep'}else{'Normal'})
                $selection.gpu_query_defer=$depth -eq '2'
                $selection.system_trace=$false
                if($depth -eq '2'){
                    Write-Host ' 1 = Engine only | 2 = Engine + Windows system trace | 0 = Back'
                    $windows=Read-OptimizedMWNumber 'Deep recording (0-2; Enter = 1)' @('0','1','2') '1'
                    if($windows -eq '0'){continue}
                    $selection.system_trace=$windows -eq '2'
                }
                return $selection
            }
        }
    }
}

function Read-OptimizedMWComparisonDeclaration {
    param([string]$GameDirectory,[string]$SavePath,[string]$RouteLabel='UNSPECIFIED',[string]$FrameCapLabel='UNSPECIFIED')
    $path=Join-Path $GameDirectory 'COMPARISON-DECLARATION.json'
    $previous=$null
    if([IO.File]::Exists($path)){
        $file=Get-Item -LiteralPath $path
        if($file.Length -le 16384 -and -not ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)){
            try{
                $value=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
                if($value.schema -eq 1 -and [string]$value.save_path -notmatch '[\r\n]' -and
                    [string]$value.route_label -match '^[^\r\n]{1,256}$' -and [string]$value.frame_cap_label -match '^[^\r\n]{1,256}$'){$previous=$value}
            }catch{}
        }
    }
    Write-Host 'Comparison labels describe your intended test; they do not load a save or change a frame cap.'
    if($previous){Write-Host ('Previous: '+$previous.route_label+'; '+$previous.frame_cap_label+'; '+$(if($previous.save_path){$previous.save_path}else{'save unspecified'}))}
    Write-Host ' 1 = Keep labels unspecified | 2 = Enter/save labels | 0 = Cancel run'
    $choices=@('0','1','2')
    if($previous){Write-Host ' 3 = Reuse the displayed previous labels';$choices+='3'}
    $choice=Read-OptimizedMWNumber 'Comparison labels' $choices '1'
    if($choice -eq '0'){return $null}
    if($choice -eq '3'){$SavePath=[string]$previous.save_path;$RouteLabel=[string]$previous.route_label;$FrameCapLabel=[string]$previous.frame_cap_label}
    if($choice -eq '2'){
        if(-not $SavePath){$SavePath=(Read-Host 'Outdoor save file full path (Enter = unspecified)').Trim('"')}
        if($RouteLabel -eq 'UNSPECIFIED'){$RouteLabel=Read-Host 'Route label (Enter = unspecified)';if(-not $RouteLabel){$RouteLabel='UNSPECIFIED'}}
        if($FrameCapLabel -eq 'UNSPECIFIED'){$FrameCapLabel=Read-Host 'Frame-cap label (Enter = unspecified)';if(-not $FrameCapLabel){$FrameCapLabel='UNSPECIFIED'}}
        if($RouteLabel -notmatch '^[^\r\n]{1,256}$' -or $FrameCapLabel -notmatch '^[^\r\n]{1,256}$'){throw 'Comparison labels must fit on one line (1-256 characters)'}
        if($SavePath -and -not [IO.File]::Exists([IO.Path]::GetFullPath($SavePath))){throw 'Declared save file is missing; correct it before running'}
        $declaration=[pscustomobject][ordered]@{schema=1;save_path=$SavePath;route_label=$RouteLabel;frame_cap_label=$FrameCapLabel;status='caller_declared_not_automatically_verified'}
        $temporary=$path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
        try{
            $declaration | ConvertTo-Json | Set-Content -LiteralPath $temporary -Encoding UTF8
            if([IO.File]::Exists($path)){
                if((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing a reparse-point comparison declaration'}
                [IO.File]::Replace($temporary,$path,[System.Management.Automation.Language.NullString]::Value)
            }else{[IO.File]::Move($temporary,$path)}
        }finally{if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}}
    }
    return [pscustomobject]@{save_path=$SavePath;route_label=$RouteLabel;frame_cap_label=$FrameCapLabel}
}
