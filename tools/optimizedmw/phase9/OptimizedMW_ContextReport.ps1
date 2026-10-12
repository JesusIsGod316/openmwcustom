function Get-OptimizedMWContextReport {
    param([Parameter(Mandatory=$true)][string]$ProfileDir)
    $statePath=Join-Path $ProfileDir 'benchmark-context.csv'
    $statusPath=$statePath+'.capture-status.txt'
    if(-not [IO.File]::Exists($statePath) -or -not [IO.File]::Exists($statusPath)){
        return [ordered]@{available=$false;qualified=$false;reason='No explicit state receipts; activity must not be inferred from physics timings'}
    }
    if(-not ('OptimizedMW.ContextReport' -as [type])){
        Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Linq;
using System.Globalization;
using System.Collections.Generic;
namespace OptimizedMW {
 public class ContextResult {
  public int state_rows,frame_rows,missing_state_frames,unused_state_rows,malformed_rows;
  public Dictionary<string,Dictionary<string,double>> cohorts=new Dictionary<string,Dictionary<string,double>>();
 }
 public static class ContextReport {
  static readonly CultureInfo CI=CultureInfo.InvariantCulture;
  static double Q(List<double> v,double p){double f=(v.Count-1)*p;int i=(int)f;return v[i]+(v[Math.Min(i+1,v.Count-1)]-v[i])*(f-i);}
  static string Cohort(uint flags){
   if((flags&64)!=0)return "frame_exception";
   if((flags&32)!=0)return "loading_or_viewer_advance";
   if((flags&8)!=0)return "hidden";
   if((flags&4)!=0)return "main_menu";
   if((flags&16)!=0)return "not_running";
   if((flags&1)!=0)return "paused";
   if((flags&2)!=0)return "gui";
   return "observed_active";
  }
  public static ContextResult Read(string framePath,string statePath){
   var r=new ContextResult();var states=new Dictionary<uint,uint>();var used=new HashSet<uint>();var seenFrames=new HashSet<uint>();
   using(var input=new StreamReader(statePath)){
    if(input.ReadLine()!="frame,viewer_end_frame,begin_flags,end_flags,observed_flags")throw new FormatException("Unknown state receipt header");
    string line;
    while((line=input.ReadLine())!=null){
     if(++r.state_rows>131072)throw new FormatException("State receipt exceeds fixed capacity");
     var p=line.Split(',');uint f,end,a,b,flags;
     if(p.Length!=5 || !uint.TryParse(p[0],out f) || !uint.TryParse(p[1],out end) || !uint.TryParse(p[2],out a) ||
        !uint.TryParse(p[3],out b) || !uint.TryParse(p[4],out flags) || (flags&~127u)!=0 || (a&~127u)!=0 || (b&~127u)!=0 ||
        (flags&a)!=a || (flags&b)!=b || (end!=f && (flags&32)==0) || states.ContainsKey(f))
      throw new FormatException("Malformed/duplicate state receipt");
     states.Add(f,flags);
    }
   }
   var values=new Dictionary<string,List<double>>();var deltas=new Dictionary<string,List<double>>();
   string previousCohort=null;uint previousFrame=0;double previousWall=0;
   using(var input=new StreamReader(framePath)){
    var header=(input.ReadLine()??"").Split(',');int fi=Array.IndexOf(header,"frame"),wi=Array.IndexOf(header,"wall_ms");
    if(fi<0 || wi<0)throw new FormatException("Missing frame timing columns");
    string line;
    while((line=input.ReadLine())!=null){
     var p=line.Split(',');uint frame;double wall;
     if(p.Length!=header.Length || !uint.TryParse(p[fi],out frame) ||
        !double.TryParse(p[wi],NumberStyles.Float,CI,out wall) || double.IsNaN(wall) || double.IsInfinity(wall) || wall<0){
      r.malformed_rows++;previousCohort=null;continue;
     }
     if(++r.frame_rows>131072)throw new FormatException("Frame series exceeds fixed capacity");
     if(!seenFrames.Add(frame))throw new FormatException("Duplicate frame timing ID");
     uint flags;string cohort;
     if(!states.TryGetValue(frame,out flags)){r.missing_state_frames++;cohort="unknown_state";}
     else {cohort=Cohort(flags);used.Add(frame);}
     if(!values.ContainsKey(cohort)){values[cohort]=new List<double>();deltas[cohort]=new List<double>();}
     values[cohort].Add(wall);
     if(previousCohort==cohort && frame>previousFrame && frame-previousFrame==1)deltas[cohort].Add(Math.Abs(wall-previousWall));
     previousCohort=cohort;previousFrame=frame;previousWall=wall;
    }
   }
   r.unused_state_rows=r.state_rows-used.Count;
   foreach(var item in values){
    var v=item.Value;v.Sort();double median=Q(v,.5),sum=v.Sum();
    var mad=v.Select(x=>Math.Abs(x-median)).OrderBy(x=>x).ToList();var changes=deltas[item.Key];changes.Sort();
    var m=new Dictionary<string,double>{{"frames",v.Count},{"engine_wall_seconds",sum/1000},{"mean_ms",sum/v.Count},
      {"median_ms",median},{"p95_ms",Q(v,.95)},{"p99_ms",Q(v,.99)},{"max_ms",v[v.Count-1]},
      {"interquartile_range_ms",Q(v,.75)-Q(v,.25)},{"median_absolute_deviation_ms",Q(mad,.5)},
      {"consecutive_pairs",changes.Count},{"p95_absolute_change_ms",changes.Count>0?Q(changes,.95):0}};
    r.cohorts[item.Key]=m;
   }
   return r;
  }
 }
}
'@
    }
    $status=@{}
    foreach($line in [IO.File]::ReadAllLines($statusPath)){
        $parts=$line -split '=',2
        if($parts.Count -eq 2){
            if($status.ContainsKey($parts[0])){return [ordered]@{available=$true;qualified=$false;reason='Duplicate context status key'}}
            $status[$parts[0]]=$parts[1]
        }
    }
    try{$result=[OptimizedMW.ContextReport]::Read((Join-Path $ProfileDir 'v3-frame.csv'),$statePath)}catch{
        return [ordered]@{available=$true;qualified=$false;reason=[string]$_}
    }
    $qualified=$status['schema'] -eq '1' -and $status['normal_finish'] -eq '1' -and $status['output_ok'] -eq '1' -and
        $status['allocation_failed'] -eq '0' -and $status['dropped'] -eq '0' -and $status['snapshots_dropped'] -eq '0' -and
        $status['unfinished_frames'] -eq '0' -and $status['rows'] -ceq [string]$result.state_rows -and
        $result.missing_state_frames -eq 0 -and $result.malformed_rows -eq 0
    return [ordered]@{available=$true;qualified=[bool]$qualified;schema=1;observed_state_cohorts=$result;
        receipt_status=$status;snapshots_file='benchmark-context.csv.snapshots.csv';
        interpretation='Main entry/after-state/main exit observations; any observed pause/menu/hidden/exception is retained in its labeled cohort. Viewer-advance/loading and immediate subsequent frames stay in the raw series. No frame-time cutoff.';
        exposure='Cohort engine-wall seconds; not continuous pause-state attestation or presentation time. Qualified joins do not establish an identical route/view/weather.';
        route_identity='User-declared route label plus low-rate pose/cell/weather snapshots; exact loaded save and all assets require independent attestation';
        missing_data='Unknown-state frames remain in unfiltered metrics; incomplete receipts prohibit state-qualified comparison'}
}
