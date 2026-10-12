# Test the actual launcher writer against mixed legacy and canonical configurations.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'OptimizedMW_SettingNames.ps1')
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'OptimizedMW_Test.ps1'),[ref]$tokens,[ref]$errors)
if($errors.Count){throw ($errors | Out-String)}
$writer=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Set-IniValue'},$true)
. ([scriptblock]::Create($writer.Extent.Text))
$path=Join-Path ([IO.Path]::GetTempPath()) ('p9-setting-names-'+[Guid]::NewGuid().ToString('N')+'.cfg')
try{
    $ConfigurationMode='Benchmark'
    [IO.File]::WriteAllText($path,"[V3]`n# retained comment`nv3.6 performance profile = true`n[Optimizations]`nlegacy performance profile = true`n[Unrelated]`nkeep = exact`n")
    Set-IniValue $path 'V3' 'v3.6 performance profile' 'false'
    $text=[IO.File]::ReadAllText($path)
    if($text -match 'v3.6 performance profile' -or @([regex]::Matches($text,'(?m)^legacy performance profile = false\r?$')).Count -ne 1 -or
        $text -notmatch '# retained comment' -or $text -notmatch 'keep = exact'){throw 'Canonical writer lost precedence, duplicates, comments or unrelated values'}
    $before=[IO.File]::ReadAllBytes($path)
    $ConfigurationMode='Custom'
    Set-IniValue $path 'Cells' 'cache expiry delay' '1800'
    if([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne [Convert]::ToBase64String($before)){throw 'Custom mode applied a frozen benchmark setting'}
    $script:ApplyingCustomConfiguration=$true
    Set-IniValue $path 'Cells' 'opimizedmw transient budget mb' '192'
    $text=[IO.File]::ReadAllText($path)
    if($text -notmatch 'speculative transient budget mb = 192' -or $text -match 'opimizedmw'){throw 'Custom value was not applied under the descriptive key'}
    foreach($reverse in @($false,$true)){
        $parts=@("[Optimizations]`nlegacy performance profile = false`n","[V3]`nv3.6 performance profile = true`n")
        if($reverse){[array]::Reverse($parts)}
        [IO.File]::WriteAllText($path,($parts -join ''))
        $entries=@(Read-OptimizedMWConfigurationSettings $path)
        if($entries.Count -ne 1 -or $entries[0].Section -ne 'Optimizations' -or $entries[0].Value -ne 'false'){throw 'Custom input alias order changed canonical precedence'}
    }
    [IO.File]::WriteAllText($path,"[Cells]`ncache expiry delay = 120`ncache expiry delay = 60`n")
    $denied=$false;try{Read-OptimizedMWConfigurationSettings $path}catch{$denied=$true}
    if(-not $denied){throw 'Custom input accepted duplicate physical keys'}
    $ConfigurationMode='Benchmark';$script:ApplyingCustomConfiguration=$false
    $foundation="[Cells]`n# foundation preserved`npaging optimizer = true`npaging readiness split = true`nactive grid prepare mode = 1`nprepared chunk quality mode = 2`ngenerated static lod active grid = true`n[Camera]`nkeep = exact`n"
    foreach($assetMask in 0..31){
        [IO.File]::WriteAllText($path,$foundation)
        Set-OptimizedMWAssetSettings -Path $path -AssetMask $assetMask
        $text=[IO.File]::ReadAllText($path)
        $expected=if($assetMask -band 1){'true'}else{'false'}
        if(@([regex]::Matches($text,'(?m)^generated static lod active grid = '+$expected+'\r?$')).Count -ne 1){throw ('Active L1 selection/control mapping failed for mask '+$assetMask)}
        foreach($unchanged in @('paging optimizer = true','paging readiness split = true','active grid prepare mode = 1','prepared chunk quality mode = 2','# foundation preserved','keep = exact')){
            if(-not $text.Contains($unchanged)){throw ('Active L1 writer changed an independent prerequisite or unrelated value: '+$unchanged)}
        }
    }
    $ConfigurationMode='Custom'
    [IO.File]::WriteAllText($path,$foundation.Replace('generated static lod active grid = true','generated static lod active grid = false'))
    $before=[Convert]::ToBase64String([IO.File]::ReadAllBytes($path))
    Set-OptimizedMWAssetSettings -Path $path -AssetMask 1
    if([Convert]::ToBase64String([IO.File]::ReadAllBytes($path)) -cne $before){throw 'Asset selection overwrote an explicit Custom active-grid control'}
    $entries=@(Read-OptimizedMWConfigurationSettings (Join-Path $PSScriptRoot 'OptimizedMW_Optimizations.cfg'))
    $active=@($entries | Where-Object {$_.Section -ceq 'Cells' -and $_.Key -ceq 'generated static lod active grid'})
    if($active.Count -ne 1 -or $active[0].Value -cne 'false'){throw 'Editable active-grid setting is missing or silently enabled'}
    Write-Output 'PASS setting writer: canonical alias replacement, duplicate removal, comments and unrelated settings preserved, custom values bypass fixed benchmark controls; all32 asset vectors independently set active L1, control clears stale flags, Custom override and prerequisite values preserved'
}finally{Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue;$script:ApplyingCustomConfiguration=$false}
