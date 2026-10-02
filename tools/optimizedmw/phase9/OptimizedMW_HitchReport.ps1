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
 public sealed class TimelineAnalysisBoundException : IOException {
  public TimelineAnalysisBoundException(string message) : base(message) {}
 }
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
  public LuaEventDecodeResult lua_event_decode=new LuaEventDecodeResult();
 }
 public class LuaEventDecodeResult {
  public bool metadata_available;
  public int retained_dispatches,retained_decoders,retained_handlers,slow_shapes,decoders_without_supplemental_shape,decode_errors,empty_nil,unattempted;
  public ulong retained_payload_bytes,retained_values,retained_tables,retained_string_bytes,retained_custom_bytes,max_visited_depth;
  public string scope="Retained event receipts only. Fast decode supplemental string/custom shape is unavailable, not zero. Error shape describes attempted/visited prefix, not a validated complete payload. Logical serialized bytes do not equal allocator footprint or disk traffic.";
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
 public class ObserverCounter {
  public ulong thread_slot,thread,calls,wall_ns;
  public string category;
  public bool wall_timing_available;
 }
 public class ObserverLedgerResult {
  public int rows,thread_slots;
  public List<ObserverCounter> counters=new List<ObserverCounter>();
 }
 public static class ObserverLedgerReport {
  public static ObserverLedgerResult Read(string path) {
   var result=new ObserverLedgerResult();var keys=new HashSet<string>();var threads=new Dictionary<ulong,ulong>();
   var ledgerCalls=new Dictionary<ulong,ulong>();var measuredCalls=new Dictionary<ulong,ulong>();
   var categories=new HashSet<string>(new[]{"trace_scope_entry","trace_scope_exit","trace_append","trace_retained","trace_filtered",
    "catalog_work","catalog_path_collection","catalog_publication","catalog_counter_update","lua_allocator_bookkeeping","lua_allocator_timer_boundary","lua_decode_timer_boundary","ledger_update"});
   var countOnly=new HashSet<string>(new[]{"trace_retained","trace_filtered","lua_allocator_timer_boundary","lua_decode_timer_boundary","ledger_update"});
   if(new FileInfo(path).Length>2097152)throw new InvalidDataException("Observer ledger exceeds 2 MiB bound");
   using(var reader=new StreamReader(path)) {
    if(reader.ReadLine()!="schema,thread_slot,thread,category,calls,wall_ns")throw new InvalidDataException("Unsupported observer ledger schema");
    string line;while((line=reader.ReadLine())!=null) {
     if(line.Length==0)continue;if(line.Length>512||result.rows>=3328)throw new InvalidDataException("Observer ledger row bound exceeded");
     var f=HitchReport.Csv(line);if(f.Length!=6||f[0]!="1")throw new InvalidDataException("Malformed observer ledger row");
     var item=new ObserverCounter{thread_slot=ulong.Parse(f[1],CultureInfo.InvariantCulture),thread=ulong.Parse(f[2],CultureInfo.InvariantCulture),
      category=f[3],calls=ulong.Parse(f[4],CultureInfo.InvariantCulture),wall_ns=ulong.Parse(f[5],CultureInfo.InvariantCulture)};
     if(item.thread_slot>=256||item.thread==0||item.calls==0||!categories.Contains(item.category)||!keys.Add(f[1]+":"+item.category))throw new InvalidDataException("Invalid/duplicate observer counter identity");
     item.wall_timing_available=!countOnly.Contains(item.category);
     if(!item.wall_timing_available&&item.wall_ns!=0)throw new InvalidDataException("Count-only observer category invented timing");
     ulong id;if(threads.TryGetValue(item.thread_slot,out id)&&id!=item.thread)throw new InvalidDataException("Observer native thread changed within one lifetime slot");
     threads[item.thread_slot]=item.thread;
     if(item.category=="ledger_update")ledgerCalls[item.thread_slot]=item.calls;
     else {ulong calls;measuredCalls.TryGetValue(item.thread_slot,out calls);measuredCalls[item.thread_slot]=checked(calls+item.calls);}
     result.counters.Add(item);result.rows++;
    }
   }
   foreach(var slot in threads.Keys) {
    ulong updates,calls;if(!ledgerCalls.TryGetValue(slot,out updates)||!measuredCalls.TryGetValue(slot,out calls)||updates!=calls)throw new InvalidDataException("Observer ledger updates contradict category call counts");
   }
   result.thread_slots=threads.Count;return result;
  }
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
   // Streaming capture capacity is a concurrent queue bound, not a lifetime
   // row limit. Keep post-game joins explicitly bounded independently of the
   // producer/writer integrity receipt.
   if(new FileInfo(path).Length>2147483648L) throw new TimelineAnalysisBoundException("Timeline exceeds 2 GiB post-run analysis bound; raw transport receipt is independent");
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
     if(line.Length>4096)throw new InvalidDataException("Timeline text row exceeds 4096-byte analysis bound");
     if(spans.Count>=4000000)throw new TimelineAnalysisBoundException("Timeline exceeds four-million-row post-run analysis bound; raw transport receipt is independent");
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
   var eventSummary=result.lua_event_decode;
   var newDispatch=spans.Values.Where(s=>s.phase=="lua.event.dispatch"&&s.metrics.ContainsKey("decode_outcome")).ToDictionary(s=>s.span);
   var decoders=new Dictionary<ulong,HitchSpan>();var shapes=new HashSet<ulong>();
   eventSummary.metadata_available=newDispatch.Count!=0;eventSummary.retained_dispatches=newDispatch.Count;
   foreach(var s in newDispatch.Values) {
    ulong outcome=Metric(s,"decode_outcome"),registered=0,invoked=Metric(s,"invoked_handlers");
    if(!s.metrics.TryGetValue("handler_count",out registered)&&outcome!=0)throw new InvalidDataException("Decoded dispatch lacks registered handler count");
    if(outcome>3||invoked>registered)throw new InvalidDataException("Invalid retained Lua event dispatch outcome/counts");
    if(outcome==0)eventSummary.unattempted++;if(outcome==2)eventSummary.decode_errors++;if(outcome==3)eventSummary.empty_nil++;
   }
   foreach(var s in spans.Values.Where(s=>s.phase=="lua.event.deserialize"&&s.metrics.ContainsKey("value_count"))) {
    HitchSpan dispatch;if(s.link==0||!newDispatch.TryGetValue(s.link,out dispatch)||s.identity!=dispatch.identity||s.asset!=dispatch.asset||
      Metric(s,"payload_bytes")!=Metric(dispatch,"payload_bytes"))throw new InvalidDataException("Lua event decoder lacks its exact dispatch receipt/payload size");
    ulong outcome=Metric(dispatch,"decode_outcome");if(outcome!=1&&outcome!=2&&outcome!=3)throw new InvalidDataException("Decoded Lua receipt disagrees with dispatch outcome");
    if(outcome==3&&(Metric(s,"payload_bytes")!=0||Metric(s,"value_count")!=0||Metric(s,"table_count")!=0||Metric(s,"max_depth")!=0))
     throw new InvalidDataException("Empty/nil Lua receipt has nonempty serialized payload/visited shape");
    decoders.Add(s.span,s);eventSummary.retained_decoders++;
    Add(ref eventSummary.retained_payload_bytes,Metric(s,"payload_bytes"));Add(ref eventSummary.retained_values,Metric(s,"value_count"));
    Add(ref eventSummary.retained_tables,Metric(s,"table_count"));eventSummary.max_visited_depth=Math.Max(eventSummary.max_visited_depth,Metric(s,"max_depth"));
   }
   foreach(var s in spans.Values.Where(s=>s.phase=="lua.event.payload_shape")) {
    HitchSpan decoder;if(s.link==0||!decoders.TryGetValue(s.link,out decoder)||s.identity!=decoder.identity||s.asset!=decoder.asset||!shapes.Add(s.link))
     throw new InvalidDataException("Lua payload shape lacks its exact unique decode receipt");
    if(decoder.end_ns-decoder.start_ns<250000)throw new InvalidDataException("Lua supplemental shape was attached to a fast decode");
    ulong strings=Metric(s,"string_count"),custom=Metric(s,"custom_values");
    if(Metric(newDispatch[decoder.link],"decode_outcome")==3&&(strings!=0||custom!=0||Metric(s,"string_bytes")!=0||Metric(s,"custom_bytes")!=0))
     throw new InvalidDataException("Empty/nil Lua receipt has nonempty supplemental shape");
    Add(ref eventSummary.retained_string_bytes,Metric(s,"string_bytes"));Add(ref eventSummary.retained_custom_bytes,Metric(s,"custom_bytes"));eventSummary.slow_shapes++;
   }
   foreach(var s in spans.Values.Where(s=>s.phase=="lua.event.handler"&&s.link!=0)) {
    HitchSpan dispatch;if(!newDispatch.TryGetValue(s.link,out dispatch)||s.identity!=dispatch.asset)throw new InvalidDataException("Lua handler lacks its exact event dispatch receipt");
    eventSummary.retained_handlers++;
   }
   eventSummary.decoders_without_supplemental_shape=eventSummary.retained_decoders-eventSummary.slow_shapes;
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
  public ulong status_span_id,coverage_span_id;
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
   r.status_span_id=status[0].span;r.coverage_span_id=coverage[0].span;
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
    $requested=($mode -match '(?m)^phase9_hitch_spans=1\r?$') -and ($mode -match '(?m)^hitch_diagnostics_revision=[12]\r?$')
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null
    $lifecycleRequested=$mode -match '(?m)^hitch_lifecycle_status=1\r?$'
    $lifecycle=@{};$lifecycleValid=$null;$lifecycleReasons=[Collections.Generic.List[string]]::new()
    $lifecyclePath=Join-Path $ProfileDir 'p9-hitch-spans.csv.lifecycle-status.txt'
    if($lifecycleRequested -or (Test-Path -LiteralPath $lifecyclePath)){
        $lifecycleValid=$false
        try{
            $lifecycle=Read-Phase9HitchReceipt -Path $lifecyclePath -Numeric
            $lifecycleKeys=@('schema','registry_capacity','status_span_id','coverage_span_id','publications','active','first_demands','prepare_hits','retirements',
                'capacity_failures','allocation_failures','metadata_truncations','unknown_origins','unknown_generations','unknown_uses','normal_finish','output_ok')
            foreach($key in $lifecycleKeys){
                if(-not $lifecycle.ContainsKey($key)){throw "Missing independent lifecycle status: $key"}
            }
            foreach($key in $lifecycle.Keys){if($key -notin $lifecycleKeys){throw "Unsupported independent lifecycle field: $key"}}
            if($lifecycle.schema -ne 1 -or $lifecycle.registry_capacity -ne 65536 -or $lifecycle.publications -gt $lifecycle.registry_capacity -or
                $lifecycle.status_span_id -eq 0 -or $lifecycle.coverage_span_id -le $lifecycle.status_span_id -or
                $lifecycle.retirements -gt $lifecycle.publications -or $lifecycle.active -ne $lifecycle.publications-$lifecycle.retirements -or
                $lifecycle.first_demands -gt $lifecycle.publications -or $lifecycle.prepare_hits -gt $lifecycle.publications -or
                $lifecycle.unknown_origins -gt $lifecycle.publications -or $lifecycle.unknown_generations -gt $lifecycle.publications -or
                $lifecycle.normal_finish -ne 1 -or $lifecycle.output_ok -ne 1){throw 'Invalid independent lifecycle status or shutdown'}
            $lifecycleValid=$true
        }catch{$lifecycleReasons.Add([string]$_)}
    }
    if($requested){
        try{
            $hitch=Test-Phase9HitchCapture -ProfileDir $ProfileDir
            if($hitch.valid_span_capture -ne $true){throw 'Expanded diagnostic joins require an independently valid CPU timeline'}
            $analysis=[Phase9.Diagnostics6Report]::Read((Join-Path $ProfileDir 'p9-hitch-spans.csv'))
            if($lifecycleValid -eq $true){
                foreach($key in @('status_span_id','coverage_span_id','publications','active','retirements','capacity_failures','allocation_failures','metadata_truncations','unknown_origins','unknown_generations','unknown_uses')){
                    if($lifecycle[$key] -ne $analysis.$key){throw "Independent lifecycle status disagrees with retained receipts: $key"}
                }
                if($lifecycle.first_demands -ne $analysis.first_retrievals -or $lifecycle.prepare_hits -ne $analysis.preparation_hits){throw 'Independent lifecycle retrieval/preparation counts disagree with exact receipts'}
            }
            if(-not $analysis.lifecycle_metadata_complete){throw 'Lifecycle metadata capacity, allocation or truncation loss; preparation/retrieval provenance is incomplete'}
        }catch{$reasons.Add([string]$_)}
        if(($lifecycleRequested -or (Test-Path -LiteralPath $lifecyclePath)) -and $lifecycleValid -ne $true){foreach($reason in $lifecycleReasons){$reasons.Add($reason)}}
    }
    $report=[pscustomobject]@{schema=2;revision=$(if($mode -match '(?m)^hitch_diagnostics_revision=2\r?$'){2}else{1});requested=$requested;
        valid_diagnostics_capture=$(if($requested){$reasons.Count -eq 0}else{$null});analysis=$analysis;reasons=$reasons.ToArray();
        independent_lifecycle_status=$lifecycle;independent_lifecycle_status_valid=$lifecycleValid;independent_lifecycle_status_reasons=$lifecycleReasons.ToArray();
        independent_lifecycle_status_scope='Final registry snapshot survives CPU queue loss and is qualified separately. Complete snapshot counters do not prove missing event lifetimes, API retrieval joins, object destruction or OS memory return.';
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

function Read-Phase9HitchReceipt {
    param([Parameter(Mandatory=$true)][string]$Path,[switch]$Numeric)
    $values=@{}
    if((Get-Item -LiteralPath $Path).Length -gt 65536){throw "Capture receipt exceeds 64 KiB bound: $Path"}
    foreach($line in [IO.File]::ReadAllLines($Path)){
        if($line -notmatch '^([a-z][a-z0-9_]*)=(.*)$'){throw "Malformed capture receipt: $Path"}
        $key=[string]$matches[1];$value=[string]$matches[2]
        if($values.ContainsKey($key)){throw "Duplicate capture receipt key: $key"}
        if($Numeric -and $key -notin @('transport','minimum_detail_ms','forced_dependency_events','writer_timing_scope','queue_high_water_scope','flush_failure_scope')){
            if($value -notmatch '^\d+$'){throw "Invalid unsigned capture receipt: $key"}
            $values[$key]=[UInt64]::Parse($value,[Globalization.CultureInfo]::InvariantCulture)
        }else{$values[$key]=$value}
    }
    return $values
}

function Assert-Phase9HitchTransportReceipt {
    param([Parameter(Mandatory=$true)][Collections.IDictionary]$Status,[string]$Mode)
    foreach($key in @('rows','capacity','dropped','allocation_failed','active_at_finish','normal_finish','output_ok')){
        if(-not $Status.Contains($key)){throw "Missing timeline status: $key"}
    }
    if($Status.Contains('schema') -and $Status.schema -ne 2){throw 'Unsupported CPU timeline row schema'}
    $newRequested=$Mode -match '(?m)^(phase9_hitch_transport_schema|hitch_transport_schema)=1\r?$'
    if(-not $Status.Contains('transport_schema')){
        if($newRequested -or $Status.Contains('transport')){throw 'Requested transport receipt is missing its schema'}
        if($Status.capacity -notin @(131072,262144,524288) -or $Status.rows -gt $Status.capacity){throw 'Invalid legacy deferred timeline capacity or row count'}
    }else{
        if($Status.transport_schema -ne 1 -or $Status.transport -notin @('stream_mpsc_v1','deferred_arena_v1')){throw 'Unsupported timeline transport schema or backend'}
        if(-not $Status.Contains('schema') -or $Status.schema -ne 2){throw 'Missing or unsupported typed transport row schema'}
        $required=@('attempted','accepted','written','pending','capacity_dropped','contention_dropped','allocation_dropped','io_dropped','stopped_dropped',
            'initialization_failed','writer_failed','drain_complete','active_producers_at_finish','queue_capacity','queue_high_water','row_size_bytes','storage_bytes','writer_thread_id',
            'writer_started_ns','writer_finished_ns','writer_cpu_ns','writer_cpu_available','writer_format_ns','writer_write_ns','writer_batches','writer_serialized_bytes')
        foreach($key in $required){if(-not $Status.Contains($key)){throw "Missing transport accounting: $key"}}
        $known=@('rows','capacity','dropped','allocation_failed','active_at_finish','normal_finish','output_ok','schema','transport_schema','transport',
            'minimum_detail_ms','forced_dependency_events','writer_timing_scope','queue_high_water_scope','flush_failure_scope')+$required
        foreach($key in $Status.Keys){if($key -notin $known){throw "Unsupported transport accounting field: $key"}}
        # Decimal retains exact uint64 sums and cannot wrap at UInt64.MaxValue.
        $producerLoss=[decimal]$Status.capacity_dropped+[decimal]$Status.contention_dropped+[decimal]$Status.allocation_dropped+[decimal]$Status.stopped_dropped
        if([decimal]$Status.attempted -ne [decimal]$Status.accepted+$producerLoss -or
            [decimal]$Status.accepted -ne [decimal]$Status.written+[decimal]$Status.pending+[decimal]$Status.io_dropped -or
            [decimal]$Status.dropped -ne $producerLoss+[decimal]$Status.io_dropped -or $Status.rows -ne $Status.written){throw 'Contradictory producer/writer accounting'}
        if($Status.capacity -le 0 -or $Status.row_size_bytes -le 0 -or $Status.queue_high_water -gt $Status.capacity){throw 'Invalid transport storage/high-water accounting'}
        if([decimal]$Status.storage_bytes -lt [decimal]$Status.capacity*[decimal]$Status.row_size_bytes -or $Status.active_producers_at_finish -ne 0){throw 'Invalid allocated storage or active producer shutdown'}
        if($Status.transport -eq 'deferred_arena_v1' -and $Status.rows -gt $Status.capacity){throw 'Deferred arena rows exceed storage capacity'}
        if(($Status.transport -eq 'stream_mpsc_v1' -and $Status.queue_capacity -ne $Status.capacity) -or
            ($Status.transport -eq 'deferred_arena_v1' -and $Status.queue_capacity -ne 0)){throw 'Transport queue capacity contradicts selected backend'}
        if($Status.transport -eq 'stream_mpsc_v1' -and $Status.writer_thread_id -eq 0){throw 'Streaming writer native thread identity is missing'}
        if($Status.writer_started_ns -gt $Status.writer_finished_ns -or $Status.writer_cpu_available -notin @(0,1)){throw 'Invalid writer clock or CPU availability'}
        if($Status.pending -ne 0 -or $Status.initialization_failed -ne 0 -or $Status.writer_failed -ne 0 -or $Status.drain_complete -ne 1){throw 'Transport initialization, writer failure or incomplete shutdown/drain'}
        $match=[regex]::Match($Mode,'(?m)^phase9_hitch_transport=(\w+)\r?$')
        if($match.Success -and $match.Groups[1].Value -ne $Status.transport){throw 'Requested and effective hitch transports differ'}
    }
    if($Status.dropped -ne 0 -or $Status.allocation_failed -ne 0 -or $Status.active_at_finish -ne 0 -or
        $Status.normal_finish -ne 1 -or $Status.output_ok -ne 1){throw 'Timeline loss or incomplete output'}
}

function Get-Phase9ObserverWorkSummary {
    param([Collections.IDictionary]$Status,$Analysis,[bool]$TransportValid,[string]$ProfileDir,[string]$Mode)
    $writer=$null
    if($Status.Contains('transport_schema')){
        $writer=[pscustomobject]@{thread_id=$Status.writer_thread_id;
            clock_start_ns=$Status.writer_started_ns;clock_end_ns=$Status.writer_finished_ns;
            format_wall_ms=$(if($Status.Contains('writer_format_ns')){$Status.writer_format_ns/1000000.0}else{$null});
            write_wall_ms=$(if($Status.Contains('writer_write_ns')){$Status.writer_write_ns/1000000.0}else{$null});
            cpu_ms=$(if($Status.writer_cpu_available -eq 1){$Status.writer_cpu_ns/1000000.0}else{$null});
            batches=$Status.writer_batches;serialized_bytes=$Status.writer_serialized_bytes;integrity_complete=$TransportValid;
            interval_scope='Recorded writer drain-loop interval; final footer/flush/close and physical thread exit are excluded.';
            timing_scope=$Status.writer_timing_scope;flush_failure_scope=$Status.flush_failure_scope}
    }
    $ledger=$null;$ledgerStatus=@{};$ledgerValid=$null;$ledgerReasons=[Collections.Generic.List[string]]::new()
    $ledgerRequested=($Mode -match '(?m)^phase9_hitch_spans=1\r?$') -and ($Mode -match '(?m)^hitch_observer_revision=1\r?$')
    $ledgerPath=if($ProfileDir){Join-Path $ProfileDir 'p9-hitch-spans.csv.observer.csv'}else{''}
    if($ledgerRequested -or ($ledgerPath -and (Test-Path -LiteralPath $ledgerPath))){
        $ledgerValid=$false
        try{
            $ledgerStatus=Read-Phase9HitchReceipt -Path ($ledgerPath.Replace('.observer.csv','.observer-status.txt'))
            $observerNumbers=@('schema','thread_capacity','thread_slots','rows','ledger_overflow','normal_finish','output_ok','coverage_complete')
            foreach($key in $observerNumbers){
                if(-not $ledgerStatus.ContainsKey($key) -or $ledgerStatus[$key] -notmatch '^\d+$'){throw "Missing/invalid observer ledger status: $key"}
                $ledgerStatus[$key]=[UInt64]::Parse($ledgerStatus[$key],[Globalization.CultureInfo]::InvariantCulture)
            }
            $localMode=[regex]::Match($Mode,'(?m)^hitch_observer_local=([01])\r?$')
            $localFields=[regex]::Matches($Mode,'(?m)^hitch_observer_local=([^\r\n]*)\r?$')
            if($localFields.Count -gt 1 -or ($localFields.Count -eq 1 -and -not $localMode.Success)){throw 'Duplicate/invalid requested observer aggregation'}
            $publicationNumbers=@('producer_threads_joined','unretired_producers','active_observer_scopes','retired_updates_rejected')
            $hasPublication=@($publicationNumbers | Where-Object {$ledgerStatus.ContainsKey($_)}).Count -gt 0
            if($ledgerStatus.ContainsKey('aggregation') -or $hasPublication -or ($localMode.Success -and $localMode.Groups[1].Value -eq '1')){
                if($ledgerStatus.aggregation -notin @('legacy_atomic','owner_local_retired_snapshot')){throw 'Missing/unsupported observer aggregation'}
                foreach($key in $publicationNumbers){
                    if(-not $ledgerStatus.ContainsKey($key) -or $ledgerStatus[$key] -notmatch '^\d+$'){throw "Missing/invalid observer publication status: $key"}
                    $ledgerStatus[$key]=[UInt64]::Parse($ledgerStatus[$key],[Globalization.CultureInfo]::InvariantCulture)
                }
                $effectiveLocal=$ledgerStatus.aggregation -eq 'owner_local_retired_snapshot'
                if(($localMode.Success -and $effectiveLocal -ne ($localMode.Groups[1].Value -eq '1')) -or
                    $ledgerStatus.producer_threads_joined -ne [UInt64][int]$effectiveLocal -or
                    $ledgerStatus.unretired_producers -ne 0 -or $ledgerStatus.active_observer_scopes -ne 0 -or
                    $ledgerStatus.retired_updates_rejected -ne 0){throw 'Observer join/publication coverage or requested aggregation is incomplete'}
            }
            if($ledgerStatus.schema -ne 1 -or $ledgerStatus.thread_capacity -ne 256 -or $ledgerStatus.thread_slots -gt 256 -or
                $ledgerStatus.rows -gt 3328 -or $ledgerStatus.ledger_overflow -ne 0 -or $ledgerStatus.normal_finish -ne 1 -or
                $ledgerStatus.output_ok -ne 1 -or $ledgerStatus.coverage_complete -ne 1 -or
                $ledgerStatus.ledger_update_timing -ne 'UNMEASURED_count_only_no_recursive_timers' -or
                $ledgerStatus.lua_allocator_timer_boundary_timing -ne 'UNMEASURED_count_only_existing_clock_calls_excludes_allocator_service' -or
                $ledgerStatus.lua_decode_timer_boundary_timing -ne 'UNMEASURED_count_only_extra_end_clock_reuses_scope_start' -or
                $ledgerStatus.count_only_categories -ne 'trace_retained,trace_filtered,ledger_update,lua_allocator_timer_boundary,lua_decode_timer_boundary'){throw 'Observer ledger loss, incomplete output or unsupported timing coverage'}
            $observerKnown=$observerNumbers+$publicationNumbers+@('aggregation','ledger_update_timing','lua_allocator_timer_boundary_timing','lua_decode_timer_boundary_timing','count_only_categories','timing_note','scope')
            foreach($key in $ledgerStatus.Keys){if($key -notin $observerKnown){throw "Unsupported observer ledger field: $key"}}
            $ledger=[Phase9.ObserverLedgerReport]::Read($ledgerPath)
            if($ledger.rows -ne $ledgerStatus.rows -or $ledger.thread_slots -ne $ledgerStatus.thread_slots){throw 'Observer ledger row/thread counts disagree with independent status'}
            $ledgerValid=$true
        }catch{$ledgerReasons.Add([string]$_)}
    }
    $result=[pscustomobject]@{schema=2;writer=$writer;retained_diagnostic_envelopes=$(if($Analysis){$Analysis.diagnostic_phase_totals}else{$null});
        ledger_requested=$ledgerRequested;ledger=$ledger;ledger_status=$ledgerStatus;ledger_valid=$ledgerValid;ledger_reasons=$ledgerReasons.ToArray();
        thread_identity='Each slot identifies an observed producer thread lifetime; native Windows IDs require the separately qualified Windows clock map. Recycled native IDs may occupy different slots.';
        count_only_categories=@('trace_retained','trace_filtered','lua_allocator_timer_boundary','lua_decode_timer_boundary','ledger_update');
        scope='Writer formatting/output work is independent of engine span envelopes. Retained diagnostic scopes omit fast filtered calls. Producer scope setup, clocks, recording boundaries and other observers are not totalled here.';
        ledger_scope='Fixed producer counters include calls below trace retention thresholds. Inclusive catalog_work overlaps path/publication; trace_scope_exit overlaps append. Ledger updates and allocator timer boundaries are counted with explicitly unmeasured elapsed time.';
        accounting='Wall intervals from multiple threads overlap; CPU clocks can be unavailable or coarse. Writer bytes are serialized diagnostic output, not asset disk traffic. No diagnostic average is subtracted from any frame.';
        all_observer_work_measured=$false}
    if($ProfileDir){$result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-OBSERVER-WORK.json') -Encoding UTF8}
    return $result
}

function Test-Phase9HitchCapture {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $mode=Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'TEST_MODE.txt')
    $requested=$mode -match '(?m)^phase9_hitch_spans=1\r?$'
    $reasons=[Collections.Generic.List[string]]::new();$analysis=$null;$status=@{};$clock=@{}
    $transportReasons=[Collections.Generic.List[string]]::new();$clockReasons=[Collections.Generic.List[string]]::new()
    $transportValid=$false;$cpuJoinsValid=$null;$analysisComplete=$false;$analysisBounded=$false
    $clockRequested=$requested -and ($mode -match '(?m)^phase9_detail_revision=[2-9]\r?$')
    $clockValid=$null
    $path=Join-Path $ProfileDir 'p9-hitch-spans.csv'
    if($requested){
        # Clock calibration is independently useful even when rows are lost.
        if($clockRequested -or (Test-Path -LiteralPath ($path+'.clock-map.txt'))){
            $clockValid=$false
            try{
                $clock=Read-Phase9HitchReceipt -Path ($path+'.clock-map.txt')
                foreach($key in @('process_id','thread_id_kind','qpc_frequency','start_qpc_before','start_qpc_after','start_steady_ns','end_qpc_before','end_qpc_after','end_steady_ns','normal_finish')){
                    if(-not $clock.ContainsKey($key)){throw "Missing ETW clock mapping: $key"}
                }
                if($clock.thread_id_kind -ne 'windows_native_tid' -or [long]$clock.process_id -le 0 -or [long]$clock.qpc_frequency -le 0 -or
                    [long]$clock.start_qpc_before -gt [long]$clock.start_qpc_after -or
                    [long]$clock.end_qpc_before -gt [long]$clock.end_qpc_after -or
                    [long]$clock.start_steady_ns -gt [long]$clock.end_steady_ns -or $clock.normal_finish -ne '1'){
                    throw 'Invalid ETW clock calibration or incomplete shutdown'
                }
                $clockValid=$true
            }catch{$clockReasons.Add([string]$_)}
        }
        try{
            $status=Read-Phase9HitchReceipt -Path ($path+'.capture-status.txt') -Numeric
            Assert-Phase9HitchTransportReceipt -Status $status -Mode $mode
            $stream=[IO.File]::OpenRead($path);$reader=$null
            try{
                [void]$stream.Seek([Math]::Max(0,$stream.Length-65536),[IO.SeekOrigin]::Begin)
                $reader=[IO.StreamReader]::new($stream);$tail=$reader.ReadToEnd()
            }finally{if($reader){$reader.Dispose()}else{$stream.Dispose()}}
            $footerKeys=@('rows','capacity','dropped','allocation_failed','active_at_finish','normal_finish')
            if($status.ContainsKey('transport_schema')){
                $footerKeys=@($status.Keys)
                foreach($entry in [regex]::Matches($tail,'(?m)^# ([a-z][a-z0-9_]*)=')){
                    if(-not $status.ContainsKey($entry.Groups[1].Value)){throw ('Unknown typed timeline footer field: '+$entry.Groups[1].Value)}
                }
            }
            foreach($key in $footerKeys){
                $pattern='(?m)^# '+[regex]::Escape($key)+'='+[regex]::Escape([string]$status[$key])+'\r?$'
                if([regex]::Matches($tail,'(?m)^# '+[regex]::Escape($key)+'=').Count -ne 1 -or $tail -notmatch $pattern){throw "Missing, duplicate or inconsistent timeline footer: $key"}
            }
            if($mode -notmatch '(?m)^final_exit_code=0\r?$'){throw 'Game did not exit normally'}
            $transportValid=$true
        }catch{$transportReasons.Add([string]$_)}
        foreach($reason in $transportReasons){$reasons.Add($reason)}
        if($transportValid){
            $cpuJoinsValid=$false
            try{
                $analysis=[Phase9.HitchReport]::Read($path)
                if($analysis.rows -ne $status.rows -or $analysis.rows -eq 0){throw 'Empty or inconsistent timeline row count'}
                $cpuJoinsValid=$true;$analysisComplete=$true
            }catch{
                if($_.Exception.GetBaseException().GetType().FullName -eq 'Phase9.TimelineAnalysisBoundException'){
                    $analysisBounded=$true;$cpuJoinsValid=$null
                }
                $reasons.Add([string]$_)
            }
        }
        # Preserve the existing combined validity contract for consumers while
        # exposing independent CPU joins and clock qualification explicitly.
        if($clockRequested -and $clockValid -ne $true){foreach($reason in $clockReasons){$reasons.Add($reason)}}
    }
    $result=[pscustomobject]@{schema=3;requested=$requested;valid_span_capture=$(if($requested){$reasons.Count -eq 0}else{$null});
        status=$status;analysis=$analysis;clock_mapping=$clock;reasons=$reasons.ToArray();
        transport_receipt_valid=$(if($requested){$transportValid}else{$null});transport_reasons=$transportReasons.ToArray();
        valid_internal_span_joins=$cpuJoinsValid;report_analysis_complete=$analysisComplete;report_analysis_bounded=$analysisBounded;
        clock_mapping_requested=$clockRequested;clock_mapping_valid=$clockValid;clock_mapping_reasons=$clockReasons.ToArray();
        observer_work=(Get-Phase9ObserverWorkSummary -Status $status -Analysis $analysis -TransportValid $transportValid -ProfileDir $ProfileDir -Mode $mode);
        integrity_scope='Producer/writer receipts, internal parent/dependency joins, clock calibration and bounded post-run reports are separate qualifications. A valid transport receipt alone does not certify all raw semantic joins or per-spike report completeness.';
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
    foreach($name in @('HITCH-PHASE-TOTALS.csv','HITCH-REMAINING-SCOPES.csv','HITCH-DEPENDENCIES.json','HITCH-DIAGNOSTIC-COST.csv')){
        [IO.File]::Delete((Join-Path $ProfileDir $name))
    }
    if($analysis){
        $analysis.phase_totals | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-PHASE-TOTALS.csv')
        $analysis.largest_unattributed_scopes | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-REMAINING-SCOPES.csv')
        $analysis.largest_dependency_scopes | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-DEPENDENCIES.json') -Encoding UTF8
        $analysis.diagnostic_phase_totals | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-DIAGNOSTIC-COST.csv')
    }
    if($requested -and $reasons.Count){Write-Warning ('INVALID HITCH CAPTURE: '+($reasons -join '; '))}
    return $result
}
