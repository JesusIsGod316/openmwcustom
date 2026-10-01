param([string]$SummaryPath='')
$ErrorActionPreference='Stop'
$totalWatch=[Diagnostics.Stopwatch]::StartNew()
. (Join-Path $PSScriptRoot 'OptimizedMW_SpikeReport.ps1')
$tempRoot=[IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')+'\'
$profile=Join-Path $tempRoot ('p9-spike-bounds-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $profile | Out-Null

# Synthetic inputs and verification only. The production parser, qualifier and
# bounded tree builder are called unchanged; no copied report implementation.
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Globalization;
using System.Collections.Generic;
using System.Text;
using System.Text.RegularExpressions;
public class P9SpikeBoundsCheck {
 public int trees,unbounded_trees,bounded_trees;
 public long context_references;
}
public static class P9SpikeBoundsFixture {
 public const int Ancestors=1000,Frames=6000;
 public const ulong Worker=1001;
 public const string PacingHeader="frame,epoch_ms,wall_ms,change_ms,baseline_ms,residual_ms,ratio,robust_score,relative_spike";
 static CultureInfo CI=CultureInfo.InvariantCulture;
 const long Epoch=1800000000000;
 static string Q(string s){return "\""+s.Replace("\"","\"\"")+"\"";}
 static void Row(StreamWriter w,ulong id,ulong parent,int frame,ulong thread,long start,long end,string phase,ulong link,double children) {
  double wall=(end-start)/1e6;
  w.WriteLine(id+","+parent+","+frame+","+thread+","+start+","+end+","+(Epoch+start/1000000)+","+
   Q(phase)+","+Q("bounds fixture")+","+Q("model, \"bounds\".nif")+","+wall.ToString("R",CI)+","+
   children.ToString("R",CI)+","+(wall-children).ToString("R",CI)+",-1,0,"+link+",\"\",0,\"\",0,\"\",0,\"\",0");
 }
 public static void Write(string profile) {
  string path=Path.Combine(profile,"p9-hitch-spans.csv");
  using(var w=new StreamWriter(path,false,new UTF8Encoding(false))) {
   w.WriteLine("span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link,metric1_name,metric1_value,metric2_name,metric2_value,metric3_name,metric3_value,metric4_name,metric4_value");
   for(ulong i=1;i<=Ancestors;i++)Row(w,i,i-1,0,1,1000000,2000000,"fixture.source_ancestor",0,i==Ancestors?0:1);
   Row(w,Worker,0,0,2,100000000,100000000+(long)Frames*25000000,"workqueue.execute",Ancestors,0);
   for(int frame=1;frame<=Frames;frame++) {
    long start=100000000+(long)(frame-1)*25000000;
    Row(w,Worker+(ulong)frame,0,frame,1,start,start+25000000,"frame.wall_interval",0,0);
   }
   w.WriteLine("# rows="+(Ancestors+1+Frames)+"\n# capacity=131072\n# dropped=0\n# allocation_failed=0\n# active_at_finish=0\n# normal_finish=1");
  }
  File.WriteAllText(path+".capture-status.txt","rows="+(Ancestors+1+Frames)+"\ncapacity=131072\ndropped=0\nallocation_failed=0\nactive_at_finish=0\nnormal_finish=1\noutput_ok=1\n");
  File.WriteAllText(Path.Combine(profile,"TEST_MODE.txt"),"phase9_hitch_spans=1\nfinal_exit_code=0\n");
  using(var w=new StreamWriter(Path.Combine(profile,"Phase9-FRAME-PACING.csv"),false,new UTF8Encoding(false))) {
   w.WriteLine(PacingHeader);
   for(int frame=1;frame<=Frames;frame++)w.WriteLine(frame+","+(Epoch+100+(long)(frame-1)*25)+",25,0,25,0,1,0,0");
  }
 }
 static string Field(string line,string name,string pattern) {
  var m=Regex.Match(line,"\""+name+"\":"+pattern,RegexOptions.CultureInvariant);
  if(!m.Success)throw new InvalidDataException("Missing or malformed tree field "+name);
  return m.Groups[1].Value;
 }
 public static P9SpikeBoundsCheck Check(string path,ulong[] nodeIds) {
  var allowed=new HashSet<ulong>(nodeIds);var r=new P9SpikeBoundsCheck();
  using(var reader=new StreamReader(path)) {
   string line;
   while((line=reader.ReadLine())!=null) {
    r.trees++;
    if(long.Parse(Field(line,"frame","([0-9]+)"),CI)!=r.trees)throw new InvalidDataException("Frame trees missing/reordered");
    ulong anchor=ulong.Parse(Field(line,"anchor_span","([0-9]+)"),CI);
    if(anchor!=Worker+(ulong)r.trees||!allowed.Contains(anchor))throw new InvalidDataException("Invalid anchor span ID");
    if(Field(line,"anchor_kind","\"([^\"]*)\"")!="exact_engine_wall_interval")throw new InvalidDataException("Lost exact frame interval");
    if(Field(line,"overlapping_worker_span_ids","\\[([0-9,]*)\\]")!=Worker.ToString(CI))throw new InvalidDataException("Worker overlap was lost or assigned by origin frame");
    foreach(string name in new[]{"same_thread_span_ids","main_thread_roots","largest_contained_unattributed_span_ids"})
     if(Field(line,name,"\\[([0-9,]*)\\]").Length!=0)throw new InvalidDataException("Out-of-interval ancestry became direct work");
    string context=Field(line,"causal_context_span_ids","\\[([0-9,]*)\\]");
    int count=0;
    if(context.Length!=0)foreach(string value in context.Split(',')) {
     ulong id=ulong.Parse(value,CI);
     if(id<1||id>Ancestors||!allowed.Contains(id))throw new InvalidDataException("Unknown or noncausal context span ID");
     count++;
    }
    bool bounded=Field(line,"node_reference_limit_reached","(true|false)")=="true";
    if(bounded){r.bounded_trees++;if(count>=Ancestors)throw new InvalidDataException("Complete context marked truncated");}
    else{r.unbounded_trees++;if(count!=Ancestors)throw new InvalidDataException("Partial context falsely marked complete");}
    r.context_references+=count;
   }
  }
  if(r.trees!=Frames||r.bounded_trees==0||r.unbounded_trees==0)throw new InvalidDataException("Context limit did not distinguish early complete/later truncated trees");
  return r;
 }
}
'@

try{
    [P9SpikeBoundsFixture]::Write($profile)
    $timeline=Join-Path $profile 'p9-hitch-spans.csv'
    $rawHash=(Get-FileHash -LiteralPath $timeline -Algorithm SHA256).Hash
    $reportWatch=[Diagnostics.Stopwatch]::StartNew()
    $report=Write-Phase9SpikeWorkReport -ProfileDir $profile
    $reportWatch.Stop()
    if($report.complete -or $null -eq $report.analysis -or $report.analysis.complete -or
        ($report.reasons -join '; ') -notmatch 'reference bound reached'){
        throw ('Context ancestry limit was not qualified incomplete: '+($report.reasons -join '; '))
    }
    if($report.analysis.references -ne 4000000 -or $report.analysis.selected_frames -ne 6000 -or
        $report.analysis.exact_anchors -ne 6000){throw 'Unexpected bounded reference/anchor counts'}
    $nodes=[Phase9.HitchReport]::ReadWorkNodes((Join-Path $profile 'HITCH-SPIKE-NODES.csv'))
    if($nodes.Count -ne 7001 -or $report.analysis.node_rows -ne 7001){throw 'Bounded derived node table lost immutable source identities'}
    $check=[P9SpikeBoundsFixture]::Check((Join-Path $profile 'HITCH-SPIKE-WORK-TREES.jsonl'),[UInt64[]]$nodes.Keys)
    if($check.context_references+6000 -ne $report.analysis.references){throw 'Ancestry did not share the direct-overlap reference budget'}
    if((Get-FileHash -LiteralPath $timeline -Algorithm SHA256).Hash -ne $rawHash){throw 'Bounded report modified raw evidence'}
    $cpu=Get-Content -Raw -LiteralPath (Join-Path $profile 'HITCH-CAPTURE.json') | ConvertFrom-Json
    if(-not $cpu.valid_span_capture -or $cpu.status.dropped -ne 0){throw 'Report bound invalidated a complete CPU timeline'}

    # A tiny oversized pacing row must fail before parsing/allocating tree data,
    # and regenerating a failed report must remove its previous partial outputs.
    [IO.File]::WriteAllLines((Join-Path $profile 'Phase9-FRAME-PACING.csv'),@([P9SpikeBoundsFixture]::PacingHeader,('x'*1025)))
    $rowFailure=Write-Phase9SpikeWorkReport -ProfileDir $profile
    if($rowFailure.complete -or $null -ne $rowFailure.analysis -or
        ($rowFailure.reasons -join '; ') -notmatch 'Pacing row bound exceeded'){throw 'Oversized pacing row bypassed input bound'}
    foreach($name in @('HITCH-SPIKE-WORK-TREES.jsonl','HITCH-SPIKE-NODES.csv')){
        if(Test-Path -LiteralPath (Join-Path $profile $name)){throw 'Failed pacing input retained stale derived nodes'}
    }
    if((Get-FileHash -LiteralPath $timeline -Algorithm SHA256).Hash -ne $rawHash){throw 'Rejected pacing input modified raw evidence'}
    $cpuAfter=Get-Content -Raw -LiteralPath (Join-Path $profile 'HITCH-CAPTURE.json') | ConvertFrom-Json
    if(-not $cpuAfter.valid_span_capture){throw 'Rejected pacing input invalidated independent CPU evidence'}
    $totalWatch.Stop()
    $summary=[pscustomobject]@{powershell_version=[string]$PSVersionTable.PSVersion;passed=$true;
        raw_rows=7001;selected_frames=6000;exact_anchors=6000;direct_overlap_references=6000;
        context_references=$check.context_references;references=$report.analysis.references;
        complete_trees=$check.unbounded_trees;truncated_trees=$check.bounded_trees;
        report_complete=$false;cpu_timeline_valid=$true;raw_sha256=$rawHash;
        pacing_row_bound_rejected=$true;stale_derived_outputs_removed=$true;
        report_elapsed_seconds=$reportWatch.Elapsed.TotalSeconds;total_elapsed_seconds=$totalWatch.Elapsed.TotalSeconds;
        timing_note='Elapsed times are recorded; no machine-speed assertion or gameplay performance conclusion.'}
    if($SummaryPath){$summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $SummaryPath -Encoding UTF8}
    $summary | ConvertTo-Json -Depth 5
    Write-Host 'PASS spike bounds: shared 4M overlap/context budget, per-tree truncation truth, exact anchors, immutable node IDs, unchanged raw evidence, independent CPU validity and pacing-row input bound.'
}finally{
    $absolute=[IO.Path]::GetFullPath($profile)
    if(-not $absolute.StartsWith($tempRoot,[StringComparison]::OrdinalIgnoreCase)){throw 'Unexpected fixture cleanup path'}
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
