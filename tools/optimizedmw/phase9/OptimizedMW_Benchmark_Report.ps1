param([Parameter(Mandatory=$true)][string]$ProfileDir)
$ErrorActionPreference='Stop'
# Post-run only. No Python dependency and no whole-file Import-Csv materialization.
if(-not ('Phase9.Report' -as [type])){
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.Globalization;
namespace Phase9 {
 public class Cluster {
  public uint start_frame,end_frame,peak_frame;
  public int slow_frames;
  public double peak_ms,slow_frame_time_ms,excess_over_25_ms;
  public string largest_recorded_component;
 }
 public class Result {
  public Dictionary<string,double> unfiltered,ordinary_under100;
  public List<Cluster> clusters=new List<Cluster>();
  public List<uint> severe_frames=new List<uint>();
  public List<uint> other_spike_intervals=new List<uint>();
  public HashSet<uint> context_frames=new HashSet<uint>();
  public int malformed_rows;
 }
 public static class Report {
  static readonly CultureInfo CI=CultureInfo.InvariantCulture;
  public static string[] Csv(string line) {
   var result=new List<string>(); var field=new System.Text.StringBuilder(); bool quoted=false;
   for(int i=0;i<line.Length;i++){
    char c=line[i];
    if(c=='"'){ if(quoted && i+1<line.Length && line[i+1]=='"'){field.Append('"');i++;} else quoted=!quoted; }
    else if(c==',' && !quoted){result.Add(field.ToString());field.Clear();}
    else field.Append(c);
   }
   if(quoted) throw new FormatException("Multiline/malformed CSV record");
   result.Add(field.ToString());return result.ToArray();
  }
  static double Number(string s){return double.Parse(s,NumberStyles.Float,CI);}
  static double Q(List<double> a,double q){if(a.Count==0)return double.NaN;double x=(a.Count-1)*q;int lo=(int)x;int hi=Math.Min(lo+1,a.Count-1);return a[lo]+(a[hi]-a[lo])*(x-lo);}
  static Dictionary<string,double> Metrics(List<double> a){
   a.Sort();double sum=a.Sum();
   return new Dictionary<string,double>{{"frames",a.Count},{"mean_ms",a.Count>0?sum/a.Count:double.NaN},
    {"median_ms",Q(a,.5)},{"p95_ms",Q(a,.95)},{"p99_ms",Q(a,.99)},
    {"max_ms",a.Count>0?a[a.Count-1]:double.NaN},{"gt33_333_frames",a.Count(x=>x>33.333333)},
    {"gt50_frames",a.Count(x=>x>50)},{"ge100_frames",a.Count(x=>x>=100)},
    {"time_over_33_333_ms",a.Sum(x=>Math.Max(0,x-33.333333))},{"captured_wall_ms",sum}};
  }
  public static Result Read(string path){
   var output=new Result();var all=new List<double>();var ordinary=new List<double>();
   using(var reader=new StreamReader(path)){
    var header=Csv(reader.ReadLine());int fi=Array.IndexOf(header,"frame"),wi=Array.IndexOf(header,"wall_ms"),oi=Array.IndexOf(header,"other_ms");
    if(fi<0||wi<0||oi<0)throw new FormatException("Missing frame/wall/other columns");
    Cluster current=null;uint? previousOther=null;string line;
    while((line=reader.ReadLine())!=null){
     if(all.Count>=2000000)throw new InvalidOperationException("Frame report exceeds two-million-row safety bound; raw files retained");
     try {
      var row=Csv(line);uint frame=uint.Parse(row[fi],CI);double wall=Number(row[wi]);
      if(!double.IsNaN(wall)&&!double.IsInfinity(wall)&&wall>=0){all.Add(wall);if(wall<100)ordinary.Add(wall);}else throw new FormatException();
      if(wall>=100)output.severe_frames.Add(frame);
      if(Number(row[oi])>=15){if(previousOther.HasValue)output.other_spike_intervals.Add(frame-previousOther.Value);previousOther=frame;}
      if(wall<25)continue;
      string component="other_ms";double biggest=Number(row[oi]);
      for(int i=wi+1;i<oi;i++){
       if(header[i]=="accounted_ms")continue;double value=Number(row[i]);
       if(value>biggest){biggest=value;component=header[i];}
      }
      if(current==null||frame>current.end_frame+3){current=new Cluster{start_frame=frame,largest_recorded_component=component};output.clusters.Add(current);}
      current.end_frame=frame;current.slow_frames++;current.slow_frame_time_ms+=wall;current.excess_over_25_ms+=wall-25;
      if(wall>current.peak_ms){current.peak_ms=wall;current.peak_frame=frame;current.largest_recorded_component=component;}
      if(frame>0)output.context_frames.Add(frame-1);output.context_frames.Add(frame);if(frame<uint.MaxValue)output.context_frames.Add(frame+1);
     } catch(FormatException){output.malformed_rows++;} catch(IndexOutOfRangeException){output.malformed_rows++;} catch(OverflowException){output.malformed_rows++;}
    }
   }
   output.unfiltered=Metrics(all);output.ordinary_under100=Metrics(ordinary);return output;
  }
  public static string Context(string source,string target,HashSet<uint> frames){
   using(var reader=new StreamReader(source))using(var writer=new StreamWriter(target)){
    string first=reader.ReadLine();if(first==null)return "empty";
    var headers=Csv(first);int index=-1;
    for(int i=0;i<headers.Length;i++)if(new[]{"frame","frame_number","frameNumber","osg_frame"}.Contains(headers[i])){index=i;break;}
    if(index<0)return "no_supported_frame_column";
    writer.WriteLine(first);string line;long read=0,selected=0,bad=0;
    while((line=reader.ReadLine())!=null){read++;try{var row=Csv(line);uint frame;if(index<row.Length&&uint.TryParse(row[index],out frame)&&frames.Contains(frame)){writer.WriteLine(line);selected++;}}catch(FormatException){bad++;}}
    return "read="+read+";selected="+selected+";malformed="+bad;
   }
  }
 }
}
'@
}
$framePath=Join-Path $ProfileDir 'v3-frame.csv'
if(-not (Test-Path -LiteralPath $framePath)){throw 'No completed frame capture; a crash can lose deferred frames.'}
$result=[Phase9.Report]::Read($framePath)
$statuses=[ordered]@{}
foreach($name in @('v3-frame.csv.capture-status.txt','p8g4-render.capture-status.txt')){
    $file=Join-Path $ProfileDir $name
    $statuses[$name]=if(Test-Path -LiteralPath $file){Get-Content -LiteralPath $file}else{@('MISSING: deferred capture not confirmed complete; legacy mode has no deferred status')}
}
$context=[ordered]@{}
foreach($name in @('v3-shadow.csv','p6-render-traversal.csv','p6-render-phase.csv','p4-compile.csv','v3-events.csv','v3-paging.csv','p8g4-render.csv','p9-draw-phases.csv','p9-draw-phases.csv.frames.csv','p9-dynamic-stream.csv','p9-gl-calls.csv','p9-gl-calls.csv.frames.csv')){
    $file=Join-Path $ProfileDir $name
    if(Test-Path -LiteralPath $file){$context[$name]=[Phase9.Report]::Context($file,(Join-Path $ProfileDir ('cluster-context-'+$name)),$result.context_frames)}
}
$summary=[ordered]@{
    format='phase9-offline-v2';scope='whole capture; historical matched route NOT automatically selected'
    cluster_rule='wall >=25ms; bridge at most two intervening non-slow frames; context includes frame +/-1'
    attribution='largest recorded component on the peak frame, NOT causal proof; nested/parallel scopes must not be summed'
    unfiltered=$result.unfiltered;ordinary_under100=$result.ordinary_under100
    severe_frames_ge100=$result.severe_frames;clusters=$result.clusters
    other_ms_ge15_intervals_frames=$result.other_spike_intervals
    malformed_frame_rows=$result.malformed_rows;capture_status=$statuses;auxiliary_context=$context
    trace_health=$(if(Test-Path -LiteralPath (Join-Path $ProfileDir 'TRACE-HEALTH.json')){Get-Content -Raw -LiteralPath (Join-Path $ProfileDir 'TRACE-HEALTH.json') | ConvertFrom-Json}else{'NOT_RECORDED'})
    render_trace_limits='Leaf time includes selected GL calls: do not sum them. Core GL entry points, render-stage setup and driver internals remain outside the selected API coverage. Adjacent-frame extraction is context, not forced causality.'
    integrity_rule='Reject performance promotion when records dropped, allocation/output failed, normal_finish=0, malformed rows or missing clean-mode status'
    caveat='Periodic other_ms spikes are an unresolved instrumentation hypothesis, not removed or subtracted'
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ProfileDir 'Phase9-BENCHMARK-REPORT.json') -Encoding UTF8
@(
    'Phase9 whole-capture report (not the historical matched late-exterior route)',
    ('Frames: {0}; median: {1:F3} ms; p95: {2:F3} ms; p99: {3:F3} ms; max: {4:F3} ms' -f $result.unfiltered['frames'],$result.unfiltered['median_ms'],$result.unfiltered['p95_ms'],$result.unfiltered['p99_ms'],$result.unfiltered['max_ms']),
    ('Clusters >=25ms: {0}; severe frames >=100ms: {1}; malformed rows: {2}' -f $result.clusters.Count,$result.severe_frames.Count,$result.malformed_rows),
    'Inspect capture-status files before comparing. A missing GPU value is not zero.',
    'cluster-context CSVs retain original frame identities, including adjacent frames.',
    'No telemetry cost has been subtracted. Largest scope is attribution, not proof.'
) | Set-Content -LiteralPath (Join-Path $ProfileDir 'Phase9-BENCHMARK-REPORT.txt') -Encoding UTF8
