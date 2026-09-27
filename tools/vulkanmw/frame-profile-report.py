"""Analyze bounded frame profiles; repeated scopes are summed per frame first.

CPU exclusive totals are additive on the frame thread. Worker graph wall times
and GPU intervals are separate overlapping timelines and must NOT be added to
those totals. GPU intervals include stalls inside their command-buffer span.
"""
import argparse
import hashlib
from collections import defaultdict
import json
from pathlib import Path
import statistics


def distribution(values):
    ordered = sorted(values)
    return {'mean': statistics.mean(ordered), 'median': statistics.median(ordered),
            'p95': ordered[max(0, (len(ordered) * 95 + 99) // 100 - 1)], 'samples': len(ordered)}


def summarize(rows):
    frames = [r for r in rows if r.get('type') == 'frame_end' and str(r.get('completed')) == '1']
    if not frames:
        raise ValueError('No completed sampled frames')
    cutoff = max(r.get('time_us', 0) for r in frames) - 30_000_000
    steady = {r['frame'] for r in frames if r.get('time_us', 0) >= cutoff}
    totals, stages, graphs, gpu = defaultdict(dict), defaultdict(dict), defaultdict(dict), defaultdict(list)
    problems = []
    unhealthy = set()
    for row in rows:
        kind, frame = row.get('type'), row.get('frame')
        if kind == 'profile_gpu' and int(row['source_frame']) in steady:
            gpu[row['name']].append(float(row['ms']))
        if kind in ('profile_gpu_error', 'profile_gpu_dropped', 'profile_gpu_unavailable'):
            problems.append(row)
        if frame not in steady:
            continue
        if kind == 'profile_total':
            totals[frame][row['name']] = {k: float(row[k]) for k in ('inclusive_ms', 'exclusive_ms', 'max_ms', 'calls')}
        elif kind == 'stage_end':
            target = stages[frame]
            target[row['name']] = target.get(row['name'], 0) + float(row['ms'])
        elif kind == 'profile_record_graph':
            graphs[frame][row['order']] = float(row['wall_ms'])
        elif kind == 'profile_health' and (int(row['dropped']) or int(row['unclosed'])):
            problems.append(row)
            unhealthy.add(frame)
    rejected = []
    for frame, scopes in totals.items():
        if frame in unhealthy:
            rejected.append(frame)
            continue
        accounted = sum(v['exclusive_ms'] for v in scopes.values())
        if 'frame' not in scopes:
            problems.append({'frame': frame, 'missing_root': True})
            rejected.append(frame)
            continue
        expected = scopes['frame']['inclusive_ms']
        if abs(accounted - expected) > 0.002:
            problems.append({'frame': frame, 'accounting_residual_ms': accounted - expected})
            rejected.append(frame)
    for frame in rejected:
        del totals[frame]
    def group(data, field=None):
        names = {name for scopes in data.values() for name in scopes}
        result = {}
        for name in names:
            values = [(scopes.get(name, {}).get(field, 0) if field else scopes.get(name, 0))
                      for scopes in data.values()]
            result[name] = distribution(values)
        return dict(sorted(result.items(), key=lambda item: item[1]['mean'], reverse=True))
    return {'note': __doc__, 'steady_sampled_frames': len(steady), 'profiled_frames': len(totals),
            'health_issues': problems,
            'cpu_exclusive_ms': group(totals, 'exclusive_ms'),
            'cpu_inclusive_ms': group(totals, 'inclusive_ms'),
            'calls_per_frame': group(totals, 'calls'),
            'stages_summed_per_frame_ms': group(stages),
            'record_graph_wall_ms_by_submit_order': group(graphs),
            'gpu_interval_ms': {k: distribution(v) for k, v in gpu.items()}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('evidence', type=Path)
    args = parser.parse_args()
    rows = [json.loads(line) for line in (args.evidence / 'gameplay.jsonl').read_text(encoding='utf-8').splitlines()]
    report = summarize(rows)
    report['analyzer_sha256'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    manifest = json.loads((args.evidence / 'manifest.json').read_text(encoding='utf-8'))
    report['executable_sha256'] = manifest['sha256']
    report['wall_clock_result'] = manifest.get('automated_result')
    report['original_chain_unchanged'] = manifest.get('original_chain_unchanged')
    target = args.evidence / 'frame-profile-summary.json'
    target.write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(target)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
