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
  public ulong span,parent,thread;
  public long frame,start_ns,end_ns,epoch_ms;
  public string phase,identity,asset;
  public double wall_ms,children_ms,unattributed_ms,thread_cpu_ms;
  public int identity_truncated;
 }
 public class HitchResult {
  public int rows,roots,truncated_identities;
  public List<string> reasons=new List<string>();
  public List<HitchSpan> largest_scopes,largest_unattributed_scopes;
  public List<HitchPhase> phase_totals;
 }
 public class HitchPhase {
  public string phase;
  public int retained_calls,cpu_samples;
  public double inclusive_ms,self_ms,max_ms,observed_thread_cpu_ms;
 }
 public static class HitchReport {
  static CultureInfo CI=CultureInfo.InvariantCulture;
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
  public static HitchResult Read(string path) {
   var result=new HitchResult(); var spans=new Dictionary<ulong,HitchSpan>();
   if(new FileInfo(path).Length>268435456) throw new InvalidDataException("Timeline exceeds 256 MiB bound");
   using(var reader=new StreamReader(path)) {
    string header=reader.ReadLine();
    if(header!="span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated")
     throw new InvalidDataException("Unsupported timeline schema");
    string line;
    while((line=reader.ReadLine())!=null) {
     if(line.Length==0 || line.StartsWith("#"))continue;
     if(line.Length>4096 || spans.Count>=262144)throw new InvalidDataException("Timeline row bound exceeded");
     var f=Csv(line); if(f.Length!=15)throw new InvalidDataException("Malformed timeline row");
     var s=new HitchSpan{span=ulong.Parse(f[0],CI),parent=ulong.Parse(f[1],CI),frame=long.Parse(f[2],CI),
      thread=ulong.Parse(f[3],CI),start_ns=long.Parse(f[4],CI),end_ns=long.Parse(f[5],CI),epoch_ms=long.Parse(f[6],CI),
      phase=f[7],identity=f[8],asset=f[9],wall_ms=double.Parse(f[10],CI),children_ms=double.Parse(f[11],CI),
      unattributed_ms=double.Parse(f[12],CI),thread_cpu_ms=double.Parse(f[13],CI),identity_truncated=int.Parse(f[14],CI)};
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
   }
   result.rows=spans.Count;
   result.largest_scopes=spans.Values.OrderByDescending(s=>s.wall_ms).Take(25).ToList();
   result.largest_unattributed_scopes=spans.Values.OrderByDescending(s=>s.unattributed_ms).Take(25).ToList();
   result.phase_totals=spans.Values.GroupBy(s=>s.phase).Select(g=>new HitchPhase {
    phase=g.Key,retained_calls=g.Count(),inclusive_ms=g.Sum(s=>s.wall_ms),self_ms=g.Sum(s=>s.unattributed_ms),
    max_ms=g.Max(s=>s.wall_ms),cpu_samples=g.Count(s=>s.thread_cpu_ms>=0),
    observed_thread_cpu_ms=g.Where(s=>s.thread_cpu_ms>=0).Sum(s=>s.thread_cpu_ms)
   }).OrderByDescending(p=>p.self_ms).ToList();
   return result;
  }
 }
}
'@
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
            if($status.capacity -notin @(131072,262144) -or $status.rows -gt $status.capacity -or $status.dropped -ne 0 -or
                $status.allocation_failed -ne 0 -or $status.active_at_finish -ne 0 -or
                $status.normal_finish -ne 1 -or $status.output_ok -ne 1){throw 'Timeline loss or incomplete output'}
            $analysis=[Phase9.HitchReport]::Read($path)
            if($mode -match '(?m)^phase9_detail_revision=2\r?$'){
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
    $result=[pscustomobject]@{schema=1;requested=$requested;valid_span_capture=$(if($requested){$reasons.Count -eq 0}else{$null});
        status=$status;analysis=$analysis;clock_mapping=$clock;reasons=$reasons.ToArray();
        scope='Nested CPU wall-time spans; selected GL calls are in the separate draw trace. GPU/driver internals remain partially unobserved.';
        identity_note='Truncated identities are explicitly marked; do not claim exact asset identity for those rows.';
        accounting_note='children_ms includes fast filtered child scopes. unattributed_ms is the remaining envelope, not proof of a specific cause. Nested scopes must not be added together.';
        phase_totals_note='Totals describe retained spans only. Inclusive totals overlap nested phases; self totals from different threads overlap in wall time. Windows thread CPU time is coarse and can exceed a short wall interval. Use ETW for precise running, ready and blocked intervals.'}
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $ProfileDir 'HITCH-CAPTURE.json') -Encoding UTF8
    if($analysis){
        $analysis.phase_totals | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-PHASE-TOTALS.csv')
        $analysis.largest_unattributed_scopes | Export-Csv -NoTypeInformation -Encoding UTF8 -LiteralPath (Join-Path $ProfileDir 'HITCH-REMAINING-SCOPES.csv')
    }
    if($requested -and $reasons.Count){Write-Warning ('INVALID HITCH CAPTURE: '+($reasons -join '; '))}
    return $result
}
