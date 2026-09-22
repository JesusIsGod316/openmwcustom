"""Streaming, bounded CP4F ownership/pressure report. Observation is not acceptance.

Never launches a process, trims a cache, follows an asset path, or reads a save.
Input may be incomplete after a native failure. Bytes from different accounting
layers are deliberately not summed into a fictitious process-memory total.
"""
import argparse
from collections import Counter, deque
import json
from pathlib import Path

MAX_KEYS = 2048
MAX_OPEN_WORK = 4096
MAX_LINE = 128 * 1024


def bounded_rows(path, counts):
    with Path(path).open(encoding='utf-8', errors='replace') as stream:
        while True:
            line = stream.readline(MAX_LINE + 1)
            if not line:
                return
            if len(line) > MAX_LINE:
                counts['oversized_rows'] += 1
                while line and not line.endswith('\n'):
                    line = stream.readline(MAX_LINE + 1)
                continue
            try:
                row = json.loads(line)
                if not isinstance(row, dict) or row.get('schema') != 2 or not isinstance(row.get('type'), str):
                    counts['unrecognized_rows'] += 1
                    continue
                yield row
            except (ValueError, TypeError):
                counts['malformed_rows'] += 1


def number(row, key):
    value = row.get(key)
    return value if isinstance(value, int) and not isinstance(value, bool) and value >= 0 else None


def analyze(path):
    counts = Counter()
    series, operations, work = {}, {}, {}
    findings, coverage = [], set()
    recent = deque(maxlen=256)
    examples = deque(maxlen=32)
    longest = []
    config = []
    latest_stats = {}
    ended = False
    schemas = ('os_memory', 'cache', 'cache_pool', 'prepared_cache', 'preload_cache',
               'neutral_memory', 'vulkan_heap', 'vsg_pool', 'resident_versions', 'retirement',
               'effect_rebuild_count', 'probe_cost')
    for row in bounded_rows(path, counts):
        counts['recognized_rows'] += 1
        kind = row['type']
        counts[kind] += 1
        if row.get('truncated'):
            counts['truncated_rows'] += 1
        owner = str(row.get('owner', ''))[:96]
        identity = str(row.get('identity', ''))[:256]
        if kind == 'coverage':
            if len(coverage) < MAX_KEYS:
                coverage.add(f'{owner}: {identity}')
            else:
                counts['summary_keys_limited'] += 1
        if kind == 'configuration':
            if len(config) < 32:
                config.append(row)
        if kind in ('recorder_stats', 'recorder_end'):
            latest_stats = row
            ended |= kind == 'recorder_end'
        if kind in schemas:
            # Instance addresses are capture-local identities, never cross-run IDs.
            discriminator = str(row.get('instance', row.get('heap', '')))
            key = (kind, owner, discriminator)
            if key not in series:
                if len(series) >= MAX_KEYS:
                    counts['summary_keys_limited'] += 1
                    continue
                series[key] = {'type': kind, 'owner': owner, 'instance_or_heap': discriminator,
                               'samples': 0, 'first_time_us': row.get('time_us'), 'last_time_us': None,
                               'first': {}, 'last': {}, 'peak': {}, 'minimum': {}}
            entry = series[key]
            entry['samples'] += 1
            entry['last_time_us'] = row.get('time_us')
            for field in row:
                value = number(row, field)
                if value is None or field in ('schema', 'frame', 'time_us', 'instance'):
                    continue
                # Unavailable API values are not zero-valued measurements.
                if kind == 'os_memory':
                    required = ('physical_valid' if field.startswith('physical_') else
                                'commit_valid' if field.startswith('system_commit_') else
                                'process_valid' if field in ('private_commit_bytes', 'working_set_bytes',
                                    'peak_working_set_bytes', 'page_fault_count') else None)
                    if required and row.get(required) != 1:
                        continue
                if kind == 'vulkan_heap' and field in ('budget_bytes', 'usage_bytes') and row.get('budget_available') != 1:
                    continue
                entry['first'].setdefault(field, value)
                entry['last'][field] = value
                entry['peak'][field] = max(value, entry['peak'].get(field, value))
                entry['minimum'][field] = min(value, entry['minimum'].get(field, value))
            if kind == 'os_memory':
                recent.append({k: v for k, v in row.items() if k in ('time_us', 'frame', 'working_set_bytes',
                    'private_commit_bytes', 'physical_available_bytes', 'physical_valid', 'process_valid')})
        if kind == 'work_begin':
            operation = number(row, 'operation')
            if operation is not None and len(operations) < MAX_OPEN_WORK:
                operations[operation] = (owner, identity)
            else:
                counts['work_tracking_limited'] += 1
        if kind == 'work_end':
            operation, elapsed = number(row, 'operation'), number(row, 'elapsed_us')
            if operations.pop(operation, None) is None:
                counts['work_without_begin'] += 1
            if elapsed is not None:
                if owner not in work and len(work) < MAX_KEYS:
                    work[owner] = {'count': 0, 'inclusive_total_us': 0, 'maximum_us': 0, 'unwinding': 0}
                if owner in work:
                    item = work[owner]
                    item['count'] += 1
                    item['inclusive_total_us'] += elapsed
                    item['maximum_us'] = max(item['maximum_us'], elapsed)
                    item['unwinding'] += row.get('unwinding') == 1
                longest.append({'owner': owner, 'identity': identity, 'elapsed_us': elapsed,
                                'unwinding': row.get('unwinding', 0), 'operation': operation})
                longest.sort(key=lambda x: x['elapsed_us'], reverse=True)
                del longest[16:]
        if kind in ('effect_rebuild', 'effect_texture', 'effect_material_delta', 'cache_asset', 'ui_missing_alias',
                    'preview', 'auxiliary', 'terrain_job', 'wait_dependency', 'ui_external_texture'):
            examples.append(row)
    if not counts.get('recognized_rows'):
        findings.append('CAPTURE FAILURE: no recognized runtime records.')
    if not ended:
        findings.append('INCOMPLETE: no recorder shutdown record; this may be a running or interrupted capture, not proof of a crash.')
    if counts['capture_limit'] or counts['malformed_rows'] or counts['oversized_rows'] or counts['truncated_rows']:
        findings.append('COVERAGE GAP: capped, malformed, oversized, or truncated records are present; missing events are not passes.')
    if latest_stats.get('dropped', 0):
        findings.append(f"COVERAGE GAP: recorder dropped {latest_stats['dropped']} events; rates and matched lifetimes may be incomplete.")
    if counts['summary_keys_limited'] or counts['work_tracking_limited']:
        findings.append('COVERAGE GAP: bounded analyzer identity/operation capacity was reached.')
    if not counts['os_memory']:
        findings.append('COVERAGE GAP: OS memory measurements unavailable.')
    if counts['ui_missing_alias']:
        findings.append('OBSERVED: MyGUI attempted to render an unresolved native texture alias; inspect the recorded alias/preview path.')
    if operations:
        findings.append(f'INCOMPLETE: {len(operations)} tracked work operations have no end record; do not sum them as completed work.')
    for entry in series.values():
        if entry['type'] == 'cache' and entry['peak'].get('limited'):
            findings.append(f"PARTIAL PAYLOAD CENSUS: {entry['owner']} exceeded the per-snapshot entry limit.")
        if entry['type'] == 'cache':
            lookups = entry['last'].get('lookups', 0) - entry['first'].get('lookups', 0)
            hits = entry['last'].get('hits', 0) - entry['first'].get('hits', 0)
            entry['interval_hit_ratio'] = hits / lookups if lookups > 0 and 0 <= hits <= lookups else None
        entry['delta'] = {k: v - entry['first'].get(k, v) for k, v in entry['last'].items()}
    for entry in series.values():
        if entry['type'] == 'os_memory' and 'private_commit_bytes' in entry['peak']:
            gib = 1024**3
            findings.insert(0, f"OS MEMORY: peak private commit {entry['peak']['private_commit_bytes']/gib:.3f} GiB; "
                f"peak sampled working set {entry['peak'].get('working_set_bytes', 0)/gib:.3f} GiB. These are different metrics.")
    largest = sorted((e for e in series.values() if e['type']=='cache' and e['peak'].get('known_payload_bytes',0)),
                     key=lambda e: e['peak']['known_payload_bytes'], reverse=True)[:5]
    for entry in largest:
        findings.append(f"CACHE PAYLOAD: {entry['owner']} peaked at {entry['peak']['known_payload_bytes']/1024**2:.1f} MiB of measured payload "
                        f"(first-to-last delta {entry['delta'].get('known_payload_bytes',0)/1024**2:+.1f} MiB); object/allocator overhead is excluded.")
    findings += [
        'INTERPRETATION: rising memory alone is not a leak. Compare first visit, revisit, departure, and settled intervals in fresh-process controls.',
        'ACCOUNTING: payload capacities, Windows private commit/working set, Vulkan heap estimates, and pool reservations overlap or measure different layers. Do not add them.',
        'COVERAGE: object/allocator/driver internals, exact saved work per cache hit, complete allocation lifetime stacks, GPU timestamps, and pixel parity are not measured by this recorder.',
        'RETIREMENT: snapshots precede normal completion collection. Completed roots pending collection can be normal; retained writable pool slots are reusable, not automatically leaked.',
        'CACHES: external-reference classification is a refcount observation (including other cache aliases), not proof of active gameplay use; NIF bytes cover selected geometry arrays only.'
    ]
    return {'schema': 1, 'findings': findings, 'counts': dict(counts), 'configuration': config,
            'series': list(series.values()), 'work': work, 'longest_completed_work': longest,
            'recent_os_samples': list(recent), 'bounded_examples': list(examples),
            'last_recorder_stats': latest_stats, 'coverage': sorted(coverage),
            'unfinished_work': [{'operation': k, 'owner': v[0], 'identity': v[1]} for k, v in list(operations.items())[:32]]}


def report(directory):
    root = Path(directory)
    trace = root / 'runtime.jsonl'
    if trace.exists():
        result = analyze(trace)
    else:
        result = {'schema': 1, 'findings': ['CAPTURE FAILURE: runtime.jsonl is missing (or diagnostics were disabled).'],
                  'series': [], 'configuration': [], 'longest_completed_work': [], 'bounded_examples': [],
                  'last_recorder_stats': {}, 'coverage': []}
    manifest_path = root / 'manifest.json'
    if manifest_path.exists():
        try:
            manifest = json.loads(manifest_path.read_text(encoding='utf-8'))
            result['run_identity'] = {k: manifest.get(k) for k in ('source_head', 'sha256', 'diagnostics', 'state', 'exit_code', 'isolated_user_data')}
            if manifest.get('exit_code') not in (None, 0):
                result['findings'].insert(0, f"RUN FAILURE: process exit {manifest['exit_code']}.")
        except (ValueError, OSError):
            result['findings'].append('COVERAGE GAP: manifest unreadable.')
    (root / 'memory-report.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    lines = ['# CP4F runtime ownership and memory report', '', 'Diagnostic observation, not runtime or performance acceptance.', '',
             *result['findings'], '', '## Effective cache policy', '',
             'Merged settings and effective profile are recorded below. No cache limits or lifetimes are changed by the observer.',
             '```json', json.dumps(result['configuration'], indent=2), '```', '',
             '## Memory and cache summaries', '',
             'All size fields use bytes. First/last/peak are observed samples; they are not simultaneous across owners.', '']
    for entry in result['series']:
        lines += [f"### {entry['type']} / {entry['owner']} / {entry['instance_or_heap']}",
                  f"Samples: {entry['samples']}", '```json',
                  json.dumps({k: entry[k] for k in ('first', 'last', 'peak', 'minimum', 'delta')}, indent=2), '```']
    lines += ['', '## Longest observed completed work', '', 'Inclusive wall time, not GPU time; nested work overlaps.', '```json',
              json.dumps(result['longest_completed_work'], indent=2), '```', '', '## Bounded asset and transition examples',
              '```json', json.dumps(result['bounded_examples'], indent=2), '```', '', '## Recorder overhead and dropped events',
              'Producer and writer wall times include different work; they are not additional frame times and must not be summed.',
              'Census costs are separate probe_cost records. Writer time excludes idle sleeps and the final footer/flush.',
              '```json', json.dumps(result['last_recorder_stats'], indent=2), '```', '', '## Explicit coverage notes', '', *result['coverage']]
    (root / 'memory-report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', help='Evidence directory containing runtime.jsonl and manifest.json')
    args = parser.parse_args()
    print('\n'.join(report(args.directory)['findings']))
