# Uses the same curated aliases as the engine's generated C++ table.
function Resolve-OptimizedMWSettingName {
    param([string]$Section,[string]$Key)
    if($null -eq $script:OptimizationSettingAliases){
        $path=Join-Path $PSScriptRoot 'optimization-settings-aliases.json'
        # Source tests use the authority directly; installed packages carry its exact copy.
        if(-not [IO.File]::Exists($path)){$path=Join-Path $PSScriptRoot '../../../files/optimization-settings-aliases.json'}
        $document=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if($document.schema_version -ne 1){throw 'Unsupported optimization settings alias schema'}
        $rows=$document.aliases
        $script:OptimizationSettingAliases=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
        foreach($row in $rows){
            $script:OptimizationSettingAliases.Add($row.old_section+"`n"+$row.old_key,
                [pscustomobject]@{Section=[string]$row.new_section;Key=[string]$row.new_key})
        }
    }
    $id=$Section+"`n"+$Key
    if($script:OptimizationSettingAliases.ContainsKey($id)){return $script:OptimizationSettingAliases[$id]}
    return [pscustomobject]@{Section=$Section;Key=$Key}
}
function Read-OptimizedMWConfigurationSettings {
    param([Parameter(Mandatory=$true)][string]$Path)
    if((Get-Item -LiteralPath $Path).Length -gt 1048576){throw 'Optimization configuration exceeds 1 MiB'}
    $raw=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
    $section=''
    foreach($line in [IO.File]::ReadAllLines($Path)){
        $text=$line.Trim()
        if(-not $text -or $text.StartsWith('#') -or $text.StartsWith(';')){continue}
        if($text -match '^\[([^\]]+)\]$'){$section=$matches[1].Trim();continue}
        if(-not $section -or $text -notmatch '^([^=]+?)\s*=\s*(.*)$'){throw ('Invalid custom optimization setting: '+$text)}
        $key=$matches[1].Trim();$value=$matches[2].Trim();$id=$section+"`n"+$key
        if($raw.ContainsKey($id)){throw ('Duplicate custom optimization setting: ['+$section+'] '+$key)}
        $raw.Add($id,[pscustomobject]@{Section=$section;Key=$key;Value=$value})
    }
    $result=[Collections.Generic.List[object]]::new()
    foreach($entry in $raw.Values){
        $canonical=Resolve-OptimizedMWSettingName $entry.Section $entry.Key
        if(($canonical.Section -cne $entry.Section -or $canonical.Key -cne $entry.Key) -and
            $raw.ContainsKey($canonical.Section+"`n"+$canonical.Key)){continue}
        $result.Add([pscustomobject]@{Section=$canonical.Section;Key=$canonical.Key;Value=$entry.Value})
    }
    return @($result | Sort-Object Section,Key)
}
function Set-OptimizedMWAssetSettings {
    param([Parameter(Mandatory=$true)][string]$Path,[ValidateRange(0,31)][int]$AssetMask=0)
    # The existing benchmark foundation owns readiness/prepare/quality. This
    # independently selects active-grid L1 and clears an inherited flag on its
    # control. Set-IniValue preserves explicit Custom configuration precedence.
    $activeLod=if($AssetMask -band 1){'true'}else{'false'}
    Set-IniValue $Path 'Cells' 'generated static lod active grid' $activeLod
}
