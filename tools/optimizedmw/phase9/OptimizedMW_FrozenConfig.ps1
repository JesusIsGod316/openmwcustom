# Manual frozen-config support only. No route automation, game launch, or writes
# to the gameplay profile belong in this library.
function Assert-OptimizedMWPlainPath {
    param([Parameter(Mandatory=$true)][string]$Path)
    $absolute=[IO.Path]::GetFullPath($Path)
    $cursor=$absolute
    while($cursor){
        if(Test-Path -LiteralPath $cursor){
            $item=Get-Item -LiteralPath $cursor -Force
            if(($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Frozen configuration refuses a reparse point: $cursor"}
        }
        $parent=[IO.Path]::GetDirectoryName($cursor)
        if($parent -eq $cursor){break}
        $cursor=$parent
    }
    return $absolute
}

function Get-OptimizedMWFrozenFile {
    param([Parameter(Mandatory=$true)][string]$Path)
    $absolute=Assert-OptimizedMWPlainPath $Path
    if(-not [IO.File]::Exists($absolute)){throw "Frozen configuration file missing: $absolute"}
    $file=Get-Item -LiteralPath $absolute -Force
    return [pscustomobject]@{path=$absolute;sha256=(Get-FileHash -LiteralPath $absolute -Algorithm SHA256).Hash.ToLowerInvariant();bytes=$file.Length}
}

function Read-OptimizedMWFrozenLock {
    param([Parameter(Mandatory=$true)][string]$Directory)
    $directory=Assert-OptimizedMWPlainPath $Directory
    if(-not [IO.Directory]::Exists($directory)){throw 'Frozen configuration directory missing'}
    $lockIdentity=Get-OptimizedMWFrozenFile (Join-Path $directory 'LOCK.json')
    if($lockIdentity.bytes -gt 1048576){throw 'Frozen lock exceeds its bounded 1 MiB size'}
    $lock=[IO.File]::ReadAllText($lockIdentity.path) | ConvertFrom-Json
    if($lock.schema -ne 1){throw 'Unsupported frozen lock schema'}
    $files=@($lock.files)
    if($files.Count -lt 4 -or $files.Count -gt 4096){throw 'Frozen configuration lock requires a bounded config/settings/save inventory'}
    $checked=[Collections.Generic.List[object]]::new()
    $names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($entry in $files){
        $relative=[string]$entry.relative_path
        if(-not $relative -or [IO.Path]::IsPathRooted($relative) -or
            $relative -match '(^|[\\/])\.\.?([\\/]|$)|:|[\r\n]|(^|[\\/])[^\\/]*[. ]([\\/]|$)'){
            throw 'Frozen lock contains an unsafe relative path'
        }
        $normalized=$relative.Replace('/','\')
        if(-not $names.Add($normalized)){throw 'Frozen lock contains duplicate file paths'}
        $path=[IO.Path]::GetFullPath((Join-Path $directory $normalized))
        if(-not $path.StartsWith($directory.TrimEnd('\','/')+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Frozen lock path escapes its directory'}
        $identity=Get-OptimizedMWFrozenFile $path
        if([string]$entry.sha256 -notmatch '^[a-fA-F0-9]{64}$' -or $identity.sha256 -cne ([string]$entry.sha256).ToLowerInvariant() -or
            $null -eq $entry.bytes -or [long]$entry.bytes -ne $identity.bytes){throw "Frozen configuration hash/length mismatch: $relative"}
        $checked.Add([pscustomobject]@{relative_path=$normalized;path=$path;sha256=$identity.sha256;bytes=$identity.bytes})
    }
    foreach($required in @('openmw.cfg','settings.cfg','Optimizations.cfg')){
        if(-not $names.Contains($required)){throw "Frozen lock lacks $required"}
    }
    $saves=@($checked | Where-Object {$_.relative_path -match '^saves\\[^\\]+\\[^\\]+\.omwsave$'})
    if($saves.Count -ne 1){throw 'Frozen lock must identify exactly one copied character save'}
    foreach($property in @('source_configuration_directory','source_user_data_directory')){
        if(-not $lock.PSObject.Properties[$property] -or -not [IO.Path]::IsPathRooted([string]$lock.$property)){
            throw "Frozen lock requires an absolute $property for historical path resolution"
        }
    }
    # A changed lock during validation is not a new accepted baseline.
    if((Get-OptimizedMWFrozenFile $lockIdentity.path).sha256 -cne $lockIdentity.sha256){throw 'Frozen lock changed during verification'}
    return [pscustomobject]@{directory=$directory;lock=$lock;lock_identity=$lockIdentity;files=$checked.ToArray();save=$saves[0]}
}

function ConvertFrom-OptimizedMWConfigPath {
    param([Parameter(Mandatory=$true)][AllowEmptyString()][string]$Value)
    $value=$Value.Trim()
    if(-not $value.StartsWith('"')){return $value}
    # OpenMW quoted paths use ampersand as their escape character, not slash.
    $builder=[Text.StringBuilder]::new();$closed=$false
    for($index=1;$index -lt $value.Length;++$index){
        $character=$value[$index]
        if($character -eq '&'){
            ++$index
            if($index -ge $value.Length){throw 'Truncated quoted config path'}
            [void]$builder.Append($value[$index])
        }elseif($character -eq '"'){
            if($value.Substring($index+1).Trim()){throw 'Trailing content after frozen config path'}
            $closed=$true;break
        }else{[void]$builder.Append($character)}
    }
    if(-not $closed){throw 'Unterminated quoted config path'}
    return $builder.ToString()
}

function ConvertTo-OptimizedMWConfigPath {
    param([Parameter(Mandatory=$true)][string]$Value)
    return '"'+$Value.Replace('\','/').Replace('&','&&').Replace('"','&"')+'"'
}

function Resolve-OptimizedMWFrozenDataPath {
    param([string]$Value,[object]$Baseline,[string]$GameDir)
    $value=ConvertFrom-OptimizedMWConfigPath $Value
    $tokens=@{'?userconfig?'=[string]$Baseline.lock.source_configuration_directory;
        '?userdata?'=[string]$Baseline.lock.source_user_data_directory;'?local?'=$GameDir}
    foreach($token in $tokens.Keys){$value=$value.Replace($token,$tokens[$token].TrimEnd('\','/')+'/')}
    if(-not $value -or $value.Contains('?') -or $value -match '[\r\n]|^[a-zA-Z]:[^\\/]'){throw 'Unsupported or unresolved frozen data path'}
    if(-not [IO.Path]::IsPathRooted($value)){$value=Join-Path ([string]$Baseline.lock.source_configuration_directory) $value}
    $absolute=[IO.Path]::GetFullPath($value)
    if(-not [IO.Directory]::Exists($absolute)){throw "Frozen mod data directory missing: $absolute"}
    return $absolute
}

function Test-OptimizedMWFrozenSaintAssets {
    param([object]$Baseline,[string[]]$OrderedData,[switch]$VerifyHashes)
    $entry=@($Baseline.files | Where-Object {$_.relative_path -ceq 'SAINTJS-ASSETS.json'})
    if($entry.Count -ne 1){throw 'Frozen SaintJs configuration lacks its locked asset manifest'}
    if($entry[0].bytes -gt 16777216){throw 'SaintJs asset manifest exceeds its bounded 16 MiB size'}
    $assets=[IO.File]::ReadAllText($entry[0].path) | ConvertFrom-Json
    if($assets.schema -ne 1){throw 'Unsupported SaintJs asset manifest schema'}
    $directories=@($assets.ordered_data_paths | ForEach-Object {[IO.Path]::GetFullPath([string]$_)})
    if(-not $directories.Count -or -not @($assets.files).Count){throw 'Empty SaintJs asset manifest'}
    $actualSaint=@($OrderedData | Where-Object {$_.Replace('\','/') -match '/saintjs/'})
    if($actualSaint.Count -ne $directories.Count){throw 'SaintJs data-directory count differs from the locked manifest'}
    for($index=0;$index -lt $directories.Count;++$index){
        $directory=Assert-OptimizedMWPlainPath $directories[$index]
        if(-not [IO.Directory]::Exists($directory) -or $directory -ine $actualSaint[$index]){throw 'SaintJs ordered data directories differ or are missing'}
    }
    $known=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $count=0;$bytes=0L
    foreach($asset in @($assets.files)){
        $directory=[IO.Path]::GetFullPath([string]$asset.data_directory)
        $relative=[string]$asset.relative_path
        if($directories -inotcontains $directory -or -not $relative -or [IO.Path]::IsPathRooted($relative) -or
            $relative -match '(^|[\\/])\.\.?([\\/]|$)|:|[\r\n]|(^|[\\/])[^\\/]*[. ]([\\/]|$)'){
            throw 'SaintJs manifest contains an unsafe asset path'
        }
        $path=Assert-OptimizedMWPlainPath (Join-Path $directory $relative.Replace('/','\'))
        if($path -ine [IO.Path]::GetFullPath([string]$asset.path) -or -not $known.Add($path) -or -not [IO.File]::Exists($path)){
            throw 'SaintJs manifest asset missing, duplicated, or outside its directory'
        }
        $file=Get-Item -LiteralPath $path -Force
        if($null -eq $asset.bytes -or $file.Length -ne [long]$asset.bytes){throw "SaintJs asset length changed: $path"}
        if($VerifyHashes -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ine [string]$asset.sha256){throw "SaintJs asset hash changed: $path"}
        ++$count;$bytes+=$file.Length
    }
    return [pscustomobject]@{manifest_identity=$entry[0];file_count=$count;bytes=$bytes;ordered_data_paths=$directories;
        verification=$(if($VerifyHashes){'full_sha256_and_length'}else{'path_and_length_only_same_size_changes_not_detected'});
        cache_state='Verification reads filesystem metadata; optional full hashing reads all SaintJs payloads and warms OS caches. Caches remain uncontrolled.';
        scope='Recorded SaintJs payloads only; added files and all other mod payloads are not frozen.'}
}

function New-OptimizedMWFrozenRuntime {
    param([Parameter(Mandatory=$true)][string]$Directory,[Parameter(Mandatory=$true)][string]$GameDir,[switch]$VerifyAssetHashes)
    $baseline=Read-OptimizedMWFrozenLock $Directory
    $game=Assert-OptimizedMWPlainPath $GameDir
    $sessions=Assert-OptimizedMWPlainPath (Join-Path $game 'TestSessions')
    $runtime=Join-Path $sessions ('FrozenConfig_'+[Guid]::NewGuid().ToString('N'))
    if(Test-Path -LiteralPath $runtime){throw 'Preserving an existing frozen runtime directory'}
    # Validate/resolve the complete read-only input before creating any outputs.
    $lines=[Collections.Generic.List[string]]::new()
    $orderedData=[Collections.Generic.List[string]]::new()
    $originalLocal=$null
    $removed=[Collections.Generic.List[string]]::new()
    $sourceConfig=Join-Path $baseline.directory 'openmw.cfg'
    if((Get-Item -LiteralPath $sourceConfig).Length -gt 8388608){throw 'Frozen openmw.cfg exceeds its bounded 8 MiB size'}
    foreach($line in [IO.File]::ReadAllLines($sourceConfig)){
        if($line -notmatch '^\s*([\w-]+)\s*=\s*(.*)$'){$lines.Add($line);continue}
        $key=$matches[1].ToLowerInvariant();$value=$matches[2]
        if($key -in @('config','user-data','resources','load-savegame','script-run','replace','start','skip-menu','new-game')){
            $removed.Add($key);continue
        }
        if($key -in @('data','data-local')){
            $resolved=Resolve-OptimizedMWFrozenDataPath $value $baseline $game
            if($key -eq 'data-local'){$originalLocal=$resolved;continue}
            $orderedData.Add($resolved)
            $lines.Add('data='+(ConvertTo-OptimizedMWConfigPath $resolved));continue
        }
        $lines.Add($line)
    }
    # replace=data removes the package's ordinary low-priority game scripts.
    # Retain that explicit VFS source before the captured mod directories.
    $packageData=Join-Path $game 'resources\vfs-mw'
    if(-not [IO.Directory]::Exists($packageData)){throw 'Installed package resources/vfs-mw directory missing'}
    if($orderedData -inotcontains $packageData){
        $orderedData.Insert(0,$packageData)
        $lines.Insert(0,'data='+(ConvertTo-OptimizedMWConfigPath $packageData))
    }
    # The package supplies ?userdata?data as its default high-priority local
    # assets. Preserve that read-only asset source, but direct local writes to
    # a new private directory. No shared user config is consulted.
    if(-not $originalLocal){
        $candidate=Join-Path ([string]$baseline.lock.source_user_data_directory) 'data'
        if([IO.Directory]::Exists($candidate)){$originalLocal=[IO.Path]::GetFullPath($candidate)}
    }
    if($originalLocal){$lines.Add('data='+(ConvertTo-OptimizedMWConfigPath $originalLocal));$orderedData.Add($originalLocal)}
    $assetVerification=Test-OptimizedMWFrozenSaintAssets -Baseline $baseline -OrderedData $orderedData.ToArray() -VerifyHashes:$VerifyAssetHashes
    # Scalar fallback keys continue composing with package defaults, just as
    # the original user config did. Its captured keys override matching keys.
    foreach($key in @('config','data','content','groundcover','fallback-archive')){$lines.Add('replace='+$key)}
    $userData=Join-Path $runtime 'UserData';$localData=Join-Path $userData 'data'
    $resources=Join-Path $game 'resources'
    if(-not [IO.Directory]::Exists($resources)){throw 'Installed package resources directory missing'}
    $lines.Add('user-data='+(ConvertTo-OptimizedMWConfigPath $userData))
    $lines.Add('data-local='+(ConvertTo-OptimizedMWConfigPath $localData))
    $lines.Add('resources='+(ConvertTo-OptimizedMWConfigPath $resources))
    $copiedSave=Join-Path $userData $baseline.save.relative_path
    [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($copiedSave))
    [void][IO.Directory]::CreateDirectory($localData)
    [IO.File]::WriteAllLines((Join-Path $runtime 'openmw.cfg'),$lines,[Text.UTF8Encoding]::new($false))
    $copiedFiles=[Collections.Generic.List[string]]::new()
    foreach($file in @('settings.cfg','Optimizations.cfg','global_storage.bin','player_storage.bin','input_v3.xml','shaders.yaml')){
        if(@($baseline.files | Where-Object {$_.relative_path -ceq $file}).Count){
            [IO.File]::Copy((Join-Path $baseline.directory $file),(Join-Path $runtime $file));$copiedFiles.Add($file)
        }
    }
    [IO.File]::Copy($baseline.save.path,$copiedSave)
    # Detect source changes during copying; compare every private original.
    $after=Read-OptimizedMWFrozenLock $Directory
    if($after.lock_identity.sha256 -cne $baseline.lock_identity.sha256){throw 'Frozen baseline changed while creating the runtime'}
    $copiedIdentities=[Collections.Generic.List[object]]::new()
    foreach($file in $copiedFiles){
        $expected=@($baseline.files | Where-Object {$_.relative_path -ceq $file})[0]
        $identity=Get-OptimizedMWFrozenFile (Join-Path $runtime $file)
        if($identity.sha256 -cne $expected.sha256){throw 'Frozen runtime copy identity mismatch'}
        $copiedIdentities.Add($identity)
    }
    if((Get-OptimizedMWFrozenFile $copiedSave).sha256 -cne $baseline.save.sha256){throw 'Copied save identity mismatch'}
    return [pscustomobject]@{directory=$runtime;user_data_directory=$userData;profiles_root=(Join-Path $runtime 'Captures');
        settings_path=(Join-Path $runtime 'settings.cfg');config_path=(Join-Path $runtime 'openmw.cfg');
        optimization_path=(Join-Path $runtime 'Optimizations.cfg');save_path=$copiedSave;
        baseline_directory=$baseline.directory;baseline_lock=$baseline.lock_identity;baseline_files=$baseline.files;
        config_identity=(Get-OptimizedMWFrozenFile (Join-Path $runtime 'openmw.cfg'));
        copied_save_identity=(Get-OptimizedMWFrozenFile $copiedSave);ordered_data_paths=$orderedData.ToArray();
        original_data_local=$originalLocal;removed_runtime_options=$removed.ToArray();resources_directory=$resources;
        copied_input_identities=$copiedIdentities.ToArray();saintjs_assets=$assetVerification;
        isolation='Private writable settings/config/log/dumps/saves/screenshots; original mod data read only. Fixed Windows renderer cache remains shared.';
        mode='manual_selection_and_gameplay_only_no_automatic_route'}
}

function Get-OptimizedMWFrozenArguments {
    param([Parameter(Mandatory=$true)][object]$Runtime)
    # Start-Process joins ArgumentList on Windows PowerShell 5.1. Supply quotes
    # explicitly; reject embedded quotes/newlines rather than shell-interpolate.
    foreach($path in @($Runtime.directory,$Runtime.user_data_directory,$Runtime.resources_directory)){
        if([string]$path -match '["\r\n]' -or [string]$path -match '[\\/]$'){throw 'Unsupported frozen runtime argument path'}
    }
    return @('--replace=config','--config',('"'+$Runtime.directory+'"'),'--user-data',('"'+$Runtime.user_data_directory+'"'),
        '--resources',('"'+$Runtime.resources_directory+'"'))
}

function Add-OptimizedMWFrozenManifest {
    param([Parameter(Mandatory=$true)][object]$Manifest,[Parameter(Mandatory=$true)][object]$Runtime)
    $baseline=Read-OptimizedMWFrozenLock $Runtime.baseline_directory
    if($baseline.lock_identity.sha256 -cne $Runtime.baseline_lock.sha256){throw 'Frozen baseline lock changed after runtime preparation'}
    if((Get-OptimizedMWFrozenFile $Runtime.config_path).sha256 -cne $Runtime.config_identity.sha256 -or
        (Get-OptimizedMWFrozenFile $Runtime.save_path).sha256 -cne $Runtime.copied_save_identity.sha256){throw 'Prepared frozen config/save changed before launch'}
    $assets=Test-OptimizedMWFrozenSaintAssets -Baseline $baseline -OrderedData $Runtime.ordered_data_paths
    foreach($copiedInput in $Runtime.copied_input_identities){
        # Settings are intentionally modified by the selected manual candidate.
        if([IO.Path]::GetFileName($copiedInput.path) -ne 'settings.cfg' -and (Get-OptimizedMWFrozenFile $copiedInput.path).sha256 -cne $copiedInput.sha256){throw 'Private frozen input changed before launch'}
    }
    $receipt=[ordered]@{baseline_directory=$Runtime.baseline_directory;baseline_lock=$Runtime.baseline_lock;
        baseline_files=$Runtime.baseline_files;runtime_directory=$Runtime.directory;user_data_directory=$Runtime.user_data_directory;
        effective_config=(Get-OptimizedMWFrozenFile $Runtime.config_path);copied_save=$Runtime.copied_save_identity;
        ordered_data_paths=$Runtime.ordered_data_paths;original_data_local=$Runtime.original_data_local;
        resources_directory=$Runtime.resources_directory;removed_runtime_options=$Runtime.removed_runtime_options;
        arguments=(Get-OptimizedMWFrozenArguments $Runtime);isolation=$Runtime.isolation;mode=$Runtime.mode;
        copied_input_identities=$Runtime.copied_input_identities;saintjs_assets_at_prepare=$Runtime.saintjs_assets;saintjs_assets_before_launch=$assets;
        save_load_status='copied_save_available_in_private_load_menu_no_automatic_load'}
    $Manifest | Add-Member -NotePropertyName frozen_configuration -NotePropertyValue $receipt -Force
    return $Manifest
}
