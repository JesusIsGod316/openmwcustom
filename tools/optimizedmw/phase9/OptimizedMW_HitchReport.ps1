# Post-run only. Streaming validation, bounded identities and no Python dependency.
if(-not ('Phase9.HitchReport' -as [type])){
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
namespace Phase9 {
 public class HitchSpan {
  public ulong span,parent,thread,link;
  public long frame,start_ns,end_ns,epoch_ms;
  public string phase,identity,asset;
  public double wall_ms,children_ms,unattributed_ms,thread_cpu_ms;
  public int identity_truncated;
  public Dictionary<string,ulong> metrics=new Dictionary<string,ulong>();
 }
 public class HitchResult {
  public int rows,roots,truncated_identities;
  public List<string> reasons=new List<string>();
  public List<HitchSpan> largest_scopes,largest_unattributed_scopes;
  public List<HitchPhase> phase_totals;
  public int dependency_links;
  public List<HitchSpan> largest_dependency_scopes;
  public List<HitchPhase> diagnostic_phase_totals;
  public HitchLogicalIo logical_io=new HitchLogicalIo();
  public CompileAttributionResult compile_attribution;
  public int open_stream_totals_unavailable;
 }
 public class CompileAttributionResult {
  public int rows,operations,summaries,fallbacks,unknown_frames,unknown_tracking,unknown_images;
  public int unjoined_compile_spans;
  public ulong metadata_allocation_failures_max,format_allocation_failures_max;
  public ulong registry_capacity_failures_max,registry_allocation_failures_max;
  public bool transport_footer,loss_footer;
  public ulong writer_dropped;
 }
 public class HitchLogicalIo {
  public bool metadata_available,complete_stream_totals;
  public bool candidate_metadata_available,complete_candidate_stream_totals;
  public int opened_streams,failed_opens,closed_streams,hash_operations,hash_incomplete_rewinds;
  public ulong read_calls,returned_bytes,read_call_ns,seek_calls,hash_logical_bytes,hash_blocks;
  public int policy_streams,buffer_streams,io_streams,consumption_streams;
  public int refill_requested_streams,refill_effective_streams,refill_fallback_streams;
  public int resident_seek_requested_streams,resident_seek_effective_streams,resident_seek_fallback_streams;
  public ulong owned_candidate_bytes_at_close_sum,buffer_cap_hits,buffer_allocation_failures;
  public ulong native_tell_calls,native_seek_calls,zero_byte_read_calls,resident_seek_hits;
  public ulong net_consumed_bytes,net_putback_bytes,discarded_unread_bytes,unread_at_close_bytes;
  public int budget_snapshot_streams;
  public ulong observed_budget_in_use_bytes_max,observed_budget_peak_bytes_max;
  public ulong observed_active_candidate_streams_max,observed_active_candidate_buffers_max;
  public List<ulong> budget_limits_bytes=new List<ulong>();
 }
 public class HitchPhase {
  public string phase;
  public int retained_calls,cpu_samples;
  public double inclusive_ms,self_ms,max_ms,observed_thread_cpu_ms;
 }
 public static class HitchReport {
  static CultureInfo CI=CultureInfo.InvariantCulture;
  static ulong Metric(HitchSpan span,string name) {
   ulong value;
   if(!span.metrics.TryGetValue(name,out value))throw new InvalidDataException("Missing metric "+name+" for "+span.phase);
   return value;
  }
  static void Add(ref ulong total,ulong value) {
   try { total=checked(total+value); }
   catch(OverflowException) { throw new InvalidDataException("Logical I/O aggregate overflow"); }
  }
  public static string[] Csv(string line) {
   var fields=new List<string>(); var text=new StringBuilder(); bool quoted=false;
   for(int i=0;i<line.Length;i++) {
    char c=line[i];
    if(c=='"') { if(quoted && i+1<line.Length && line[i+1]=='"'){text.Append('"');i++;}
     else quoted=!quoted; }
    else if(c==',' && !quoted){fields.Add(text.ToString());text.Clear();}
    else text.Append(c);
   }
   if(quoted) throw new InvalidDataException("Unclosed CSV quote");
   fields.Add(text.ToString()); return fields.ToArray();
  }
  public static HitchResult Read(string path) { return Read(path,null); }
  public static HitchResult Read(string path,string attributionPath) {
   var result=new HitchResult(); var spans=new Dictionary<ulong,HitchSpan>();
   if(new FileInfo(path).Length>1073741824) throw new InvalidDataException("Timeline exceeds 1 GiB bound");
   using(var reader=new StreamReader(path)) {
    string header=reader.ReadLine();
    const string original="span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated";
    const string extended=",link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value";
    bool metadata=header==original+extended;
    result.logical_io.metadata_available=metadata;
    if(header!=original && !metadata)
     throw new InvalidDataException("Unsupported timeline schema");
    string line;
    while((line=reader.ReadLine())!=null) {
     if(line.Length==0 || line.StartsWith("#"))continue;
     if(line.Length>4096 || spans.Count>=524288)throw new InvalidDataException("Timeline row bound exceeded");
     var f=Csv(line); if(f.Length!=(metadata?24:15))throw new InvalidDataException("Malformed timeline row");
     var s=new HitchSpan{span=ulong.Parse(f[0],CI),parent=ulong.Parse(f[1],CI),frame=long.Parse(f[2],CI),
      thread=ulong.Parse(f[3],CI),start_ns=long.Parse(f[4],CI),end_ns=long.Parse(f[5],CI),epoch_ms=long.Parse(f[6],CI),
      phase=f[7],identity=f[8],asset=f[9],wall_ms=double.Parse(f[10],CI),children_ms=double.Parse(f[11],CI),
      unattributed_ms=double.Parse(f[12],CI),thread_cpu_ms=double.Parse(f[13],CI),identity_truncated=int.Parse(f[14],CI)};
     if(metadata) {
      s.link=ulong.Parse(f[15],CI);
      for(int i=0;i<4;i++) {
       string name=f[16+i*2]; ulong value=ulong.Parse(f[17+i*2],CI);
       if(name.Length==0) { if(value!=0)throw new InvalidDataException("Unnamed timeline metric"); continue; }
       if(name.Length>31)throw new InvalidDataException("Timeline metric name exceeds bound");
       s.metrics.Add(name,value);
      }
     }
     if(s.span==0 || s.parent>=s.span || s.start_ns>s.end_ns || s.wall_ms<0 || s.children_ms<0 || s.unattributed_ms<0 ||
      double.IsNaN(s.wall_ms) || double.IsInfinity(s.wall_ms) || double.IsNaN(s.children_ms) || double.IsInfinity(s.children_ms) ||
      double.IsNaN(s.unattributed_ms) || double.IsInfinity(s.unattributed_ms) ||
      Math.Abs(s.wall_ms-(s.end_ns-s.start_ns)/1e6)>0.00001 ||
      Math.Abs(s.wall_ms-s.children_ms-s.unattributed_ms)>0.00001 ||
      double.IsNaN(s.thread_cpu_ms) || double.IsInfinity(s.thread_cpu_ms) ||
      (s.thread_cpu_ms<0 && s.thread_cpu_ms!=-1) ||
      s.identity_truncated<0 || s.identity_truncated>1)throw new InvalidDataException("Incoherent span timing or identity");
     spans.Add(s.span,s);
    }
   }
   foreach(var s in spans.Values) {
    if(s.parent==0)result.roots++;
    else {
     HitchSpan p;
     if(!spans.TryGetValue(s.parent,out p) || p.thread!=s.thread || p.start_ns>s.start_ns || p.end_ns<s.end_ns)
      throw new InvalidDataException("Missing or incoherent same-thread parent");
    }
    result.truncated_identities+=s.identity_truncated;
    if(s.link!=0) {
     if(s.link>=s.span || !spans.ContainsKey(s.link))throw new InvalidDataException("Missing or incoherent causal dependency");
     result.dependency_links++;
    }
   }
   result.rows=spans.Count;
   result.largest_scopes=spans.Values.OrderByDescending(s=>s.wall_ms).Take(25).ToList();
   result.largest_unattributed_scopes=spans.Values.OrderByDescending(s=>s.unattributed_ms).Take(25).ToList();
   result.phase_totals=spans.Values.GroupBy(s=>s.phase).Select(g=>new HitchPhase {
    phase=g.Key,retained_calls=g.Count(),inclusive_ms=g.Sum(s=>s.wall_ms),self_ms=g.Sum(s=>s.unattributed_ms),
    max_ms=g.Max(s=>s.wall_ms),cpu_samples=g.Count(s=>s.thread_cpu_ms>=0),
    observed_thread_cpu_ms=g.Where(s=>s.thread_cpu_ms>=0).Sum(s=>s.thread_cpu_ms)
   }).OrderByDescending(p=>p.self_ms).ToList();
   result.diagnostic_phase_totals=result.phase_totals.Where(p=>p.phase.StartsWith("diagnostic.",StringComparison.Ordinal)).ToList();
   result.largest_dependency_scopes=spans.Values.Where(s=>s.link!=0).OrderByDescending(s=>s.wall_ms).Take(50).ToList();
   if(result.logical_io.metadata_available) {
    var successfulOpens=new HashSet<ulong>(); var totalsSources=new HashSet<ulong>();
    var extraSources=new Dictionary<string,HashSet<ulong>>();
    foreach(var phase in new[]{"file.stream_policy","file.stream_buffer","file.stream_io","file.stream_consumption"})
     extraSources.Add(phase,new HashSet<ulong>());
    var expectedBudgetSources=new HashSet<ulong>();var budgetSources=new HashSet<ulong>();
    var streamBudgets=new Dictionary<ulong,ulong>();var budgetSnapshots=new Dictionary<ulong,HitchSpan>();
    foreach(var s in spans.Values) {
     if(s.phase=="file.open_size") {
      ulong opened=Metric(s,"opened");
      if(opened>1)throw new InvalidDataException("Invalid file-open outcome");
      if(opened==1) { successfulOpens.Add(s.span); result.logical_io.opened_streams++; }
      else result.logical_io.failed_opens++;
     }
     else if(s.phase=="file.stream_totals") {
      HitchSpan source;
      if(s.link==0 || !spans.TryGetValue(s.link,out source) || source.phase!="file.open_size" ||
       Metric(source,"opened")!=1 || source.asset!=s.asset || !totalsSources.Add(s.link))
       throw new InvalidDataException("Missing or incoherent stream-totals source");
      result.logical_io.closed_streams++;
      Add(ref result.logical_io.read_calls,Metric(s,"read_calls"));
      Add(ref result.logical_io.returned_bytes,Metric(s,"returned_bytes"));
      Add(ref result.logical_io.read_call_ns,Metric(s,"read_call_ns"));
      Add(ref result.logical_io.seek_calls,Metric(s,"seek_calls"));
     }
     else if(extraSources.ContainsKey(s.phase)) {
      HitchSpan source;
      if(s.link==0 || !spans.TryGetValue(s.link,out source) || source.phase!="file.open_size" ||
       Metric(source,"opened")!=1 || source.asset!=s.asset || !extraSources[s.phase].Add(s.link))
       throw new InvalidDataException("Missing or incoherent extended stream source");
      result.logical_io.candidate_metadata_available=true;
      if(s.phase=="file.stream_policy") {
       var requested=Metric(s,"requested_refill_bytes");var effective=Metric(s,"effective_refill_bytes");
       var seekRequested=Metric(s,"requested_resident_seek");var seekEffective=Metric(s,"effective_resident_seek");
       if((requested!=8192 && requested!=65536) || (effective!=8192 && effective!=65536) ||
        effective>requested || seekRequested>1 || seekEffective>seekRequested)
        throw new InvalidDataException("Invalid effective constrained stream policy");
       result.logical_io.policy_streams++;
       if(requested==65536)result.logical_io.refill_requested_streams++;
       if(effective==65536)result.logical_io.refill_effective_streams++;
       if(requested!=effective)result.logical_io.refill_fallback_streams++;
       if(requested==65536 || seekRequested==1)expectedBudgetSources.Add(s.link);
       if(seekRequested==1)result.logical_io.resident_seek_requested_streams++;
       if(seekEffective==1)result.logical_io.resident_seek_effective_streams++;
       if(seekRequested!=seekEffective)result.logical_io.resident_seek_fallback_streams++;
      } else if(s.phase=="file.stream_buffer") {
       var owned=Metric(s,"owned_candidate_bytes");var limit=Metric(s,"budget_limit_bytes");
       if(limit>33554432 || owned>limit)throw new InvalidDataException("Invalid constrained stream buffer budget");
       result.logical_io.buffer_streams++;
       Add(ref result.logical_io.owned_candidate_bytes_at_close_sum,owned);
       Add(ref result.logical_io.buffer_cap_hits,Metric(s,"buffer_cap_hits"));
       Add(ref result.logical_io.buffer_allocation_failures,Metric(s,"buffer_allocation_failures"));
       if(!result.logical_io.budget_limits_bytes.Contains(limit))result.logical_io.budget_limits_bytes.Add(limit);
       streamBudgets.Add(s.link,limit);
      } else if(s.phase=="file.stream_io") {
       result.logical_io.io_streams++;
       Add(ref result.logical_io.native_tell_calls,Metric(s,"native_tell_calls"));
       Add(ref result.logical_io.native_seek_calls,Metric(s,"native_seek_calls"));
       Add(ref result.logical_io.zero_byte_read_calls,Metric(s,"zero_byte_read_calls"));
       Add(ref result.logical_io.resident_seek_hits,Metric(s,"resident_seek_hits"));
      } else {
       result.logical_io.consumption_streams++;
       Add(ref result.logical_io.net_consumed_bytes,Metric(s,"net_consumed_bytes"));
       Add(ref result.logical_io.net_putback_bytes,Metric(s,"net_putback_bytes"));
       Add(ref result.logical_io.discarded_unread_bytes,Metric(s,"discarded_unread_bytes"));
       Add(ref result.logical_io.unread_at_close_bytes,Metric(s,"unread_at_close_bytes"));
      }
     }
     else if(s.phase=="file.stream_budget") {
      HitchSpan source;
      if(s.link==0 || !spans.TryGetValue(s.link,out source) || source.phase!="file.open_size" ||
       Metric(source,"opened")!=1 || source.asset!=s.asset || !budgetSources.Add(s.link))
       throw new InvalidDataException("Missing or incoherent stream-budget source");
      budgetSnapshots.Add(s.link,s);result.logical_io.budget_snapshot_streams++;
      result.logical_io.observed_budget_in_use_bytes_max=Math.Max(result.logical_io.observed_budget_in_use_bytes_max,Metric(s,"budget_in_use_bytes"));
      result.logical_io.observed_budget_peak_bytes_max=Math.Max(result.logical_io.observed_budget_peak_bytes_max,Metric(s,"budget_peak_bytes"));
      result.logical_io.observed_active_candidate_streams_max=Math.Max(result.logical_io.observed_active_candidate_streams_max,Metric(s,"active_candidate_streams"));
      result.logical_io.observed_active_candidate_buffers_max=Math.Max(result.logical_io.observed_active_candidate_buffers_max,Metric(s,"active_candidate_buffers"));
     }
     else if(s.phase=="asset.hash") {
      result.logical_io.hash_operations++;
      Add(ref result.logical_io.hash_logical_bytes,Metric(s,"logical_bytes"));
      Add(ref result.logical_io.hash_blocks,Metric(s,"hash_blocks"));
      ulong rewound=Metric(s,"rewound");
      if(rewound>1)throw new InvalidDataException("Invalid hash-rewind outcome");
      if(rewound==0)result.logical_io.hash_incomplete_rewinds++;
     }
    }
    result.open_stream_totals_unavailable=successfulOpens.Count(id=>!totalsSources.Contains(id));
    result.logical_io.complete_stream_totals=result.open_stream_totals_unavailable==0;
    result.logical_io.complete_candidate_stream_totals=result.logical_io.candidate_metadata_available &&
     result.logical_io.complete_stream_totals && extraSources.Values.All(ids=>successfulOpens.SetEquals(ids)) &&
     expectedBudgetSources.SetEquals(budgetSources);
    foreach(var item in budgetSnapshots) {
     ulong limit;
     if(!expectedBudgetSources.Contains(item.Key) || !streamBudgets.TryGetValue(item.Key,out limit) ||
      Metric(item.Value,"budget_in_use_bytes")>limit || Metric(item.Value,"budget_peak_bytes")>limit)
      throw new InvalidDataException("Invalid or unrequested stream-budget snapshot");
    }
   }
   if(attributionPath!=null)result.compile_attribution=ReadCompileAttribution(attributionPath,spans);
   return result;
  }
  static CompileAttributionResult ReadCompileAttribution(string path,Dictionary<ulong,HitchSpan> spans) {
   const string header="schema,frame,epoch_ms,event,context,frame_known,scheduler,set,set_generation,op,span,branch,"+
    "selected_set_first_seen_frame,selected_set_age_frames,op_first_observed_frame,op_age_observed_frames,"+
    "demand_first_observed_frame,last_progress_observed_frame,tracking_status,urgency,quarantine,forcing_reason,"+
    "age_threshold_frames,hard_age_threshold_frames,budget_ms,predicted_ms,actual_ms,target_frame_ms,"+
    "max_budget_ms,base_target_fps,elapsed_before_ms,last_handoff_ms,headroom_ms,resource_bytes,"+
    "prepared_reused,required,cancelled,kind,compile_class,texture,image0,image0_revision,image0_filename,"+
    "image0_known,texture_object_before,texture_object_after,queue_depth,global_oldest_age_frames,"+
    "active_candidates,quarantined_candidates,metadata_allocation_failures,format_allocation_failures,"+
    "registry_capacity_failures,registry_allocation_failures";
   if(new FileInfo(path).Length>1073741824)throw new InvalidDataException("Compile attribution exceeds 1 GiB bound");
   var result=new CompileAttributionResult();var identities=new HashSet<ulong>();
   using(var reader=new StreamReader(path)) {
    if(reader.ReadLine()!=header)throw new InvalidDataException("Unsupported compile attribution schema/header");
    string line;
    while((line=reader.ReadLine())!=null) {
     if(line.Length==0)continue;
     if(line.StartsWith("#")) {
      if(line=="# p9_capture_transport=bounded_mpsc_v1")result.transport_footer=true;
      const string loss="# v3_async_diagnostics_dropped_lines=";
      if(line.StartsWith(loss,StringComparison.Ordinal)) {
       if(result.loss_footer)throw new InvalidDataException("Duplicate compile writer loss footer");
       result.writer_dropped=ulong.Parse(line.Substring(loss.Length),CI);result.loss_footer=true;
      }
      continue;
     }
     if(result.rows>=524288 || line.Length>65536)throw new InvalidDataException("Compile attribution row bound exceeded");
     // Image filenames may contain quoted newlines. Keep one bounded logical CSV row.
     var record=new StringBuilder(line);bool quoted=false;
     while(true) {
      quoted=false;
      for(int i=0;i<record.Length;i++)if(record[i]=='"') {
       if(quoted && i+1<record.Length && record[i+1]=='"')i++;else quoted=!quoted;
      }
      if(!quoted)break;
      var next=reader.ReadLine();if(next==null)throw new InvalidDataException("Unclosed compile attribution CSV row");
      record.Append('\n').Append(next);if(record.Length>65536)throw new InvalidDataException("Compile attribution row bound exceeded");
     }
     var f=Csv(record.ToString());if(f.Length!=54)throw new InvalidDataException("Malformed compile attribution row");
     var integers=new Dictionary<int,ulong>();
     foreach(var i in new[]{0,1,2,4,5,6,7,8,9,10,13,22,23,33,34,35,36,39,40,41,43,44,45,46,47,48,49,50,51,52,53})
      integers.Add(i,ulong.Parse(f[i],CI));
     if(integers[0]!=1 || integers[1]>uint.MaxValue || integers[4]>uint.MaxValue || integers[6]==0)
      throw new InvalidDataException("Invalid compile attribution schema/frame/context/scheduler");
     foreach(var i in new[]{5,34,35,36,43})if(integers[i]>1)throw new InvalidDataException("Invalid compile attribution flag");
     foreach(var i in new[]{12,14,15,16,17})if(f[i].Length>0 && ulong.Parse(f[i],CI)>uint.MaxValue)
      throw new InvalidDataException("Invalid compile observation frame/age");
     foreach(var i in new[]{24,25,26,27,28,29,30,31,32}) {
      var value=double.Parse(f[i],CI);if(double.IsNaN(value) || double.IsInfinity(value) || value<0)
       throw new InvalidDataException("Invalid compile attribution timing");
     }
     HitchSpan source;var id=integers[10];
     if(id==0 || !spans.TryGetValue(id,out source) || source.frame!=(long)integers[1] || !identities.Add(id))
      throw new InvalidDataException("Missing/duplicate compile span or mismatched source frame");
     if(f[3]=="op") {
      if(source.phase!="compile.operation" || integers[7]==0 || integers[9]==0 || f[11]!="p4")
       throw new InvalidDataException("Invalid compile operation source/owner");
      result.operations++;
     } else if(f[3]=="summary" || f[3]=="fallback") {
      if(source.phase!="compile.scheduler" || integers[7]!=0 || integers[9]!=0 ||
       (f[3]=="summary" && f[11]!="p4") ||
       (f[3]=="fallback" && f[11]!="osg_fallback" && f[11]!="osg_compile_all"))
       throw new InvalidDataException("Invalid compile scheduler source/branch");
      if(f[3]=="summary")result.summaries++;else result.fallbacks++;
     } else throw new InvalidDataException("Unknown compile attribution event");
     if(f[18]!="observed" && f[18]!="unobserved" && f[18]!="unknown_capacity" && f[18]!="unknown_allocation")
      throw new InvalidDataException("Unknown compile observation status");
     if(f[18]=="unknown_capacity" || f[18]=="unknown_allocation") {
      if(integers[8]!=0 || f[14].Length>0 || f[15].Length>0 || f[16].Length>0 || f[17].Length>0)
       throw new InvalidDataException("Unknown compile tracking acquired invented provenance");
      result.unknown_tracking++;
     }
     if(f[14].Length>0 && integers[5]==1 && integers[1]>=ulong.Parse(f[14],CI) &&
      (f[15].Length==0 || ulong.Parse(f[15],CI)!=integers[1]-ulong.Parse(f[14],CI)))
      throw new InvalidDataException("Incoherent observed operation age");
     if(integers[43]==0) {
      if(integers[40]!=0 || integers[41]!=0 || f[42].Length>0)
       throw new InvalidDataException("Unknown image acquired invented provenance");
      result.unknown_images++;
     } else if(integers[39]==0 || integers[40]==0)throw new InvalidDataException("Known image has no texture/image owner");
     if(integers[5]==0)result.unknown_frames++;
     result.metadata_allocation_failures_max=Math.Max(result.metadata_allocation_failures_max,integers[50]);
     result.format_allocation_failures_max=Math.Max(result.format_allocation_failures_max,integers[51]);
     result.registry_capacity_failures_max=Math.Max(result.registry_capacity_failures_max,integers[52]);
     result.registry_allocation_failures_max=Math.Max(result.registry_allocation_failures_max,integers[53]);
     result.rows++;
    }
   }
   if(!result.transport_footer || !result.loss_footer || result.writer_dropped!=0)
    throw new InvalidDataException("Missing/incomplete compile attribution transport footer");
   result.unjoined_compile_spans=spans.Values.Count(s=>(s.phase=="compile.operation" || s.phase=="compile.scheduler") && !identities.Contains(s.span));
   return result;
  }
 }
}
'@
}

function Test-Phase9CompileAttributionCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $mode=Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt')
    $requested=$mode -match '(?m)^hitch_compile_attribution=1\r?$'
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null
    if($requested){
        try{
            $hitch=Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'HITCH-CAPTURE.json') | ConvertFrom-Json
            if($hitch.valid_span_capture -ne $true){throw 'Exact span joins require an independently valid CPU timeline'}
            $path=Join-Path $ProfileDir 'compile-attribution.csv'
            $status=Read-Phase9WriterStatus ($path+'.writer-status.txt')
            if(-not $status.OutputOk -or $status.Loss -ne 0){throw 'Compile attribution writer loss or incomplete output'}
            $analysis=([Phase9.HitchReport]::Read((Join-Path $ProfileDir 'p9-hitch-spans.csv'),$path)).compile_attribution
            if($analysis.rows -eq 0){throw 'Requested compile attribution has no joined records'}
            if($analysis.unjoined_compile_spans -ne 0 -or $analysis.unknown_tracking -ne 0 -or $analysis.unknown_frames -ne 0 -or
                $analysis.metadata_allocation_failures_max -ne 0 -or $analysis.format_allocation_failures_max -ne 0 -or
                $analysis.registry_capacity_failures_max -ne 0 -or $analysis.registry_allocation_failures_max -ne 0){
                throw 'Compile family has missing span joins, unknown frame ownership, partial tracking, or metadata/format allocation failure'
            }
        }catch{[void]$reasons.Add([string]$_)}
    }
    $result=[pscustomobject]@{schema=1;requested=$requested;
        valid_compile_attribution_capture=$(if($requested){$reasons.Count -eq 0}else{$null});analysis=$analysis;
        reasons=$reasons.ToArray();
        scope='Actual scheduler/operation observations linked to retained CPU spans; image0 alone is not complete GL binding or every texture image';
        provenance_note='First-observed operation/demand frames are scheduler observations, not unobserved producer timestamps. Tracking/frame/image unknowns remain explicit. Global queue oldest age is distinct from selected-set/current-operation age.'}
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ProfileDir 'COMPILE-ATTRIBUTION-CAPTURE.json') -Encoding UTF8
    if($requested -and $reasons.Count){Write-Warning ('INVALID COMPILE ATTRIBUTION CAPTURE: '+($reasons -join '; '))}
    return $result
}

function Test-Phase9HitchCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $mode=Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt')
    $requested=$mode -match '(?m)^phase9_hitch_spans=1\r?$'
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null;$status=@{};$clock=@{}
    if($requested){
        try{
            $path=Join-Path $ProfileDir 'p9-hitch-spans.csv'
            foreach($line in (Get-Content -LiteralPath ($path+'.capture-status.txt'))){
                if($line -match '^([a-z_]+)=(\d+)$'){$status[$matches[1]]=[long]$matches[2]}
            }
            foreach($key in @('rows','capacity','dropped','allocation_failed','active_at_finish','normal_finish','output_ok')){
                if(-not $status.ContainsKey($key)){throw "Missing timeline status: $key"}
            }
            if($status.capacity -notin @(131072,262144,524288) -or $status.rows -gt $status.capacity -or $status.dropped -ne 0 -or
                $status.allocation_failed -ne 0 -or $status.active_at_finish -ne 0 -or
                $status.normal_finish -ne 1 -or $status.output_ok -ne 1){throw 'Timeline loss or incomplete output'}
            $analysis=[Phase9.HitchReport]::Read($path)
            if($mode -match '(?m)^phase9_detail_revision=[23]\r?$'){
                foreach($line in [IO.File]::ReadAllLines($path+'.clock-map.txt')){
                    if($line -match '^([^=]+)=(.*)$'){$clock[$matches[1]]=[string]$matches[2]}
                }
                foreach($key in @('process_id','thread_id_kind','qpc_frequency','start_qpc_before','start_qpc_after','start_steady_ns','end_qpc_before','end_qpc_after','end_steady_ns','normal_finish')){
                    if(-not $clock.ContainsKey($key)){throw "Missing ETW clock mapping: $key"}
                }
                if($clock.thread_id_kind -ne 'windows_native_tid' -or [long]$clock.qpc_frequency -le 0 -or
                    [long]$clock.start_qpc_before -gt [long]$clock.start_qpc_after -or
                    [long]$clock.end_qpc_before -gt [long]$clock.end_qpc_after -or
                    [long]$clock.start_steady_ns -gt [long]$clock.end_steady_ns -or $clock.normal_finish -ne '1'){
                    throw 'Invalid ETW clock calibration or incomplete shutdown'
                }
            }
            if($analysis.rows -ne $status.rows -or $analysis.rows -eq 0){throw 'Empty or inconsistent timeline row count'}
            $stream=[IO.File]::OpenRead($path);$reader=$null
            try{
                [void]$stream.Seek([Math]::Max(0,$stream.Length-4096),[IO.SeekOrigin]::Begin)
                $reader=[IO.StreamReader]::new($stream);$tail=$reader.ReadToEnd()
            }finally{if($reader){$reader.Dispose()}else{$stream.Dispose()}}
            foreach($key in @('rows','capacity','dropped','allocation_failed','active_at_finish','normal_finish')){
                if($tail -notmatch ('(?m)^# '+$key+'='+$status[$key]+'\r?$')){throw "Missing or inconsistent timeline footer: $key"}
            }
            if($mode -notmatch '(?m)^final_exit_code=0\r?$'){throw 'Game did not exit normally'}
        }catch{[void]$reasons.Add([string]$_)}
    }
    $result=[pscustomobject]@{schema=2;requested=$requested;valid_span_capture=$(if($requested){$reasons.Count -eq 0}else{$null});
        status=$status;analysis=$analysis;clock_mapping=$clock;reasons=$reasons.ToArray();
        scope='Nested CPU wall-time spans; selected GL calls are in the separate draw trace. GPU/driver internals remain partially unobserved.';
        identity_note='Truncated identities are explicitly marked; do not claim exact asset identity for those rows.';
        accounting_note='children_ms includes fast filtered child scopes. unattributed_ms is the remaining envelope, not proof of a specific cause. Nested scopes must not be added together.';
        dependency_note='link is a causal source span and may cross threads; it is distinct from the same-thread nesting parent. Named metrics have phase-specific units; logical reads do not equal physical disk operations. Lua allocation metrics include nested calls and describe logical allocator requests, not OS residency.';
        diagnostic_note='diagnostic.* totals identify retained recorder envelopes. Catalog all-call counters separately include work below the span threshold. Do not subtract averages or infer normal gameplay speed from diagnostic runs.';
        logical_io_note='file.stream_totals includes every logical platform read in each closed constrained stream, including reads below the per-span threshold. open_stream_totals_unavailable counts successful opens without final totals; aggregates then cover closed streams only. Original 15-column captures have no named metrics, so metadata_available=false means these totals are unavailable. Hash logical bytes can overlap stream returned bytes and must not be added to them; neither is physical disk traffic. read_call_ns sums platform read-call elapsed time across streams and threads, not exclusive frame time.';
        candidate_io_note='Extended file.stream_policy/buffer/io/consumption rows retain each unique successful open link. Complete candidate totals require all four rows and original totals for every successful open, plus a file.stream_budget snapshot for each requested-candidate stream. Requested refill/seek can fall back; report effective counts separately. Net consumption/putback tracks get-pointer progress at refill/seek/destruction checkpoints, excluding explicit seek movement; it is not every-character service or unique asset footprint. owned_candidate_bytes_at_close_sum adds storage across closed stream lifetimes, not concurrent peak or current process memory. Budget snapshots occur after releasing that stream and use independent concurrent loads; observed active-count maxima are not continuous exact peaks or a coherent instantaneous state. Do not add returned, consumed, hash, retained or discarded bytes together as physical traffic.';
        lua_metrics_note='Lua allocation counters are inclusive of nested Lua calls. Do not add parent and child allocator deltas together or interpret growth minus shrink as per-script retained memory, OS residency, or GC pause duration.';
        catalog_metrics_note='resource_catalog_wall_ns includes catalog body work and below-threshold calls; its timer starts after the diagnostic scope enters and ends before aggregate counter updates and scope teardown. It excludes those recorder boundaries and deferred output. Catalog and other diagnostic envelopes can overlap; this counter is not total telemetry overhead.';
        phase_totals_note='Totals describe retained spans only. Inclusive totals overlap nested phases; self totals from different threads overlap in wall time. Windows thread CPU time is coarse and can exceed a short wall interval. Use ETW for precise running, ready and blocked intervals.'}
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-CAPTURE.json') -Encoding UTF8
    if($analysis){
        $analysis.phase_totals | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-PHASE-TOTALS.csv')
        $analysis.largest_unattributed_scopes | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-REMAINING-SCOPES.csv')
        $analysis.largest_dependency_scopes | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-DEPENDENCIES.json') -Encoding UTF8
        $analysis.diagnostic_phase_totals | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-DIAGNOSTIC-COST.csv')
    }
    if($requested -and $reasons.Count){Write-Warning ('INVALID HITCH CAPTURE: '+($reasons -join '; '))}
    return $result
}
