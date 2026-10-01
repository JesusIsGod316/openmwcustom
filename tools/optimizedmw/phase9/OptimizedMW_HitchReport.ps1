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
   Dictionary<ulong,HitchSpan> spans;
   return ReadCore(path,attributionPath,out spans);
  }
  public static Dictionary<ulong,HitchSpan> ReadWorkNodes(string path) {
   Dictionary<ulong,HitchSpan> spans;
   ReadCore(path,null,out spans); return spans;
  }
  static HitchResult ReadCore(string path,string attributionPath,out Dictionary<ulong,HitchSpan> spans) {
   var result=new HitchResult(); spans=new Dictionary<ulong,HitchSpan>();
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
   // Frame intervals are synchronization anchors, not another measured work
   // envelope. Including them would charge the whole frame twice.
   var work=spans.Values.Where(s=>s.phase!="frame.wall_interval");
   result.largest_scopes=work.OrderByDescending(s=>s.wall_ms).Take(25).ToList();
   result.largest_unattributed_scopes=work.OrderByDescending(s=>s.unattributed_ms).Take(25).ToList();
   result.phase_totals=work.GroupBy(s=>s.phase).Select(g=>new HitchPhase {
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
 public class Diagnostics6Result {
  public int publications,first_retrievals,preparation_hits,retirements,invalidations,lua_vms;
  public ulong active,unknown_origins,unknown_generations,unknown_uses;
  public ulong capacity_failures,allocation_failures,metadata_truncations;
  public bool lifecycle_metadata_complete,origin_generation_provenance_complete;
  public string automatic_gc_observation="unavailable_without_vm_sampler";
  public Dictionary<string,int> retained_group_rows=new Dictionary<string,int>();
 }
 public static class Diagnostics6Report {
  static ulong M(HitchSpan s,string name) {
   ulong value;if(!s.metrics.TryGetValue(name,out value))throw new InvalidDataException("Missing diagnostics metric "+name);
   return value;
  }
  public static Diagnostics6Result Read(string path) {
   var nodes=HitchReport.ReadWorkNodes(path);var r=new Diagnostics6Result();
   var status=nodes.Values.Where(x=>x.phase=="resource.lifecycle_status").ToList();
   var coverage=nodes.Values.Where(x=>x.phase=="resource.lifecycle_coverage").ToList();
   if(status.Count!=1||coverage.Count!=1)throw new InvalidDataException("Requested lifecycle diagnostics require exactly one status and coverage receipt, including zero-resource runs");
   r.capacity_failures=M(status[0],"capacity_failures");r.allocation_failures=M(status[0],"allocation_failures");
   r.metadata_truncations=M(status[0],"metadata_truncations");r.active=M(coverage[0],"active");
   r.unknown_origins=M(coverage[0],"unknown_origins");r.unknown_generations=M(coverage[0],"unknown_generations");
   r.unknown_uses=M(coverage[0],"unknown_uses");
   var published=new Dictionary<ulong,HitchSpan>();var first=new HashSet<ulong>();var prepared=new HashSet<ulong>();var retired=new HashSet<ulong>();
   ulong visiblyUnknownOrigins=0;var visiblyUnknownGenerations=new HashSet<ulong>();
   foreach(var s in nodes.Values.Where(x=>x.phase=="resource.lifecycle_publish")) {
    ulong token=M(s,"lifecycle_id"),flags=M(s,"source_flags");
    if(token==0||token>65536||M(s,"resource_ptr")==0||(flags&~23UL)!=0||(flags&3)>2)
     throw new InvalidDataException("Invalid lifecycle publication identity/flags");
    if((flags&3)==2 && (s.link==0||nodes[s.link].phase!="resource.preparation_origin"))
     throw new InvalidDataException("Declared preparation publication has no exact preparation source");
    published.Add(token,s);
    if((flags&3)==0)visiblyUnknownOrigins++;
    if((flags&4)!=0)visiblyUnknownGenerations.Add(token);
   }
   var invalidated=new Dictionary<ulong,HitchSpan>();
   foreach(var s in nodes.Values.Where(x=>x.phase=="resource.lifecycle_invalidation")) {
    ulong token=M(s,"lifecycle_id"),flags=M(s,"source_flags"),reason=M(s,"reason_flags");HitchSpan p;
    if(!published.TryGetValue(token,out p)||s.identity!=p.identity||s.asset!=p.asset||s.link!=p.span||
       reason==0||reason>15||(flags&4)==0||(flags&~23UL)!=0||(flags&3)!=(M(p,"source_flags")&3)||
       (flags&M(p,"source_flags"))!=M(p,"source_flags")||(M(p,"source_flags")&4)!=0)
     throw new InvalidDataException("Incoherent exact resource lifetime invalidation");
    M(s,"observed_generation");invalidated.Add(token,s);visiblyUnknownGenerations.Add(token);
   }
   r.invalidations=invalidated.Count;
   foreach(var s in nodes.Values.Where(x=>x.phase=="resource.lifecycle_first_demand"||x.phase=="resource.lifecycle_prepare_hit"||x.phase=="resource.lifecycle_retire")) {
    ulong token=M(s,"lifecycle_id");HitchSpan p;
    if(!published.TryGetValue(token,out p)||s.identity!=p.identity||s.asset!=p.asset)
     throw new InvalidDataException("Missing/mismatched exact lifecycle publication");
    HitchSpan invalid;
    if(s.phase!="resource.lifecycle_retire"&&invalidated.TryGetValue(token,out invalid)&&s.span>invalid.span)
     throw new InvalidDataException("Invalidated resource residency acquired a later qualified retrieval/preparation hit");
    if(s.phase=="resource.lifecycle_prepare_hit") {
     if(!prepared.Add(token)||s.link==0||nodes[s.link].phase!="resource.preparation_origin"||
        M(s,"publication_span")!=p.span||M(s,"resource_ptr")!=M(p,"resource_ptr")||M(s,"vfs_generation")!=M(p,"vfs_generation"))
      throw new InvalidDataException("Incoherent lifecycle preparation hit");
    } else {
     if(s.link!=p.span)throw new InvalidDataException("Lifecycle receipt is linked to a different lifetime");
     if(s.phase=="resource.lifecycle_first_demand") {
      ulong flags=M(s,"source_flags");
      if(!first.Add(token)||M(s,"resource_ptr")!=M(p,"resource_ptr")||M(s,"cache_hit")>1||(flags&~23UL)!=0||(flags&3)>2)
       throw new InvalidDataException("Incoherent first resource API retrieval");
      ulong initialFlags=M(p,"source_flags");
      if((flags&3)!=(initialFlags&3)||(flags&initialFlags)!=initialFlags)
       throw new InvalidDataException("Lifecycle retrieval changed the immutable publication origin or removed flags");
      if((flags&4)!=0)visiblyUnknownGenerations.Add(token);
     } else if(!retired.Add(token)||M(s,"reason")<1||M(s,"reason")>7)
      throw new InvalidDataException("Incoherent lifecycle retirement");
    }
   }
   r.publications=published.Count;r.first_retrievals=first.Count;r.preparation_hits=prepared.Count;r.retirements=retired.Count;
   if(M(status[0],"publications")!=(ulong)r.publications||r.active!=(ulong)(r.publications-r.retirements)||
      r.unknown_origins!=visiblyUnknownOrigins||r.unknown_generations>(ulong)r.publications||
      r.unknown_generations<(ulong)visiblyUnknownGenerations.Count)
    throw new InvalidDataException("Lifecycle status disagrees with retained lifetime receipts");
   r.lifecycle_metadata_complete=r.capacity_failures==0&&r.allocation_failures==0&&r.metadata_truncations==0;
   r.origin_generation_provenance_complete=r.lifecycle_metadata_complete&&r.unknown_origins==0&&r.unknown_generations==0&&r.unknown_uses==0;
   foreach(var s in nodes.Values.Where(x=>x.phase=="lua.gc.observation")) {
    if(s.identity!="unavailable_without_vm_sampler"||M(s,"automatic_gc_observed")!=0||M(s,"vm_sampler_enabled")!=0||M(s,"tracking_available")>1)
     throw new InvalidDataException("Unsupported automatic Lua GC coverage claim");
    r.lua_vms++;
   }
   r.retained_group_rows.Add("preparation_to_first_api_retrieval",r.publications+r.first_retrievals+r.preparation_hits+r.invalidations);
   r.retained_group_rows.Add("model_construction",nodes.Values.Count(x=>x.phase.StartsWith("resource.model_",StringComparison.Ordinal)||x.phase.StartsWith("resource.template_",StringComparison.Ordinal)||x.phase.StartsWith("resource.clone_",StringComparison.Ordinal)));
   r.retained_group_rows.Add("physics_navigation",nodes.Values.Count(x=>x.phase.StartsWith("physics.",StringComparison.Ordinal)||x.phase.StartsWith("navigation.",StringComparison.Ordinal)||x.phase.StartsWith("nav.",StringComparison.Ordinal)));
   r.retained_group_rows.Add("queues_and_resource_retirement",nodes.Values.Count(x=>x.phase.StartsWith("workqueue.",StringComparison.Ordinal)||x.phase=="resource.lifecycle_retire"||x.phase.StartsWith("resource.cache_",StringComparison.Ordinal)||x.phase=="resource.prepared_owner_release"||x.phase=="resource.deferred_owner_release"));
   r.retained_group_rows.Add("lua_allocator_and_explicit_gc",nodes.Values.Count(x=>x.phase.StartsWith("lua.alloc.",StringComparison.Ordinal)||x.phase.StartsWith("lua.gc.",StringComparison.Ordinal)));
   return r;
  }
 }
 public class SpikeWorkResult {
  public int selected_frames,exact_anchors,missing_anchors,iteration_fallbacks;
  public int node_rows; public long references;
  public bool complete=true;
  public List<string> reasons=new List<string>();
 }
 public static class SpikeWorkReport {
  static CultureInfo CI=CultureInfo.InvariantCulture;
  class Event {
   public long frame; public double wall;
   public string[] pacing; public bool relative;
   public HitchSpan anchor;
   public string anchor_kind="missing";
   public bool references_truncated;
   public List<HitchSpan> same=new List<HitchSpan>(),workers=new List<HitchSpan>();
  }
  static string J(string text) {
   var b=new StringBuilder("\"");
   foreach(char c in text??"") {
    if(c=='"' || c=='\\')b.Append('\\').Append(c);
    else if(c<32)b.Append("\\u").Append(((int)c).ToString("x4",CI));
    else b.Append(c);
   }
   return b.Append('"').ToString();
  }
  static string Number(string value) {
   if(value.Length==0)return "null";
   double number=double.Parse(value,CI);
   if(double.IsNaN(number)||double.IsInfinity(number))throw new InvalidDataException("Nonfinite pacing value");
   return number.ToString("R",CI);
  }
  static string Ids(IEnumerable<HitchSpan> rows) {return "["+string.Join(",",rows.Select(x=>x.span.ToString(CI)).ToArray())+"]";}
  static double UnionMs(List<HitchSpan> rows,HitchSpan anchor) {
   long start=0,end=0,total=0; bool active=false;
   foreach(var span in rows.OrderBy(x=>x.start_ns)) {
    long a=Math.Max(span.start_ns,anchor.start_ns),b=Math.Min(span.end_ns,anchor.end_ns);
    if(b<=a)continue;
    if(!active){start=a;end=b;active=true;}
    else if(a<=end)end=Math.Max(end,b);
    else{total+=end-start;start=a;end=b;}
   }
   if(active)total+=end-start;
   return total/1e6;
  }
  public static SpikeWorkResult Write(string timeline,string pacing,string output,long[] extraFrames) {
   var result=new SpikeWorkResult();
   var nodes=HitchReport.ReadWorkNodes(timeline);
   var extras=new HashSet<long>(extraFrames??new long[0]);
   var events=new List<Event>();var seen=new HashSet<long>();
   if(extras.Count>524288 || new FileInfo(pacing).Length>67108864)
    throw new InvalidDataException("Pacing input bound exceeded");
   using(var reader=new StreamReader(pacing)) {
    if(reader.ReadLine()!="frame,epoch_ms,wall_ms,change_ms,baseline_ms,residual_ms,ratio,robust_score,relative_spike")
     throw new InvalidDataException("Unsupported pacing schema");
    string line;
    while((line=reader.ReadLine())!=null) {
     if(line.Length>1024 || seen.Count>=524288)throw new InvalidDataException("Pacing row bound exceeded");
     var f=HitchReport.Csv(line);
     if(f.Length!=9)throw new InvalidDataException("Malformed pacing row");
     long frame=long.Parse(f[0],CI);double wall=double.Parse(f[2],CI);
     if(!seen.Add(frame)||wall<0||double.IsNaN(wall)||double.IsInfinity(wall)||
        (f[8]!="0"&&f[8]!="1"))throw new InvalidDataException("Incoherent pacing row");
     if(f[8]=="1"||wall>=25||extras.Contains(frame))events.Add(new Event{frame=frame,wall=wall,pacing=f,relative=f[8]=="1"});
     // Validate fields used by the output before opening derived files.
     Number(f[1]);Number(f[4]);Number(f[5]);
     if(events.Count>131072)throw new InvalidDataException("Selected pacing frame bound exceeded");
    }
   }
   result.selected_frames=events.Count;
   var anchorGroups=nodes.Values.Where(x=>x.phase=="frame.wall_interval").GroupBy(x=>x.frame).ToDictionary(x=>x.Key,x=>x.ToList());
   var iterations=nodes.Values.Where(x=>x.phase=="engine.iteration").GroupBy(x=>x.frame).ToDictionary(x=>x.Key,x=>x.ToList());
   foreach(var item in events) {
    List<HitchSpan> anchors;
    if(anchorGroups.TryGetValue(item.frame,out anchors)) {
     if(anchors.Count!=1)throw new InvalidDataException("Duplicate exact frame anchor");
     item.anchor=anchors[0];item.anchor_kind="exact_engine_wall_interval";result.exact_anchors++;
     if(Math.Abs(item.anchor.wall_ms-item.wall)>.002)throw new InvalidDataException("Frame anchor and pacing duration disagree");
     if(item.anchor.epoch_ms!=long.Parse(item.pacing[1],CI))
      throw new InvalidDataException("Frame anchor and pacing epoch disagree; sources may belong to different runs");
    } else if(iterations.TryGetValue(item.frame,out anchors)&&anchors.Count==1) {
     item.anchor=anchors[0];item.anchor_kind="iteration_only_partial_frame";result.iteration_fallbacks++;
    } else result.missing_anchors++;
   }
   if(result.missing_anchors+result.iteration_fallbacks>0) {
    result.complete=false;result.reasons.Add("Some selected frames have no exact engine-wall anchor; iteration fallback is partial coverage");
   }
   var anchored=events.Where(x=>x.anchor!=null).OrderBy(x=>x.anchor.start_ns).ToList();
   for(int i=1;i<anchored.Count;i++) {
    if(anchored[i-1].anchor.end_ns>anchored[i].anchor.start_ns)
     throw new InvalidDataException("Selected frame anchors overlap");
   }
   const long referenceLimit=4000000;
   bool bounded=false;
   foreach(var span in nodes.Values) {
    if(span.phase=="frame.wall_interval"||span.end_ns<=span.start_ns)continue;
    int low=0,high=anchored.Count;
    while(low<high){int mid=low+(high-low)/2;if(anchored[mid].anchor.end_ns<=span.start_ns)low=mid+1;else high=mid;}
    for(int i=low;i<anchored.Count&&anchored[i].anchor.start_ns<span.end_ns;i++) {
     if(result.references>=referenceLimit){bounded=true;break;}
     var item=anchored[i];
     if(span.thread==item.anchor.thread)item.same.Add(span);else item.workers.Add(span);
     result.references++;
    }
    if(bounded)break;
   }
   var used=new HashSet<ulong>();
   string eventPath=Path.Combine(output,"HITCH-SPIKE-WORK-TREES.jsonl");
   // If overlap scanning itself stopped, omitted spans could affect any
   // selected frame. Qualify each tree conservatively rather than guessing.
   if(bounded)foreach(var item in events)item.references_truncated=true;
   using(var writer=new StreamWriter(eventPath,false,new UTF8Encoding(false))) {
    foreach(var item in events.OrderBy(x=>x.frame)) {
     var direct=new HashSet<ulong>(item.same.Concat(item.workers).Select(x=>x.span));
     var context=new HashSet<ulong>();var todo=new Queue<ulong>(direct);
     if(item.anchor!=null)used.Add(item.anchor.span);
     while(todo.Count>0) {
      var node=nodes[todo.Dequeue()];
      foreach(ulong source in new[]{node.parent,node.link}) {
       if(source!=0&&!direct.Contains(source)&&!context.Contains(source)) {
        if(result.references>=referenceLimit){bounded=true;item.references_truncated=true;todo.Clear();break;}
        context.Add(source);todo.Enqueue(source);result.references++;
       }
      }
      if(item.references_truncated && result.references>=referenceLimit)break;
     }
     foreach(ulong id in direct.Concat(context))used.Add(id);
     var a=item.anchor;
     string roots=Ids(item.same.Where(x=>x.parent==0||!direct.Contains(x.parent)).OrderBy(x=>x.start_ns));
     var remaining=a==null?new List<HitchSpan>():item.same.Where(x=>x.start_ns>=a.start_ns&&x.end_ns<=a.end_ns)
       .OrderByDescending(x=>x.unattributed_ms).Take(10).ToList();
     double covered=a==null?0:UnionMs(item.same,a);
     writer.WriteLine("{\"frame\":"+item.frame+",\"wall_ms\":"+Number(item.pacing[2])+
      ",\"relative_spike\":"+(item.relative?"true":"false")+",\"absolute_slow\":"+(item.wall>=25?"true":"false")+
      ",\"selected_local_deviation\":"+(extras.Contains(item.frame)?"true":"false")+
      ",\"baseline_ms\":"+Number(item.pacing[4])+",\"residual_ms\":"+Number(item.pacing[5])+
      ",\"anchor_kind\":"+J(item.anchor_kind)+",\"anchor_span\":"+(a==null?"null":a.span.ToString(CI))+
      ",\"node_reference_limit_reached\":"+(item.references_truncated?"true":"false")+
      ",\"main_thread_roots\":"+roots+",\"same_thread_span_ids\":"+Ids(item.same.OrderBy(x=>x.start_ns))+
      ",\"overlapping_worker_span_ids\":"+Ids(item.workers.OrderBy(x=>x.start_ns))+
      ",\"causal_context_span_ids\":["+string.Join(",",context.OrderBy(x=>x).Select(x=>x.ToString(CI)).ToArray())+"]"+
      ",\"largest_contained_unattributed_span_ids\":"+Ids(remaining)+
      ",\"main_thread_envelope_covered_ms\":"+covered.ToString("R",CI)+
      ",\"outside_retained_main_envelopes_ms\":"+(a==null?"null":Math.Max(0,a.wall_ms-covered).ToString("R",CI))+"}");
    }
   }
   // Preserve the original CSV rows rather than reformatting or rounding times.
   using(var source=new StreamReader(timeline))using(var target=new StreamWriter(Path.Combine(output,"HITCH-SPIKE-NODES.csv"),false,new UTF8Encoding(false))) {
    target.WriteLine(source.ReadLine());string line;
    while((line=source.ReadLine())!=null) {
     if(line.Length==0||line.StartsWith("#"))continue;
     int comma=line.IndexOf(',');ulong id=ulong.Parse(line[0]=='"'?HitchReport.Csv(line)[0]:line.Substring(0,comma),CI);
     if(used.Contains(id)){target.WriteLine(line);result.node_rows++;}
    }
   }
   if(bounded){result.complete=false;result.reasons.Add("Selected-frame overlap/context reference bound reached; raw timeline remains authoritative");}
   return result;
  }
 }
}
'@
}

function Test-Phase9Diagnostics6Capture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $mode=Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt')
    $requested=$mode -match '(?m)^hitch_diagnostics_revision=1\r?$'
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null
    if($requested){
        try{
            $hitch=Test-Phase9HitchCapture -ProfileDir $ProfileDir
            if($hitch.valid_span_capture -ne $true){throw 'Expanded diagnostic joins require an independently valid CPU timeline'}
            $analysis=[Phase9.Diagnostics6Report]::Read((Join-Path $ProfileDir 'p9-hitch-spans.csv'))
            if(-not $analysis.lifecycle_metadata_complete){throw 'Lifecycle metadata capacity, allocation or truncation loss; preparation/retrieval provenance is incomplete'}
        }catch{$reasons.Add([string]$_)}
    }
    $report=[pscustomobject]@{schema=1;revision=1;requested=$requested;
        valid_diagnostics_capture=$(if($requested){$reasons.Count -eq 0}else{$null});analysis=$analysis;reasons=$reasons.ToArray();
        groups=@('Per-spike work trees (HITCH-SPIKE-REPORT.json)','Preparation to first resource API retrieval','Model construction','Physics/navigation work and waits','Queues and cache/owner retirement','Lua allocator service and explicit GC');
        automatic_gc_observation='unavailable_without_vm_sampler';all_causes_observed=$false;
        provenance_scope='Exact lifetime tokens cover scene templates, NIF files, keyframes and prepared scene instances. First demand means first required resource API retrieval, not first visible draw or GPU use. Unknown origins/generations/uses remain explicit; absence is not expiry.';
        retirement_scope='Cache entry/owner release, with observed remaining reference count and retained-size estimate. This does not prove object destruction or returned OS memory.';
        activation_scope='Retained row counts indicate observed paths, not all calls. Zero rows can mean a path did not run or was below retention thresholds; it does not prove zero cost. Lifecycle status is mandatory even for zero-resource runs.';
        lua_scope='Allocator service measures actual free/realloc call wall time and attempted request sizes. Explicit GC steps retain the actual completion result and heap endpoints. Inclusive Lua deltas overlap nested calls. Automatic GC during arbitrary Lua execution remains unresolved.';
        integrity_rule='This family is qualified separately from the CPU timeline and per-spike joins. Metadata loss prevents exact provenance claims but does not invalidate otherwise complete CPU wall-time evidence. Consult HITCH-SPIKE-REPORT.json separately for frame-tree completeness.'}
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-DIAGNOSTICS6-CAPTURE.json') -Encoding UTF8
    if($requested -and $reasons.Count){Write-Warning ('INCOMPLETE EXPANDED DIAGNOSTICS: '+($reasons -join '; '))}
    return $report
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
