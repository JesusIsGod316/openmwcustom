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
  public Dictionary<string,double> pacing;
  public List<PacingWindow> windows=new List<PacingWindow>();
  public List<PacingFrame> largest_local_deviations;
  public int frame_discontinuities,relative_spike_frames;
  public List<Cluster> clusters=new List<Cluster>();
  public List<uint> severe_frames=new List<uint>();
  public List<uint> other_spike_intervals=new List<uint>();
  public HashSet<uint> context_frames=new HashSet<uint>();
  public int malformed_rows;
 }
 public class PacingFrame {
  public uint frame;
  public long epoch_ms;
  public double wall_ms;
  public double? change_ms,baseline_ms,residual_ms,ratio,robust_score;
  public bool relative_spike;
 }
 public class PacingWindow {
  public uint start_frame,end_frame;
  public long start_epoch_ms,end_epoch_ms;
  public Dictionary<string,double> distribution,pacing;
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
   a=new List<double>(a);a.Sort();double sum=a.Sum();double mean=a.Count>0?sum/a.Count:double.NaN;
   double variance=a.Count>0?a.Sum(x=>(x-mean)*(x-mean))/a.Count:double.NaN;
   double median=Q(a,.5);var deviations=a.Select(x=>Math.Abs(x-median)).OrderBy(x=>x).ToList();
   int tailCount=(int)Math.Ceiling(a.Count*.01);double tailSum=a.Skip(a.Count-tailCount).Sum();
   return new Dictionary<string,double>{{"frames",a.Count},{"mean_ms",a.Count>0?sum/a.Count:double.NaN},
    {"median_ms",median},{"p01_ms",Q(a,.01)},{"p05_ms",Q(a,.05)},{"p25_ms",Q(a,.25)},
    {"p75_ms",Q(a,.75)},{"p90_ms",Q(a,.9)},{"p95_ms",Q(a,.95)},{"p99_ms",Q(a,.99)},{"p99_9_ms",Q(a,.999)},
    {"variance_ms_squared",variance},{"population_stddev_ms",Math.Sqrt(variance)},
    {"coefficient_of_variation",mean>0?Math.Sqrt(variance)/mean:double.NaN},
    {"interquartile_range_ms",Q(a,.75)-Q(a,.25)},{"median_absolute_deviation_ms",Q(deviations,.5)},
    {"p99_minus_median_ms",Q(a,.99)-median},{"slowest_one_percent_frames",tailCount},
    {"slowest_one_percent_mean_ms",tailCount>0?tailSum/tailCount:double.NaN},
    {"slowest_one_percent_wall_fraction",sum>0?tailSum/sum:double.NaN},
    {"max_ms",a.Count>0?a[a.Count-1]:double.NaN},{"gt33_333_frames",a.Count(x=>x>33.333333)},
    {"gt50_frames",a.Count(x=>x>50)},{"ge100_frames",a.Count(x=>x>=100)},
    {"time_over_33_333_ms",a.Sum(x=>Math.Max(0,x-33.333333))},{"captured_wall_ms",sum}};
  }
  static Dictionary<string,double> Pacing(List<PacingFrame> frames){
   var differences=frames.Where(x=>x.change_ms.HasValue).Select(x=>Math.Abs(x.change_ms.Value)).OrderBy(x=>x).ToList();
   return new Dictionary<string,double>{{"contiguous_pairs",differences.Count},
    {"mean_absolute_change_ms",differences.Count>0?differences.Average():double.NaN},
    {"rms_change_ms",differences.Count>0?Math.Sqrt(differences.Average(x=>x*x)):double.NaN},
    {"p95_absolute_change_ms",Q(differences,.95)},{"p99_absolute_change_ms",Q(differences,.99)},
    {"max_absolute_change_ms",differences.Count>0?differences.Last():double.NaN},
    {"relative_spike_frames",frames.Count(x=>x.relative_spike)}};
  }
  static void AddWindow(Result output,List<PacingFrame> window){
   if(window.Count==0)return;
   var local=window.Select(x=>new PacingFrame{frame=x.frame,wall_ms=x.wall_ms,change_ms=x.change_ms,relative_spike=x.relative_spike}).ToList();
   local[0].change_ms=null;
   output.windows.Add(new PacingWindow{start_frame=window[0].frame,end_frame=window.Last().frame,
    start_epoch_ms=window[0].epoch_ms,end_epoch_ms=window.Last().epoch_ms,
    distribution=Metrics(window.Select(x=>x.wall_ms).ToList()),pacing=Pacing(local)});
   window.Clear();
  }
  public static Result Read(string path,string pacingPath){
   var output=new Result();var all=new List<double>();var ordinary=new List<double>();
   var series=new List<PacingFrame>();var recent=new Queue<double>();var window=new List<PacingFrame>();PacingFrame previous=null;
   using(var reader=new StreamReader(path))using(var pacingWriter=new StreamWriter(pacingPath)){
    pacingWriter.WriteLine("frame,epoch_ms,wall_ms,change_ms,baseline_ms,residual_ms,ratio,robust_score,relative_spike");
    var header=Csv(reader.ReadLine());int fi=Array.IndexOf(header,"frame"),wi=Array.IndexOf(header,"wall_ms"),oi=Array.IndexOf(header,"other_ms"),ei=Array.IndexOf(header,"epoch_ms");
    if(fi<0||wi<0||oi<0||ei<0)throw new FormatException("Missing frame/wall/other/epoch columns");
    Cluster current=null;uint? previousOther=null;string line;
    while((line=reader.ReadLine())!=null){
     if(line.StartsWith("#")||string.IsNullOrWhiteSpace(line))continue;
     if(all.Count>=2000000)throw new InvalidOperationException("Frame report exceeds two-million-row safety bound; raw files retained");
     try {
      var row=Csv(line);uint frame=uint.Parse(row[fi],CI);double wall=Number(row[wi]);
      if(double.IsNaN(wall)||double.IsInfinity(wall)||wall<0)throw new FormatException();
      long epoch=long.Parse(row[ei],CI);
      // Validate the stage row before adding it to the distribution or sequence.
      double other=Number(row[oi]);
      for(int i=wi+1;i<=oi;i++)if(double.IsNaN(Number(row[i]))||double.IsInfinity(Number(row[i])))throw new FormatException();
      all.Add(wall);if(wall<100)ordinary.Add(wall);
      bool consecutive=previous!=null && previous.frame<uint.MaxValue && frame==previous.frame+1 && epoch>=previous.epoch_ms;
      if(previous!=null&&!consecutive){output.frame_discontinuities++;recent.Clear();AddWindow(output,window);current=null;previousOther=null;}
      var sample=new PacingFrame{frame=frame,epoch_ms=epoch,wall_ms=wall,change_ms=consecutive?(double?)(wall-previous.wall_ms):null};
      if(recent.Count>=15){
       var baseline=recent.OrderBy(x=>x).ToList();double median=Q(baseline,.5);
       var deviations=baseline.Select(x=>Math.Abs(x-median)).OrderBy(x=>x).ToList();
       sample.baseline_ms=median;sample.residual_ms=wall-median;sample.ratio=median>0?(double?)(wall/median):null;
       sample.robust_score=(wall-median)/Math.Max(1,1.4826*Q(deviations,.5));
       sample.relative_spike=sample.ratio>1.5 || sample.robust_score>3;
      }
      if(sample.relative_spike){output.relative_spike_frames++;if(frame>0)output.context_frames.Add(frame-1);output.context_frames.Add(frame);if(frame<uint.MaxValue)output.context_frames.Add(frame+1);}
      pacingWriter.WriteLine(string.Join(",",new[]{frame.ToString(CI),epoch.ToString(CI),wall.ToString("R",CI),
       sample.change_ms.HasValue?sample.change_ms.Value.ToString("R",CI):"",sample.baseline_ms.HasValue?sample.baseline_ms.Value.ToString("R",CI):"",
       sample.residual_ms.HasValue?sample.residual_ms.Value.ToString("R",CI):"",sample.ratio.HasValue?sample.ratio.Value.ToString("R",CI):"",
       sample.robust_score.HasValue?sample.robust_score.Value.ToString("R",CI):"",sample.relative_spike?"1":"0"}));
      series.Add(sample);window.Add(sample);if(window.Count==120)AddWindow(output,window);
      recent.Enqueue(wall);if(recent.Count>61)recent.Dequeue();previous=sample;
      if(wall>=100)output.severe_frames.Add(frame);
      if(other>=15){if(previousOther.HasValue)output.other_spike_intervals.Add(frame-previousOther.Value);previousOther=frame;}
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
   AddWindow(output,window);output.pacing=Pacing(series);
   output.largest_local_deviations=series.Where(x=>x.residual_ms.HasValue).OrderByDescending(x=>x.residual_ms).Take(25).ToList();
   output.unfiltered=Metrics(all);output.ordinary_under100=Metrics(ordinary);return output;
  }
  public static string Context(string source,string target,HashSet<uint> frames){
   using(var reader=new StreamReader(source))using(var writer=new StreamWriter(target)){
    string first=reader.ReadLine();if(first==null)return "empty";
    var headers=Csv(first);int index=-1;
    for(int i=0;i<headers.Length;i++)if(new[]{"frame","frame_number","frameNumber","osg_frame"}.Contains(headers[i])){index=i;break;}
    if(index<0)return "no_supported_frame_column";
    writer.WriteLine(first);string line;long read=0,selected=0,bad=0;
    while((line=reader.ReadLine())!=null){if(line.StartsWith("#")||string.IsNullOrWhiteSpace(line))continue;read++;try{var row=Csv(line);uint frame;if(index<row.Length&&uint.TryParse(row[index],out frame)&&frames.Contains(frame)){writer.WriteLine(line);selected++;}}catch(FormatException){bad++;}}
    return "read="+read+";selected="+selected+";malformed="+bad;
   }
  }
 }
}
'@
}
$framePath=Join-Path $ProfileDir 'v3-frame.csv'
if(-not (Test-Path -LiteralPath $framePath)){throw 'No completed frame capture; a crash can lose deferred frames.'}
$result=[Phase9.Report]::Read($framePath,(Join-Path $ProfileDir 'Phase9-FRAME-PACING.csv'))
$statuses=[ordered]@{}
foreach($name in @('v3-frame.csv.capture-status.txt','p8g4-render.capture-status.txt')){
    $file=Join-Path $ProfileDir $name
    $statuses[$name]=if(Test-Path -LiteralPath $file){Get-Content -LiteralPath $file}else{@('MISSING: deferred capture not confirmed complete; legacy mode has no deferred status')}
}
$qualifications=[ordered]@{}
foreach($name in @('WRITER-CAPTURE.json','TEMPORAL-CAPTURE.json','ROOT-CAUSE-CAPTURE.json','HITCH-CAPTURE.json')){
    $file=Join-Path $ProfileDir $name
    $qualifications[$name]=if(Test-Path -LiteralPath $file){Get-Content -Raw -LiteralPath $file | ConvertFrom-Json}else{$null}
}
$context=[ordered]@{}
foreach($name in @('v3-shadow.csv','p6-render-traversal.csv','p6-render-phase.csv','p4-compile.csv','v3-events.csv','v3-paging.csv','p8g4-render.csv','p9-draw-phases.csv','p9-dynamic-stream.csv')){
    $file=Join-Path $ProfileDir $name
    if(Test-Path -LiteralPath $file){$context[$name]=[Phase9.Report]::Context($file,(Join-Path $ProfileDir ('cluster-context-'+$name)),$result.context_frames)}
}
$summary=[ordered]@{
    format='phase9-offline-v3';scope='whole capture; historical matched route NOT automatically selected'
    cluster_rule='wall >=25ms; bridge at most two intervening non-slow frames; context includes frame +/-1'
    attribution='largest recorded component on the peak frame, NOT causal proof; nested/parallel scopes must not be summed'
    unfiltered=$result.unfiltered;ordinary_under100=$result.ordinary_under100
    frame_pacing=$result.pacing;contiguous_120_frame_windows=$result.windows
    largest_local_deviations=$result.largest_local_deviations;frame_discontinuities=$result.frame_discontinuities
    relative_spike_rule='Compare with the previous 61 contiguous frames, after 15 samples: wall/baseline median >1.5 OR (wall-median)/max(1ms,1.4826*MAD)>3. Thresholds flag investigation, not causal proof. Reset at gaps/reordering/clock reversal.'
    pacing_scope='Engine wall-time series, not display presentation intervals. Distribution uses every valid frame; adjacent changes only join consecutive IDs. Windows expose local drift without an absolute slow-frame cutoff. Under100 is a conditional view and must not replace unfiltered results.'
    severe_frames_ge100=$result.severe_frames;clusters=$result.clusters
    other_ms_ge15_intervals_frames=$result.other_spike_intervals
    malformed_frame_rows=$result.malformed_rows;capture_status=$statuses;auxiliary_context=$context
    channel_qualifications=$qualifications
    integrity_rule='Assess channels independently. A complete CPU series survives auxiliary loss; a claim requiring a lost channel remains unqualified. Reject promotion when its required records/output/normal finish are incomplete.'
    caveat='Periodic other_ms spikes are an unresolved instrumentation hypothesis, not removed or subtracted'
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $ProfileDir 'Phase9-BENCHMARK-REPORT.json') -Encoding UTF8
@(
    'Phase9 whole-capture report (not the historical matched late-exterior route)',
    ('Frames: {0}; median: {1:F3} ms; p95: {2:F3} ms; p99: {3:F3} ms; max: {4:F3} ms' -f $result.unfiltered['frames'],$result.unfiltered['median_ms'],$result.unfiltered['p95_ms'],$result.unfiltered['p99_ms'],$result.unfiltered['max_ms']),
    ('Clusters >=25ms: {0}; severe frames >=100ms: {1}; malformed rows: {2}' -f $result.clusters.Count,$result.severe_frames.Count,$result.malformed_rows),
    ('Spread: stddev {0:F3} ms; IQR {1:F3} ms; MAD {2:F3} ms; p99-median {3:F3} ms' -f $result.unfiltered['population_stddev_ms'],$result.unfiltered['interquartile_range_ms'],$result.unfiltered['median_absolute_deviation_ms'],$result.unfiltered['p99_minus_median_ms']),
    ('Consecutive-frame changes: mean absolute {0:F3} ms; RMS {1:F3} ms; p95 absolute {2:F3} ms; relative spikes {3}; discontinuities {4}' -f $result.pacing['mean_absolute_change_ms'],$result.pacing['rms_change_ms'],$result.pacing['p95_absolute_change_ms'],$result.relative_spike_frames,$result.frame_discontinuities),
    'Phase9-FRAME-PACING.csv includes every valid frame, signed change and prior rolling-baseline residual. JSON includes 120-frame local windows. Engine timing is not presentation timing.',
    'Inspect capture-status files before comparing. A missing GPU value is not zero.',
    'Inspect WRITER-CAPTURE, TEMPORAL-CAPTURE and ROOT-CAUSE-CAPTURE separately; requested ownership does not prove activation.',
    'cluster-context CSVs retain original frame identities, including adjacent frames.',
    'No telemetry cost has been subtracted. Largest scope is attribution, not proof.'
) | Set-Content -LiteralPath (Join-Path $ProfileDir 'Phase9-BENCHMARK-REPORT.txt') -Encoding UTF8
