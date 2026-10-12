# Host-side diagnostic journal. These observations are not gameplay measurements
# or complete heap/VRAM attribution. Call once per second from the owning launcher.

function Initialize-OptimizedMWMemoryJournalNative {
    if ('OptimizedMW.MemoryJournalNative' -as [type]) { return }
    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'System memory journaling requires Windows.'
    }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace OptimizedMW {
    public static class MemoryJournalNative {
        [StructLayout(LayoutKind.Sequential)]
        public struct PerformanceInformation {
            public UInt32 cb;
            public UIntPtr CommitTotal, CommitLimit, CommitPeak;
            public UIntPtr PhysicalTotal, PhysicalAvailable, SystemCache;
            public UIntPtr KernelTotal, KernelPaged, KernelNonpaged, PageSize;
            public UInt32 HandleCount, ProcessCount, ThreadCount;
        }
        [StructLayout(LayoutKind.Sequential)]
        public struct MemoryStatus {
            public UInt32 dwLength, dwMemoryLoad;
            public UInt64 ullTotalPhys, ullAvailPhys, ullTotalPageFile, ullAvailPageFile;
            public UInt64 ullTotalVirtual, ullAvailVirtual, ullAvailExtendedVirtual;
        }
        [DllImport("psapi.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GetPerformanceInfo(ref PerformanceInformation value, UInt32 size);
        [DllImport("kernel32.dll", SetLastError=true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool GlobalMemoryStatusEx(ref MemoryStatus value);
    }
}
'@ -ErrorAction Stop | Out-Null
}

function Start-OptimizedMWMemoryJournal {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)][ValidateNotNullOrEmpty()][string]$Directory,
        [switch]$IncludeSystem
    )
    if (-not [IO.Directory]::Exists($Directory)) { throw "Memory journal directory must already exist: $Directory" }
    $resolvedDirectory = [IO.Path]::GetFullPath($Directory)
    $processPath = [IO.Path]::Combine($resolvedDirectory, 'process-memory.csv')
    $systemPath = [IO.Path]::Combine($resolvedDirectory, 'system-memory.csv')
    $statusPath = [IO.Path]::Combine($resolvedDirectory, 'memory-journal-status.json')
    foreach ($path in @($processPath, $statusPath)) {
        if ([IO.File]::Exists($path) -or [IO.Directory]::Exists($path)) { throw "Refusing to overwrite memory journal: $path" }
    }
    if ($IncludeSystem) {
        if ([IO.File]::Exists($systemPath) -or [IO.Directory]::Exists($systemPath)) { throw "Refusing to overwrite memory journal: $systemPath" }
        Initialize-OptimizedMWMemoryJournalNative
    }
    $journal = [pscustomobject]@{
        Directory=$resolvedDirectory; ProcessPath=$processPath; SystemPath=$systemPath; StatusPath=$statusPath
        IncludeSystem=[bool]$IncludeSystem; ProcessWriter=$null; SystemWriter=$null
        MaxSamples=8192; Attempts=[long]0; AcceptedAttempts=[long]0; Dropped=[long]0
        ProcessRows=[long]0; SystemRows=[long]0; Errors=[long]0; ProcessSampleErrors=[long]0
        SystemSampleErrors=[long]0; WriteErrors=[long]0; Stopped=$false
        FirstEpochMs=$null; LastEpochMs=$null; LastPressureSample=$null
        ErrorMessages=(New-Object 'System.Collections.Generic.List[string]')
    }
    $encoding = New-Object System.Text.UTF8Encoding($false)
    $stream = $null
    try {
        $stream = [IO.FileStream]::new($processPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $journal.ProcessWriter = [IO.StreamWriter]::new($stream, $encoding)
        $stream = $null
        $journal.ProcessWriter.AutoFlush = $true
        $journal.ProcessWriter.WriteLine('epoch_ms,elapsed_s,working_set_mb,private_mb,virtual_mb,cpu_total_s')
        if ($IncludeSystem) {
            $stream = [IO.FileStream]::new($systemPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
            $journal.SystemWriter = [IO.StreamWriter]::new($stream, $encoding)
            $stream = $null
            $journal.SystemWriter.AutoFlush = $true
            $journal.SystemWriter.WriteLine('epoch_ms,elapsed_s,performance_valid,physical_valid,process_valid,system_commit_mb,system_commit_limit_mb,system_commit_peak_mb,physical_available_mb,physical_total_mb,process_peak_working_set_mb,process_peak_virtual_mb')
        }
        return $journal
    }
    catch {
        foreach ($writer in @($journal.ProcessWriter, $journal.SystemWriter)) {
            if ($null -ne $writer) { try { $writer.Dispose() } catch {} }
        }
        if ($null -ne $stream) { try { $stream.Dispose() } catch {} }
        # Preserve any partial headers; callers must use a fresh private directory.
        throw
    }
}

function Add-OptimizedMWMemoryJournalError {
    param($Journal, [string]$Kind, [string]$Message)
    $Journal.Errors++
    if ($Kind -eq 'process') { $Journal.ProcessSampleErrors++ }
    elseif ($Kind -eq 'system') { $Journal.SystemSampleErrors++ }
    else { $Journal.WriteErrors++ }
    if ($Journal.ErrorMessages.Count -lt 8) {
        $bounded = if ($Message.Length -gt 512) { $Message.Substring(0,512) } else { $Message }
        $Journal.ErrorMessages.Add($Kind + ': ' + $bounded)
    }
}

function Add-OptimizedMWMemorySample {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory=$true)]$Journal,
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][DateTimeOffset]$Now,
        [Parameter(Mandatory=$true)][DateTimeOffset]$Start
    )
    if ($Journal.Stopped) { throw 'Memory journal is already stopped.' }
    $Journal.Attempts++
    if ($Journal.AcceptedAttempts -ge $Journal.MaxSamples) { $Journal.Dropped++; return }
    $Journal.AcceptedAttempts++
    $epoch = $Now.ToUnixTimeMilliseconds()
    $elapsed = ($Now - $Start).TotalSeconds
    $culture = [Globalization.CultureInfo]::InvariantCulture
    $format = 'F6'
    $epochText = $epoch.ToString($culture)
    $elapsedText = $elapsed.ToString($format,$culture)
    if ($null -eq $Journal.FirstEpochMs) { $Journal.FirstEpochMs=$epoch }
    $Journal.LastEpochMs=$epoch
    $processValid=$false; $peakWorking=0.0; $peakVirtual=0.0
    try {
        if ($Process.Id -le 0 -or $Process.HasExited) { throw 'Process identity is unavailable or the owned process has exited.' }
        $Process.Refresh()
        $working = [double]$Process.WorkingSet64 / 1048576.0
        $private = [double]$Process.PrivateMemorySize64 / 1048576.0
        $virtual = [double]$Process.VirtualMemorySize64 / 1048576.0
        $cpu = $Process.TotalProcessorTime.TotalSeconds
        $peakWorking = [double]$Process.PeakWorkingSet64 / 1048576.0
        $peakVirtual = [double]$Process.PeakVirtualMemorySize64 / 1048576.0
        $processValid=$true
    }
    catch { Add-OptimizedMWMemoryJournalError $Journal 'process' $_.Exception.Message }
    if ($processValid) {
        try {
            $row = @($epochText,$elapsedText,$working.ToString($format,$culture),$private.ToString($format,$culture),
                $virtual.ToString($format,$culture),$cpu.ToString($format,$culture)) -join ','
            $Journal.ProcessWriter.WriteLine($row)
            $Journal.ProcessRows++
        }
        catch { Add-OptimizedMWMemoryJournalError $Journal 'write' $_.Exception.Message }
    }
    if ($Journal.IncludeSystem) {
        $performanceValid=$false; $physicalValid=$false
        $commit=0.0; $limit=0.0; $peak=0.0; $available=0.0; $total=0.0
        try {
            $performance=New-Object OptimizedMW.MemoryJournalNative+PerformanceInformation
            $performance.cb=[Runtime.InteropServices.Marshal]::SizeOf($performance)
            $performanceValid=[OptimizedMW.MemoryJournalNative]::GetPerformanceInfo([ref]$performance,$performance.cb)
            if ($performanceValid) {
                $page=[double]$performance.PageSize.ToUInt64() / 1048576.0
                $commit=[double]$performance.CommitTotal.ToUInt64()*$page
                $limit=[double]$performance.CommitLimit.ToUInt64()*$page
                $peak=[double]$performance.CommitPeak.ToUInt64()*$page
            }
            else { Add-OptimizedMWMemoryJournalError $Journal 'system' ('GetPerformanceInfo error '+[Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
            $memory=New-Object OptimizedMW.MemoryJournalNative+MemoryStatus
            $memory.dwLength=[Runtime.InteropServices.Marshal]::SizeOf($memory)
            $physicalValid=[OptimizedMW.MemoryJournalNative]::GlobalMemoryStatusEx([ref]$memory)
            if ($physicalValid) { $available=[double]$memory.ullAvailPhys/1048576.0; $total=[double]$memory.ullTotalPhys/1048576.0 }
            else { Add-OptimizedMWMemoryJournalError $Journal 'system' ('GlobalMemoryStatusEx error '+[Runtime.InteropServices.Marshal]::GetLastWin32Error()) }
        }
        catch { Add-OptimizedMWMemoryJournalError $Journal 'system' $_.Exception.Message }
        $Journal.LastPressureSample=[pscustomobject]@{
            EpochMs=$epoch; PerformanceValid=$performanceValid; PhysicalValid=$physicalValid
            CommitSpareMiB=($limit-$commit); PhysicalAvailableMiB=$available
        }
        try {
            $row=@($epochText,$elapsedText,([int]$performanceValid).ToString($culture),([int]$physicalValid).ToString($culture),
                ([int]$processValid).ToString($culture),$commit.ToString($format,$culture),$limit.ToString($format,$culture),
                $peak.ToString($format,$culture),$available.ToString($format,$culture),$total.ToString($format,$culture),
                $peakWorking.ToString($format,$culture),$peakVirtual.ToString($format,$culture)) -join ','
            $Journal.SystemWriter.WriteLine($row)
            $Journal.SystemRows++
        }
        catch { Add-OptimizedMWMemoryJournalError $Journal 'write' $_.Exception.Message }
    }
}

function Get-OptimizedMWMemoryGuardDecision {
    param($Sample, [DateTimeOffset]$Now)
    # Fresh valid host observations only. This cancels a diagnostic run; it is
    # neither the optional engine admission policy nor a guaranteed RAM ceiling.
    if ($null -eq $Sample) { return 'unavailable' }
    $age=$Now.ToUnixTimeMilliseconds()-$Sample.EpochMs
    if ($age -lt 0 -or $age -gt 2500) { return 'unavailable' }
    if (($Sample.PerformanceValid -and $Sample.CommitSpareMiB -lt 256) -or
        ($Sample.PhysicalValid -and $Sample.PhysicalAvailableMiB -lt 128)) { return 'urgent' }
    if (($Sample.PerformanceValid -and $Sample.CommitSpareMiB -lt 1024) -or
        ($Sample.PhysicalValid -and $Sample.PhysicalAvailableMiB -lt 512)) { return 'cancel' }
    return 'continue'
}

function Stop-OptimizedMWOwnedAutomaticProcess {
    param([Parameter(Mandatory=$true)][Diagnostics.Process]$Process, [bool]$Urgent=$false)
    # Caller passes the exact Process returned by its own launch; no name/PID
    # enumeration or lookup, so an unrelated game cannot become the target.
    if($Process.HasExited){return}
    if(-not $Urgent){
        try{
            if($Process.CloseMainWindow()){$null=$Process.WaitForExit(1000)}
        }catch{if($Process.HasExited){return};throw}
    }
    if(-not $Process.HasExited){$Process.Kill();$Process.WaitForExit()}
}

function Stop-OptimizedMWMemoryJournal {
    [CmdletBinding()]
    param([Parameter(Mandatory=$true)]$Journal, [bool]$NormalFinish=$false)
    if ($Journal.Stopped) { return }
    $Journal.Stopped=$true
    foreach ($writer in @($Journal.ProcessWriter,$Journal.SystemWriter)) {
        if ($null -ne $writer) {
            try { $writer.Dispose() }
            catch { Add-OptimizedMWMemoryJournalError $Journal 'write' $_.Exception.Message }
        }
    }
    $status=[ordered]@{
        schema=1; diagnostic_only=$true; scope='Host process/system observations; not gameplay or complete allocation attribution'
        normal_finish=$NormalFinish; complete=($Journal.Errors -eq 0 -and $Journal.Dropped -eq 0)
        completion_scope='complete describes journal transport; normal_finish is the owning launcher game result'
        output_ok=($Journal.WriteErrors -eq 0); include_system=$Journal.IncludeSystem
        attempted_samples=$Journal.Attempts; accepted_attempts=$Journal.AcceptedAttempts; sample_cap=$Journal.MaxSamples
        process_rows=$Journal.ProcessRows; system_rows=$Journal.SystemRows; dropped_samples=$Journal.Dropped
        errors=$Journal.Errors; process_sample_errors=$Journal.ProcessSampleErrors; system_sample_errors=$Journal.SystemSampleErrors
        write_errors=$Journal.WriteErrors; first_epoch_ms=$Journal.FirstEpochMs; last_epoch_ms=$Journal.LastEpochMs
        error_messages=@($Journal.ErrorMessages.ToArray())
        limitations=@('One-second sampling can miss shorter peaks.',
            'System commit peak is since system boot, not this game.',
            'Process peak columns are process-lifetime peaks.',
            'Virtual memory columns describe address-space size, not RAM or commit.',
            'Zero system fields with invalid flags are unavailable, not zero usage.',
            'Raw CSV rows are AutoFlush and remain readable before Stop or after an interrupted launcher.')
    }
    $stream=$null; $writer=$null
    try {
        $stream=[IO.FileStream]::new($Journal.StatusPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
        $writer=[IO.StreamWriter]::new($stream,(New-Object System.Text.UTF8Encoding($false)))
        $writer.WriteLine(($status | ConvertTo-Json -Depth 4))
        $writer.Dispose(); $writer=$null; $stream=$null
    }
    catch {
        if ($null -ne $writer) { try { $writer.Dispose() } catch {} }
        elseif ($null -ne $stream) { try { $stream.Dispose() } catch {} }
        throw 'Memory journal CSV rows were preserved, but writing its completion status failed.'
    }
}
