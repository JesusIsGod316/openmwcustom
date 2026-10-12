param(
    [Parameter(Mandatory=$true)][string]$CaptureDirectory,
    [string]$ProfileZip,
    [switch]$FunctionsOnly
)
$ErrorActionPreference='Stop'

function ConvertTo-SystemTraceUtcValue {
    param([object]$Value)
    if($Value -is [DateTime]){return $Value.ToUniversalTime()}
    return [DateTime]::Parse([string]$Value).ToUniversalTime()
}

function Write-SystemTraceQualificationJson {
    param([string]$Path,[object]$Value)
    $temp=$Path+'.'+[Guid]::NewGuid().ToString('N')+'.tmp'
    try{
        $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $temp -Encoding UTF8
        if([IO.File]::Exists($Path)){
            [IO.File]::Replace($temp,$Path,[System.Management.Automation.Language.NullString]::Value)
        }else{
            try{[IO.File]::Move($temp,$Path)}catch [IO.IOException]{
                if([IO.File]::Exists($Path)){
                    [IO.File]::Replace($temp,$Path,[System.Management.Automation.Language.NullString]::Value)
                }else{throw}
            }
        }
    }finally{if([IO.File]::Exists($temp)){[IO.File]::Delete($temp)}}
}

function Get-SystemTraceQualification {
    param([object]$Native,[object]$State,[object]$Binding)
    $reasons=[Collections.Generic.List[string]]::new()
    if($State.ambiguous_game_processes -or $State.stop_reason -eq 'multiple_games'){[void]$reasons.Add('Multiple engine processes observed; recorder stopped early and capture pairing is ambiguous')}
    if($Native.LostEvents -ne 0 -or $Native.LostBuffers -ne 0){[void]$reasons.Add('ETL reports lost events or buffers')}
    if($Native.SchedulingEvents -eq 0 -or $Native.FirstSchedulingFileTime -eq 0){[void]$reasons.Add('No retained scheduling events')}
    $first=$null;$last=$null
    if($Native.FirstSchedulingFileTime -gt 0){$first=[DateTime]::FromFileTimeUtc($Native.FirstSchedulingFileTime)}
    if($Native.LastSchedulingFileTime -gt 0){$last=[DateTime]::FromFileTimeUtc($Native.LastSchedulingFileTime)}
    $coverage='UNQUALIFIED: engine binding required'
    if($null -ne $Binding){
        if(-not $Binding.identity_valid){[void]$reasons.Add('Engine/system capture identity mismatch')}
        if($first -and $last){
            # Clock-map anchors bracket the entire engine recorder lifetime.
            # No tolerance hides missing early/late events: expose actual gaps.
            $earlyGap=[Math]::Max(0,($first-$Binding.start_utc).TotalMilliseconds)
            $lateGap=[Math]::Max(0,($Binding.end_utc-$last).TotalMilliseconds)
            if($earlyGap -gt 0 -or $lateGap -gt 0){
                $coverage='PARTIAL';[void]$reasons.Add("Engine clock range not fully covered: early gap $earlyGap ms, late gap $lateGap ms")
            }else{$coverage='FULL_ENGINE_CLOCK_RANGE'}
        }
    }elseif($State.game_processes -and @($State.game_processes).Count -eq 1 -and $first -and $last){
        $gameStart=ConvertTo-SystemTraceUtcValue $State.game_processes[0].creation_utc
        $coverage=$(if($first -le $gameStart){'STARTUP_RETAINED; engine end anchor still required'}else{'EARLY_GAME_EVENTS_MISSING'})
        if($first -gt $gameStart){[void]$reasons.Add('Retained scheduling events begin after observed game creation')}
    }
    return [ordered]@{schema=1;capture_id=$State.capture_id;
        etl_header_start_utc=[DateTime]::FromFileTimeUtc($Native.HeaderStartFileTime).ToString('o');
        etl_header_stop_utc=[DateTime]::FromFileTimeUtc($Native.HeaderEndFileTime).ToString('o');
        retained_scheduling_first_utc=$(if($first){$first.ToString('o')}else{$null});
        retained_scheduling_last_utc=$(if($last){$last.ToString('o')}else{$null});
        retained_scheduling_seconds=$(if($first -and $last){($last-$first).TotalSeconds}else{0});
        stop_reason=$State.stop_reason;ambiguous_game_processes=[bool]$State.ambiguous_game_processes;
        ignored_helper_processes=$State.helper_processes;process_inventory=$State.process_inventory;
        processed_events=$Native.Events;lost_events=$Native.LostEvents;lost_buffers=$Native.LostBuffers;
        scheduling_events=$Native.SchedulingEvents;sample_events=$Native.SampleEvents;
        dxgkrnl_events=$Native.GpuEvents;dxgkrnl_task_opcode_event_version_counts=$Native.GpuKinds;
        gpu_event_availability=$(if($Native.GpuEvents -gt 0){'PROVIDER_EVENTS_PRESENT'}else{'UNAVAILABLE: zero DxgKrnl events'});
        gpu_queue_attribution='UNQUALIFIED: task/opcode counts alone do not prove decoded DMA packets or game queue ownership';
        coverage=$coverage;engine_binding=$Binding;qualification_reasons=@($reasons);
        scheduling_complete=($coverage -eq 'FULL_ENGINE_CLOCK_RANGE' -and $reasons.Count -eq 0);
        limitation='Coverage means retained timestamps bracket the engine clock range, not proof of no internal event holes. Exact anchor gaps are exposed without tolerance. Loss counts remain separately checked. Provider counts are not driver-causality proof.'}
}

function Read-SystemTraceZipText {
    param([IO.Compression.ZipArchive]$Archive,[string]$Name)
    $entries=@($Archive.Entries | Where-Object {$_.FullName.Replace('\','/').TrimStart('/') -eq $Name})
    if($entries.Count -ne 1){throw "Engine ZIP must contain exactly one $Name"}
    if($entries[0].Length -gt 1MB){throw 'Identity metadata unexpectedly large'}
    $reader=[IO.StreamReader]::new($entries[0].Open())
    try{return $reader.ReadToEnd()}finally{$reader.Dispose()}
}

function Resolve-SystemTraceFinalBinding {
    param([object]$State,[object]$FinalBinding,[string]$ProfileZip)
    if($FinalBinding.capture_id -ne $State.capture_id -or $FinalBinding.instance -ne $State.instance -or
        $FinalBinding.recorder_process_id -ne $State.recorder_process_id -or
        (ConvertTo-SystemTraceUtcValue $FinalBinding.recorder_creation_utc) -ne (ConvertTo-SystemTraceUtcValue $State.recorder_creation_utc)){
        throw 'Final engine binding belongs to another recorder lifetime'
    }
    if(-not $ProfileZip){$ProfileZip=$FinalBinding.engine_zip_path}
    if(-not [string]::Equals([IO.Path]::GetFullPath($ProfileZip),[IO.Path]::GetFullPath($FinalBinding.engine_zip_path),[StringComparison]::OrdinalIgnoreCase) -or
        (Get-Item -LiteralPath $ProfileZip).Length -ne $FinalBinding.engine_zip_bytes -or
        (Get-FileHash -LiteralPath $ProfileZip).Hash -ne $FinalBinding.engine_zip_sha256){
        throw 'Final ZIP identity/hash/length changed; refusing to pair an intermediate archive'
    }
    return [pscustomobject]@{binding=$FinalBinding;profile_zip=$ProfileZip}
}

if($FunctionsOnly){return}
if(-not [Environment]::Is64BitProcess){throw 'Native ETL qualification requires 64-bit PowerShell; ETL is retained unchanged'}
$statePath=Join-Path $CaptureDirectory 'SYSTEM-TRACE-STATE.json'
$state=Get-Content -Raw -LiteralPath $statePath | ConvertFrom-Json
if($state.schema -eq 2 -and (-not $state.normal_stop -or $state.status -ne 'saved')){
    [ordered]@{schema=1;capture_id=$state.capture_id;status='PENDING_OR_UNAVAILABLE';
        reason=('Recorder has not finished saving: '+$state.status);profile_zip=$ProfileZip} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $CaptureDirectory 'SYSTEM-TRACE-QUALIFICATION-PENDING.json') -Encoding UTF8
    Write-Host ('Qualification pending or unavailable: recorder status '+$state.status+'; no ETL completeness claim')
    return
}
$bindingPath=Join-Path $CaptureDirectory 'SYSTEM-TRACE-ENGINE-BINDING.json'
$finalBinding=$null
if(Test-Path -LiteralPath $bindingPath){
    $finalBinding=Get-Content -Raw -LiteralPath $bindingPath | ConvertFrom-Json
    $resolvedBinding=Resolve-SystemTraceFinalBinding $state $finalBinding $ProfileZip
    $ProfileZip=$resolvedBinding.profile_zip
}
$etl=Join-Path $CaptureDirectory 'OptimizedMW-SystemTrace.etl'
$etlHash=(Get-FileHash -LiteralPath $etl).Hash.ToLowerInvariant()
if($etlHash -ne $state.etl_sha256){throw 'ETL hash differs from recorder state; refusing qualification'}
if(-not ('OptimizedMW.NativeTraceQualification' -as [type])){
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
namespace OptimizedMW {
    public sealed class NativeTraceResult {
        public long Events, SchedulingEvents, SampleEvents, GpuEvents;
        public long FirstSchedulingFileTime, LastSchedulingFileTime;
        public long HeaderStartFileTime, HeaderEndFileTime;
        public uint LostEvents, LostBuffers;
        public Dictionary<string,long> GpuKinds = new Dictionary<string,long>();
    }
    public static class NativeTraceQualification {
        // Windows x64 EVENT_TRACE_LOGFILEW layout, including its nested
        // EVENT_TRACE and TRACE_LOGFILE_HEADER. No unmanaged pointers survive.
        [StructLayout(LayoutKind.Explicit, Size=448)]
        struct LogFile {
            [FieldOffset(0)] public IntPtr FileName;
            [FieldOffset(28)] public uint ProcessTraceMode;
            [FieldOffset(136)] public long HeaderEndTime;
            [FieldOffset(168)] public uint HeaderEventsLost;
            [FieldOffset(384)] public long HeaderStartTime;
            [FieldOffset(396)] public uint HeaderBuffersLost;
            [FieldOffset(424)] public IntPtr EventRecordCallback;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Descriptor {
            public ushort Id;
            public byte Version, Channel, Level, Opcode;
            public ushort Task;
            public ulong Keyword;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct Header {
            public ushort Size, HeaderType, Flags, EventProperty;
            public uint ThreadId, ProcessId;
            public long TimeStamp;
            public Guid ProviderId;
            public Descriptor Event;
            public uint KernelTime, UserTime;
            public Guid ActivityId;
        }
        [UnmanagedFunctionPointer(CallingConvention.Winapi)] delegate void EventCallback(IntPtr record);
        [DllImport("advapi32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
        static extern ulong OpenTraceW(ref LogFile logFile);
        [DllImport("advapi32.dll")] static extern uint ProcessTrace(ulong[] handles, uint count, IntPtr start, IntPtr end);
        [DllImport("advapi32.dll")] static extern uint CloseTrace(ulong handle);
        static readonly Guid ThreadProvider = new Guid("3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c");
        static readonly Guid PerfProvider = new Guid("ce1dbfb4-137e-4da6-87b0-3f59aa102cbc");
        static readonly Guid GpuProvider = new Guid("802ec45a-1e99-4b83-9920-87c98277ba9d");
        public static NativeTraceResult Read(string path) {
            if(IntPtr.Size != 8) throw new InvalidOperationException("Windows x64 layout required");
            NativeTraceResult result = new NativeTraceResult();
            Exception callbackError = null;
            EventCallback callback = delegate(IntPtr record) {
                try {
                    Header h = (Header)Marshal.PtrToStructure(record, typeof(Header));
                    ++result.Events;
                    bool scheduling = h.ProviderId == ThreadProvider && (h.Event.Opcode == 36 || h.Event.Opcode == 50);
                    bool sample = h.ProviderId == PerfProvider && h.Event.Opcode == 46;
                    if(scheduling || sample) {
                        if(result.FirstSchedulingFileTime == 0 || h.TimeStamp < result.FirstSchedulingFileTime) result.FirstSchedulingFileTime = h.TimeStamp;
                        if(h.TimeStamp > result.LastSchedulingFileTime) result.LastSchedulingFileTime = h.TimeStamp;
                    }
                    if(scheduling) ++result.SchedulingEvents;
                    if(sample) ++result.SampleEvents;
                    if(h.ProviderId == GpuProvider) {
                        ++result.GpuEvents;
                        string key = "task="+h.Event.Task+";opcode="+h.Event.Opcode+";event="+h.Event.Id+";version="+h.Event.Version;
                        long count; result.GpuKinds.TryGetValue(key, out count); result.GpuKinds[key] = count+1;
                    }
                } catch(Exception e) { callbackError = e; }
            };
            LogFile file = new LogFile();
            file.FileName = Marshal.StringToHGlobalUni(path);
            file.ProcessTraceMode = 0x10000000; // EVENT_RECORD, converted FILETIME timestamps; never RAW_TIMESTAMP.
            file.EventRecordCallback = Marshal.GetFunctionPointerForDelegate(callback);
            ulong handle = UInt64.MaxValue;
            try {
                handle = OpenTraceW(ref file);
                if(handle == UInt64.MaxValue) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "OpenTrace failed: "+path);
                result.HeaderStartFileTime = file.HeaderStartTime;
                result.HeaderEndFileTime = file.HeaderEndTime;
                result.LostEvents = file.HeaderEventsLost;
                result.LostBuffers = file.HeaderBuffersLost;
                uint status = ProcessTrace(new ulong[]{handle}, 1, IntPtr.Zero, IntPtr.Zero);
                if(status != 0) throw new System.ComponentModel.Win32Exception((int)status, "ProcessTrace failed");
                if(callbackError != null) throw new InvalidOperationException("Native event callback failed", callbackError);
            } finally {
                if(handle != UInt64.MaxValue) CloseTrace(handle);
                Marshal.FreeHGlobal(file.FileName);
                GC.KeepAlive(callback);
            }
            return result;
        }
    }
}
'@
}
$native=[OptimizedMW.NativeTraceQualification]::Read([IO.Path]::GetFullPath($etl))
$binding=$null
if($ProfileZip){
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($ProfileZip))
    try{
        $link=Read-SystemTraceZipText $archive 'SYSTEM-TRACE-LINK.json' | ConvertFrom-Json
        $clock=@{};foreach($line in (Read-SystemTraceZipText $archive 'p9-hitch-spans.csv.clock-map.txt') -split '\r?\n'){
            if($line -match '^([^=]+)=(.*)$'){$clock[$matches[1]]=$matches[2]}
        }
        $mode=@{};foreach($line in (Read-SystemTraceZipText $archive 'TEST_MODE.txt') -split '\r?\n'){
            if($line -match '^([^=]+)=(.*)$'){$mode[$matches[1]]=$matches[2]}
        }
        $matching=@($state.game_processes | Where-Object {$_.process_id -eq [int]$clock.process_id})
        $epoch=[DateTime]::new(1970,1,1,0,0,0,[DateTimeKind]::Utc)
        # Decimal division preserves large Unix-nanosecond anchors; floating
        # point division would silently round hundreds of nanoseconds here.
        $startUtc=$epoch.AddTicks([long][decimal]::Truncate([decimal][long]::Parse($clock.start_utc_ns)/100))
        $endUtc=$epoch.AddTicks([long][decimal]::Truncate([decimal][long]::Parse($clock.end_utc_ns)/100))
        $identityValid=($state.schema -eq 2 -and $link.capture_id -eq $state.capture_id -and
            $link.instance -eq $state.instance -and $link.recorder_process_id -eq $state.recorder_process_id -and
            (ConvertTo-SystemTraceUtcValue $link.recorder_creation_utc) -eq (ConvertTo-SystemTraceUtcValue $state.recorder_creation_utc) -and $matching.Count -eq 1 -and
            $link.executable_sha256 -eq $state.executable_sha256 -and
            [string]::Equals($link.executable_path,$state.executable_path,[StringComparison]::OrdinalIgnoreCase) -and
            $mode.openmw_exe_sha256 -eq $state.executable_sha256 -and
            (ConvertTo-SystemTraceUtcValue $matching[0].creation_utc) -le $startUtc -and
            (ConvertTo-SystemTraceUtcValue $state.recording_started_utc) -le $startUtc -and
            $endUtc -le (ConvertTo-SystemTraceUtcValue $state.end_utc) -and
            $clock.started -eq '1' -and $clock.normal_finish -eq '1' -and
            [long]::Parse($clock.qpc_frequency) -gt 0 -and $state.executable_unchanged -and $startUtc -lt $endUtc)
        if($finalBinding){$identityValid=$identityValid -and $finalBinding.game_process_id -eq [int]$clock.process_id -and
            (ConvertTo-SystemTraceUtcValue $finalBinding.game_creation_utc) -eq (ConvertTo-SystemTraceUtcValue $matching[0].creation_utc)}
        $binding=[ordered]@{identity_valid=$identityValid;engine_zip_path=[IO.Path]::GetFullPath($ProfileZip);
            engine_zip_sha256=(Get-FileHash -LiteralPath $ProfileZip).Hash.ToLowerInvariant();
            process_id=[int]$clock.process_id;start_utc=$startUtc;end_utc=$endUtc;
            start_qpc_before=$clock.start_qpc_before;start_qpc_after=$clock.start_qpc_after;
            end_qpc_before=$clock.end_qpc_before;end_qpc_after=$clock.end_qpc_after;qpc_frequency=$clock.qpc_frequency}
    }finally{$archive.Dispose()}
}
$report=Get-SystemTraceQualification $native $state $binding
$report.etl_sha256=$etlHash
$reportName=$(if($binding){'SYSTEM-TRACE-ENGINE-QUALIFICATION.json'}else{'SYSTEM-TRACE-QUALIFICATION.json'})
$reportPath=Join-Path $CaptureDirectory $reportName
Write-SystemTraceQualificationJson $reportPath $report
if($binding){
    # The bound report can finish after the system ZIP was made. Publish a
    # companion beside the immutable engine ZIP without re-zipping a large ETL.
    Write-SystemTraceQualificationJson ($binding.engine_zip_path+'.system-qualification.json') $report
}
Write-Host ('Retained scheduling range: '+$report.retained_scheduling_seconds+' seconds; '+$report.coverage)
Write-Host ('DxgKrnl: '+$report.gpu_event_availability+'; GPU queue attribution remains unqualified')
