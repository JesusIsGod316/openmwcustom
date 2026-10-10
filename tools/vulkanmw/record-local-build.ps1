param(
    [Parameter(Mandatory=$true)][string]$Package,
    [string]$Stage = 'Stage 1 object binding updates and retained populations'
)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../..')).Path
$taskPackage = (Resolve-Path -LiteralPath $Package).Path
$taskExe = Join-Path $taskPackage 'openmw.exe'
if (-not (Test-Path -LiteralPath $taskExe -PathType Leaf)) { throw 'Local package has no executable.' }
$taskEvidence = Join-Path $taskPackage 'Benchmarks'
if ((Test-Path -LiteralPath $taskEvidence) -and (Get-ChildItem -LiteralPath $taskEvidence -Recurse -File | Select-Object -First 1)) {
    throw 'Package already has benchmark evidence. Do not rewrite its source manifest.'
}
$taskLaunchers = @(Get-ChildItem -LiteralPath $taskPackage -File | Where-Object { $_.Extension -in @('.bat', '.cmd') })
if ($taskLaunchers.Count -ne 2 -or @($taskLaunchers.Name | Where-Object { $_ -notin @('START-VulkanMW-Test.bat','START-VulkanMW-Benchmark.bat') }).Count) {
    throw 'Package must have exactly the two public VulkanMW launchers.'
}
Push-Location $taskRoot
try {
    $taskHead = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read source HEAD.' }
    $taskBranch = (& git branch --show-current).Trim()
    $taskPaths = @(& git diff --name-only HEAD)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate changed source.' }
    $taskPaths += @(& git ls-files --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate untracked source.' }
    $taskChanges = [ordered]@{}
    $taskDocuments = [ordered]@{}
    foreach ($taskPath in $taskPaths | Sort-Object -Unique) {
        $taskFile = Join-Path $taskRoot $taskPath
        if (-not (Test-Path -LiteralPath $taskFile -PathType Leaf)) { throw "Changed file is missing: $taskPath" }
        $taskHash = (Get-FileHash -LiteralPath $taskFile -Algorithm SHA256).Hash.ToLowerInvariant()
        if ([IO.Path]::GetExtension($taskPath) -eq '.md') { $taskDocuments[$taskPath] = $taskHash }
        else { $taskChanges[$taskPath] = $taskHash }
    }
    $taskIdentity = [ordered]@{
        schema = 1
        source_head = $taskHead
        source_branch = $taskBranch
        source_head_is_base_only = ($taskChanges.Count -ne 0)
        source_tree_dirty = ($taskPaths.Count -ne 0)
        source_identity_basis = 'Local worktree on the recorded base; changed source and documentation are hashed separately. No benchmark promotion is inferred.'
        created_utc = [DateTime]::UtcNow.ToString('o')
        executable_sha256 = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
        changed_source_files = $taskChanges
        checkpoint_documents = $taskDocuments
        stage = $Stage
        build_type = 'Release'
        build_system = 'Ninja'
        compiler = 'MSVC 19.44.35228 (VS2022 Build Tools)'
        lto = $false
        installed_launchers = @('START-VulkanMW-Test.bat','START-VulkanMW-Benchmark.bat')
        default_launcher_feature = 'OPENMW_VK_OBJECT_BINDING_UPDATES=1 and OPENMW_VK_PERSISTENT_POPULATIONS=1; engine defaults remain off'
        runtime_promotion = 'Pending; source checks and runtime smoke are not a performance or exhaustive compatibility verdict'
    }
    $taskIdentity | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $taskPackage 'source-changes.json') -Encoding utf8
    Write-Output "Recorded local base $taskHead and $($taskChanges.Count) changed source files in $taskPackage."
    Write-Output "Executable SHA256: $($taskIdentity.executable_sha256)"
} finally { Pop-Location }
