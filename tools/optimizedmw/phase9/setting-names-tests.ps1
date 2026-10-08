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
    Write-Output 'PASS setting writer: canonical alias replacement, duplicate removal, comments and unrelated settings preserved, custom values bypass fixed benchmark controls'
}finally{Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue;$script:ApplyingCustomConfiguration=$false}
