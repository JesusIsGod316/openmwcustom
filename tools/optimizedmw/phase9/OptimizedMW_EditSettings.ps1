# Settings editing is independent of game launch. Dot-sourcing only defines functions.
. (Join-Path $PSScriptRoot 'OptimizedMW_SettingsSchema.ps1')
. (Join-Path $PSScriptRoot 'OptimizedMW_SettingNames.ps1')

function Get-OptimizedMWSettingsBytesHash {
    param([byte[]]$Bytes)
    $algorithm=[Security.Cryptography.SHA256]::Create()
    try{return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-','').ToLowerInvariant()}
    finally{$algorithm.Dispose()}
}

function Assert-OptimizedMWEditableSettingsPath {
    param([Parameter(Mandatory=$true)][string]$Path)
    if($Path -match '[\r\n]' -or -not [IO.Path]::IsPathRooted($Path)){throw 'Choose an absolute settings path'}
    $absolute=[IO.Path]::GetFullPath($Path)
    if([IO.Path]::GetExtension($absolute) -cne '.cfg'){throw 'Settings editor accepts .cfg profiles only'}
    if($absolute -match '(?i)(^|[\\/])(TestSessions|Benchmarks|Routes|FrozenConfiguration|FrozenConfig_[^\\/]*|Baseline)([\\/]|$)'){
        throw 'Frozen benchmark inputs are protected; edit the ordinary Custom profile instead'
    }
    $probe=$absolute
    while($probe){
        if(Test-Path -LiteralPath $probe){
            $item=Get-Item -LiteralPath $probe -Force
            if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Settings editor refuses reparse-point profiles'}
            if($probe -ceq $absolute -and $item.PSIsContainer){throw 'Settings path is a directory'}
        }
        $parent=[IO.Path]::GetDirectoryName($probe)
        if($parent -ceq $probe){break};$probe=$parent
    }
    return $absolute
}

function Get-OptimizedMWSettingsDefinition {
    param([object]$Schema,[string]$Section,[string]$Key)
    $canonical=Resolve-OptimizedMWSettingName $Section $Key
    $matches=@($Schema.settings | Where-Object {$_.section -ceq $canonical.Section -and $_.key -ceq $canonical.Key})
    if($matches.Count -ne 1){throw ('Unknown editable setting: ['+$canonical.Section+'] '+$canonical.Key)}
    return $matches[0]
}

function ConvertTo-OptimizedMWSettingsValue {
    param([Parameter(Mandatory=$true)][object]$Definition,[Parameter(Mandatory=$true)][object]$Value)
    $text=([string]$Value).Trim()
    if($text -match '[\r\n\x00]' -or $text.Length -gt 128){throw 'A setting must be one bounded value'}
    switch($Definition.type){
        'boolean'{
            if($Value -is [bool]){return ([string]$Value).ToLowerInvariant()}
            if($text -cnotin @('true','false')){throw 'Choose true or false'}
            return $text
        }
        'integer'{
            if($text -cnotmatch '^-?[0-9]+$'){throw 'Enter a whole number'}
            $number=0L
            if(-not [long]::TryParse($text,[Globalization.NumberStyles]::Integer,[Globalization.CultureInfo]::InvariantCulture,[ref]$number)){
                throw 'Whole number exceeds supported capacity'
            }
            if($number -lt $Definition.minimum -or $number -gt $Definition.maximum -or
                ($Definition.exclusive_minimum -and $number -eq $Definition.minimum)){throw 'Value is outside the displayed bounds'}
            return $number.ToString([Globalization.CultureInfo]::InvariantCulture)
        }
        'number'{
            if($text -cnotmatch '^-?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?$'){
                throw 'Enter a finite number with a decimal point when needed'
            }
            $number=0.0
            if(-not [double]::TryParse($text,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$number) -or
                [double]::IsNaN($number) -or [double]::IsInfinity($number)){throw 'Enter a finite number'}
            if($number -lt $Definition.minimum -or $number -gt $Definition.maximum -or
                ($Definition.exclusive_minimum -and $number -eq $Definition.minimum)){throw 'Value is outside the displayed bounds'}
            # Float settings must survive the engine's conversion; reject underflow at strict minima.
            $engineNumber=[single]$number
            if([single]::IsInfinity($engineNumber) -or ($Definition.exclusive_minimum -and $engineNumber -le $Definition.minimum)){
                throw 'Value cannot be represented by the engine setting'
            }
            return $engineNumber.ToString('G9',[Globalization.CultureInfo]::InvariantCulture)
        }
        'enum'{
            if($text -cnotin $Definition.choices){throw ('Choose one of: '+($Definition.choices -join ', '))}
            return $text
        }
        default{throw 'Unsupported setting type'}
    }
}

function Read-OptimizedMWSettingsValues {
    param([string]$Text,[object]$Schema)
    $physical=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    $section=''
    foreach($line in ($Text -split '\r\n|\n|\r')){
        $trimmed=$line.Trim()
        if(-not $trimmed -or $trimmed.StartsWith('#') -or $trimmed.StartsWith(';')){continue}
        if($trimmed -match '^\[([^\]]+)\]$'){$section=$matches[1].Trim();continue}
        if(-not $section -or $trimmed -notmatch '^([^=]+?)\s*=\s*(.*)$'){continue}
        $key=$matches[1].Trim();$value=$matches[2].Trim();$physicalId=$section+"`n"+$key
        if($physical.ContainsKey($physicalId)){
            $canonical=Resolve-OptimizedMWSettingName $section $key
            $known=@($Schema.settings | Where-Object {$_.section -ceq $canonical.Section -and $_.key -ceq $canonical.Key})
            if($known.Count){throw ('Duplicate profile key: ['+$section+'] '+$key)}
            # Preserve unsupported duplicate lines byte-for-byte; this editor does not own their semantics.
            $physical[$physicalId]=[pscustomobject]@{section=$section;key=$key;value=$value};continue
        }
        $physical.Add($physicalId,[pscustomobject]@{section=$section;key=$key;value=$value})
    }
    $values=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    foreach($entry in $physical.Values){
        $canonical=Resolve-OptimizedMWSettingName $entry.section $entry.key
        $id=$canonical.Section+"`n"+$canonical.Key
        if(($canonical.Section -cne $entry.section -or $canonical.Key -cne $entry.key) -and $physical.ContainsKey($id)){continue}
        $values[$id]=$entry.value
    }
    return ,$values
}

function New-OptimizedMWSettingsSession {
    param([Parameter(Mandatory=$true)][string]$Path,[string]$TemplatePath='',
        [ValidateSet('Custom','Gameplay')][string]$ProfileKind='Custom')
    $absolute=Assert-OptimizedMWEditableSettingsPath $Path
    $exists=[IO.File]::Exists($absolute)
    $original=if($exists){[IO.File]::ReadAllBytes($absolute)}else{[byte[]]@()}
    if($original.Length -gt 1048576){throw 'Settings profile exceeds 1 MiB'}
    $baseBytes=$original
    if(-not $exists -and $TemplatePath){
        $baseBytes=[IO.File]::ReadAllBytes([IO.Path]::GetFullPath($TemplatePath))
        if($baseBytes.Length -gt 1048576){throw 'Settings template exceeds 1 MiB'}
    }
    $encoding=[Text.UTF8Encoding]::new($false,$true)
    $hasBom=$baseBytes.Length -ge 3 -and $baseBytes[0] -eq 239 -and $baseBytes[1] -eq 187 -and $baseBytes[2] -eq 191
    $offset=if($hasBom){3}else{0}
    $text=$encoding.GetString($baseBytes,$offset,$baseBytes.Length-$offset)
    $schema=Get-OptimizedMWSettingsSchema
    return [pscustomobject][ordered]@{schema=$schema;path=$absolute;profile_kind=$ProfileKind;
        original_exists=$exists;original_sha256=$(if($exists){Get-OptimizedMWSettingsBytesHash $original}else{$null});
        original_bytes=$original;base_text=$text;utf8_bom=$hasBom;
        newline=$(if($text.Contains("`r`n")){"`r`n"}elseif($text.Contains("`r")){"`r"}else{"`n"});
        saved_values=(Read-OptimizedMWSettingsValues $text $schema);
        pending=[Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal);
        effective_state='NotObserved';apply_class='RestartRequired';closed=$false}
}

function Get-OptimizedMWSettingsSessionValue {
    param([object]$Session,[object]$Definition)
    $id=$Definition.section+"`n"+$Definition.key
    if($Session.pending.ContainsKey($id)){return $Session.pending[$id]}
    if($Session.saved_values.ContainsKey($id)){return $Session.saved_values[$id]}
    return [string]$Definition.default
}

function Set-OptimizedMWSettingsPendingValue {
    param([object]$Session,[string]$Section,[string]$Key,[object]$Value)
    if($Session.closed){throw 'Settings session is closed'}
    $definition=Get-OptimizedMWSettingsDefinition $Session.schema $Section $Key
    if(-not $definition.editable){throw 'This control is not available for editing'}
    $normalized=ConvertTo-OptimizedMWSettingsValue $definition $Value
    $id=$definition.section+"`n"+$definition.key
    $Session.pending[$id]=$normalized
}

function Reset-OptimizedMWSettingsPendingDefaults {
    param([object]$Session,[string]$Section='')
    if($Session.closed){throw 'Settings session is closed'}
    foreach($definition in $Session.schema.settings){
        if($definition.editable -and (-not $Section -or $definition.section -ceq $Section)){
            Set-OptimizedMWSettingsPendingValue $Session $definition.section $definition.key $definition.default
        }
    }
}

function Get-OptimizedMWSettingsRows {
    param([object]$Session,[ValidateSet('Custom','Benchmark','Gameplay')][string]$RunMode='Custom')
    foreach($definition in $Session.schema.settings){
        $id=$definition.section+"`n"+$definition.key
        $saved=if($Session.saved_values.ContainsKey($id)){$Session.saved_values[$id]}else{[string]$definition.default}
        $pending=Get-OptimizedMWSettingsSessionValue $Session $definition
        $available=[bool]$definition.editable;$reason=[string]$definition.description
        if($definition.section -ceq 'Video' -and $definition.key -ceq 'upscaler sharpness'){
            $scale=Get-OptimizedMWSettingsSessionValue $Session (Get-OptimizedMWSettingsDefinition $Session.schema 'Video' 'render scale')
            $upscaler=Get-OptimizedMWSettingsSessionValue $Session (Get-OptimizedMWSettingsDefinition $Session.schema 'Video' 'upscaler')
            $number=0.0
            if($upscaler -cne 'nis' -or -not [double]::TryParse($scale,[Globalization.NumberStyles]::Float,[Globalization.CultureInfo]::InvariantCulture,[ref]$number) -or $number -ge 1){
                $available=$false;$reason='Inactive: sharpening requires NIS and render scale below 1; no full-resolution sharpening path is claimed.'
            }
        }
        [pscustomobject][ordered]@{section=$definition.section;key=$definition.key;label=$definition.label;
            saved_value=$saved;saved_origin=$(if($Session.saved_values.ContainsKey($id)){'Profile'}else{'EngineDefault'});
            pending_value=$pending;unsaved=$Session.pending.ContainsKey($id);effective_value=$null;
            effective_state='NotObserved';apply_class=$definition.apply_class;available=$available;reason=$reason;
            application=$(if($RunMode -ceq 'Benchmark'){'SavedCustomIgnoredByBenchmark'}else{'NextLaunchRequested_NotRuntimeVerified'})}
    }
}

function Test-OptimizedMWSettingsPending {
    param([object]$Session)
    foreach($definition in $Session.schema.settings){
        $id=$definition.section+"`n"+$definition.key
        if($Session.pending.ContainsKey($id) -or $Session.saved_values.ContainsKey($id)){
            $null=ConvertTo-OptimizedMWSettingsValue $definition (Get-OptimizedMWSettingsSessionValue $Session $definition)
        }
    }
    $constraints=@(@('Cells','preload cell cache min','Cells','preload cell cache max'),
        @('Cells','gpu soft budget mb','Cells','gpu hard budget mb'),
        @('Optimizations','experimental scratch budget mb','Optimizations','experimental cache budget mb'),
        @('Groundcover','lod near distance','Groundcover','lod far distance'))
    foreach($pair in $constraints){
        $left=Get-OptimizedMWSettingsDefinition $Session.schema $pair[0] $pair[1]
        $right=Get-OptimizedMWSettingsDefinition $Session.schema $pair[2] $pair[3]
        if(-not ($Session.pending.ContainsKey($left.section+"`n"+$left.key) -or $Session.pending.ContainsKey($right.section+"`n"+$right.key))){continue}
        $lv=ConvertTo-OptimizedMWSettingsValue $left (Get-OptimizedMWSettingsSessionValue $Session $left)
        $rv=ConvertTo-OptimizedMWSettingsValue $right (Get-OptimizedMWSettingsSessionValue $Session $right)
        if([double]::Parse($lv,[Globalization.CultureInfo]::InvariantCulture) -gt [double]::Parse($rv,[Globalization.CultureInfo]::InvariantCulture)){
            throw ($left.label+' must not exceed '+$right.label)
        }
    }
    $budget=Get-OptimizedMWSettingsDefinition $Session.schema 'Cells' 'speculative memory budget'
    $pressure=Get-OptimizedMWSettingsDefinition $Session.schema 'Cells' 'host memory pressure'
    if(($Session.pending.ContainsKey("Cells`nspeculative memory budget") -or $Session.pending.ContainsKey("Cells`nhost memory pressure")) -and
        (Get-OptimizedMWSettingsSessionValue $Session $budget) -ceq 'true' -and (Get-OptimizedMWSettingsSessionValue $Session $pressure) -cne 'true'){
        throw 'Speculative memory budget requires host memory pressure'
    }
    return $true
}

function Set-OptimizedMWSettingsTextValue {
    param([string]$Text,[string]$Section,[string]$Key,[string]$Value,[string]$Newline)
    # Remove only the edited canonical/legacy key; every other line stays intact.
    $lines=[Collections.Generic.List[string]]::new()
    $current=''
    foreach($line in [regex]::Matches($Text,'[^\r\n]*(?:\r\n|\r|\n|$)')){
        $part=$line.Value;if(-not $part){continue}
        if($part.TrimEnd("`r","`n") -match '^\s*\[([^\]]+)\]\s*$'){$current=$matches[1].Trim()}
        if($part -match '^\s*([^#;=]+?)\s*='){
            $existing=Resolve-OptimizedMWSettingName $current $matches[1].Trim()
            if($existing.Section -ceq $Section -and $existing.Key -ceq $Key){continue}
        }
        $lines.Add($part)
    }
    $sectionIndex=-1
    for($index=0;$index -lt $lines.Count;$index++){
        if($lines[$index].TrimEnd("`r","`n") -match '^\s*\[([^\]]+)\]\s*$' -and $matches[1].Trim() -ceq $Section){$sectionIndex=$index;break}
    }
    $replacement=$Key+' = '+$Value+$Newline
    if($sectionIndex -ge 0){
        if(-not $lines[$sectionIndex].EndsWith("`n") -and -not $lines[$sectionIndex].EndsWith("`r")){$lines[$sectionIndex]+=$Newline}
        $lines.Insert($sectionIndex+1,$replacement)
    }else{
        if($lines.Count -and -not $lines[$lines.Count-1].EndsWith("`n") -and -not $lines[$lines.Count-1].EndsWith("`r")){$lines[$lines.Count-1]+=$Newline}
        $lines.Add('['+$Section+']'+$Newline);$lines.Add($replacement)
    }
    return ($lines -join '')
}

function Save-OptimizedMWSettingsSession {
    param([object]$Session,[scriptblock]$BeforePublish=$null)
    if($Session.closed){throw 'Settings session is closed'}
    $null=Test-OptimizedMWSettingsPending $Session
    $null=Assert-OptimizedMWEditableSettingsPath $Session.path
    if(-not $Session.pending.Count){return [pscustomobject]@{saved=$false;changed=0;status='NoChanges';effective_state='NotObserved'}}
    $text=$Session.base_text
    foreach($entry in ($Session.pending.GetEnumerator() | Sort-Object Key)){
        $parts=$entry.Key -split "`n",2
        $text=Set-OptimizedMWSettingsTextValue $text $parts[0] $parts[1] $entry.Value $Session.newline
    }
    $encoding=[Text.UTF8Encoding]::new([bool]$Session.utf8_bom,$true)
    $payload=$encoding.GetPreamble()+$encoding.GetBytes($text)
    if($payload.Length -gt 1048576){throw 'Edited settings exceed 1 MiB'}
    $parent=Split-Path -Parent $Session.path
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    $lockPath=$Session.path+'.optimizedmw-editor.lock'
    $lock=$null;$originalHandle=$null;$temporary=$Session.path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        if((Test-Path -LiteralPath $lockPath) -and ((Get-Item -LiteralPath $lockPath -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)){
            throw 'Settings lock is a reparse point'
        }
        $lock=[IO.File]::Open($lockPath,[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
        $stream=[IO.File]::Open($temporary,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
        try{$stream.Write($payload,0,$payload.Length);$stream.Flush($true)}finally{$stream.Dispose()}
        if($BeforePublish){& $BeforePublish}
        $null=Assert-OptimizedMWEditableSettingsPath $Session.path
        $exists=[IO.File]::Exists($Session.path)
        if($exists){
            # Deny noncooperating writes until replacement. Delete sharing permits atomic File.Replace on Windows.
            $originalHandle=[IO.File]::Open($Session.path,[IO.FileMode]::Open,[IO.FileAccess]::Read,
                ([IO.FileShare]::Read -bor [IO.FileShare]::Delete))
        }
        if($exists -ne $Session.original_exists -or ($exists -and
            (Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($Session.path))) -cne $Session.original_sha256)){
            throw 'Profile changed outside this editor; cancel and reopen it before applying'
        }
        if($exists){[IO.File]::Replace($temporary,$Session.path,[Management.Automation.Language.NullString]::Value)}
        else{[IO.File]::Move($temporary,$Session.path)}
        $hash=Get-OptimizedMWSettingsBytesHash ([IO.File]::ReadAllBytes($Session.path))
        if($hash -cne (Get-OptimizedMWSettingsBytesHash $payload)){throw 'Settings read-back differs from the committed values'}
        $count=$Session.pending.Count;$Session.closed=$true
        return [pscustomobject][ordered]@{saved=$true;changed=$count;path=$Session.path;sha256=$hash;
            status='SavedForNextLaunch';apply_class='RestartRequired';effective_state='NotObserved';
            scope='Preferences only; no game or frozen benchmark inputs changed'}
    }finally{
        if($originalHandle){$originalHandle.Dispose()}
        if($lock){$lock.Dispose()}
        if([IO.File]::Exists($temporary)){[IO.File]::Delete($temporary)}
    }
}

function Test-OptimizedMWSettingsEffectiveSnapshot {
    param([object]$Session,[Parameter(Mandatory=$true)][string]$EffectiveSettingsPath,
        [ValidateSet('Custom','Benchmark','Gameplay')][string]$RunMode='Custom')
    if($RunMode -ceq 'Benchmark'){
        return [pscustomobject]@{qualified=$false;status='NotComparable_CustomIgnoredByBenchmark';mismatches=@();
            scope='File snapshot only; no running engine application proof'}
    }
    $bytes=[IO.File]::ReadAllBytes([IO.Path]::GetFullPath($EffectiveSettingsPath))
    if($bytes.Length -gt 1048576){throw 'Effective settings snapshot exceeds 1 MiB'}
    $encoding=[Text.UTF8Encoding]::new($false,$true)
    $offset=if($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191){3}else{0}
    $effective=Read-OptimizedMWSettingsValues ($encoding.GetString($bytes,$offset,$bytes.Length-$offset)) $Session.schema
    $mismatches=[Collections.Generic.List[object]]::new()
    foreach($definition in $Session.schema.settings){
        $id=$definition.section+"`n"+$definition.key
        if(-not ($Session.pending.ContainsKey($id) -or $Session.saved_values.ContainsKey($id))){continue}
        $requested=Get-OptimizedMWSettingsSessionValue $Session $definition
        $actual=if($effective.ContainsKey($id)){$effective[$id]}else{[string]$definition.default}
        try{
            $left=ConvertTo-OptimizedMWSettingsValue $definition $requested
            $right=ConvertTo-OptimizedMWSettingsValue $definition $actual
            if($left -ceq $right){continue}
        }catch{}
        $mismatches.Add([pscustomobject]@{section=$definition.section;key=$definition.key;requested=$requested;snapshot=$actual})
    }
    return [pscustomobject]@{qualified=$mismatches.Count -eq 0;
        status=$(if($mismatches.Count){'RequestedSnapshotMismatch'}else{'RequestedSnapshotMatches'});
        mismatches=@($mismatches.ToArray());scope='File snapshot only; no running engine application proof'}
}

function Close-OptimizedMWSettingsSession {
    param([object]$Session)
    $Session.pending.Clear();$Session.closed=$true
}

function Read-OptimizedMWEditorNumber {
    param([string]$Prompt,[int]$Maximum)
    while($true){
        $value=Read-Host $Prompt
        if($value -cmatch '^[0-9]+$'){
            $parsed=0
            if([int]::TryParse($value,[ref]$parsed) -and $parsed -ge 0 -and $parsed -le $Maximum){return $parsed}
        }
        Write-Host ('Choose a number from 0 to '+$Maximum) -ForegroundColor Yellow
    }
}

function Invoke-OptimizedMWSettingsEditor {
    param([string]$GameDirectory=$PSScriptRoot,[string]$Path='')
    $directory=[IO.Path]::GetFullPath($GameDirectory)
    if(-not $Path){$Path=Join-Path $directory 'Configuration\Optimizations.cfg'}
    $session=New-OptimizedMWSettingsSession -Path $Path -TemplatePath (Join-Path $directory 'OptimizedMW_Optimizations.cfg')
    try{
        while($true){
            Write-Host ('Custom preference profile: '+$session.path) -ForegroundColor Cyan
            Write-Host 'Saved values are requested for the next Custom launch. Active engine values have not been observed.'
            Write-Host 'Benchmark tests keep their frozen foundation and ignore these saved Custom preferences.'
            $categories=@($session.schema.settings.section | Select-Object -Unique)
            foreach($index in 0..($categories.Count-1)){Write-Host (' {0} = {1}' -f ($index+1),$categories[$index])}
            $review=$categories.Count+1;$defaults=$review+1
            Write-Host (' {0} = Review and Apply | {1} = Restore all engine defaults (pending) | 0 = Cancel edits' -f $review,$defaults)
            $choice=Read-OptimizedMWEditorNumber 'Settings category' $defaults
            if($choice -eq 0){return [pscustomobject]@{saved=$false;status='Cancelled';changed=0}}
            if($choice -eq $defaults){Reset-OptimizedMWSettingsPendingDefaults $session;continue}
            if($choice -eq $review){
                foreach($entry in ($session.pending.GetEnumerator() | Sort-Object Key)){
                    $parts=$entry.Key -split "`n",2;Write-Host (' ['+$parts[0]+'] '+$parts[1]+' = '+$entry.Value+' (restart required)')
                }
                if($session.pending.ContainsKey("Optimizations`nlegacy performance profile") -and $session.pending["Optimizations`nlegacy performance profile"] -ceq 'true'){
                    Write-Host 'Legacy profile is enabled: engine policy may impose other preset minima. This is not a memory ceiling.' -ForegroundColor Yellow
                }
                Write-Host ' 1 = Apply saved requests | 0 = Keep editing'
                if((Read-OptimizedMWEditorNumber 'Apply' 1) -eq 0){continue}
                try{return (Save-OptimizedMWSettingsSession $session)}catch{Write-Host $_.Exception.Message -ForegroundColor Red;continue}
            }
            $section=$categories[$choice-1]
            while($true){
                $definitions=@($session.schema.settings | Where-Object {$_.section -ceq $section})
                $rows=@(Get-OptimizedMWSettingsRows $session | Where-Object {$_.section -ceq $section})
                foreach($index in 0..($rows.Count-1)){
                    $row=$rows[$index]
                    Write-Host (' {0} = {1}: saved {2}; pending {3}{4}' -f ($index+1),$row.label,$row.saved_value,$row.pending_value,$(if($row.unsaved){' (unsaved; restart)'}else{''}))
                }
                $reset=$definitions.Count+1
                Write-Host (' {0} = Restore this category defaults (pending) | 0 = Back' -f $reset)
                $setting=Read-OptimizedMWEditorNumber 'Individual setting' $reset
                if($setting -eq 0){break}
                if($setting -eq $reset){Reset-OptimizedMWSettingsPendingDefaults $session $section;continue}
                $definition=$definitions[$setting-1];$row=$rows[$setting-1]
                Write-Host $row.reason
                if(-not $row.available){Write-Host 'This setting is inactive or unavailable in the current pending configuration.' -ForegroundColor Yellow;continue}
                Write-Host 'Saved only on Apply; this editor does not change a running game.'
                if($definition.type -ceq 'boolean'){
                    Write-Host ' 1 = Enabled | 2 = Disabled | 0 = Back'
                    $valueChoice=Read-OptimizedMWEditorNumber 'Value' 2
                    if($valueChoice -eq 0){continue};$value=if($valueChoice -eq 1){'true'}else{'false'}
                }elseif($definition.type -ceq 'enum'){
                    foreach($index in 0..($definition.choices.Count-1)){Write-Host (' {0} = {1}' -f ($index+1),$definition.choices[$index])}
                    Write-Host ' 0 = Back';$valueChoice=Read-OptimizedMWEditorNumber 'Value' $definition.choices.Count
                    if($valueChoice -eq 0){continue};$value=$definition.choices[$valueChoice-1]
                }else{
                    Write-Host (' Bounds: '+$definition.minimum+' to '+$definition.maximum+$(if($definition.exclusive_minimum){' (lower bound exclusive)'}else{''}))
                    $value=Read-Host 'Numeric value (Enter = cancel)';if(-not $value){continue}
                }
                try{Set-OptimizedMWSettingsPendingValue $session $section $definition.key $value}
                catch{Write-Host $_.Exception.Message -ForegroundColor Yellow}
            }
        }
    }finally{if(-not $session.closed){Close-OptimizedMWSettingsSession $session}}
}
