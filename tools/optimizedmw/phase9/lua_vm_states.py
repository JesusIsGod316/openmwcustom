"""Qualify delivered LuaJIT VM-state counts without inventing exact GC times.

The profiler timer records a VM state before its callback is delivered. CSV frame
labels are delivery observations, not exact originating frame or callback IDs.
"""
import csv
import math
from pathlib import Path
import re

STATES = ('native', 'interpreted', 'c', 'gc', 'jit', 'unknown')
HEADER = ('frame', 'epoch_ms', 'sampler_attempted', 'callback_deliveries', *STATES,
          'row_seq', 'callbacks_total', *(key + '_total' for key in STATES))
LIMITS = (
    'VM states were recorded when the profiling timer triggered; delivery may occur in a later scope.',
    'Frame labels describe delivery observations; CPU span ownership remains the authority for exact frames.',
    'State sample counts are statistical distributions, not exact elapsed milliseconds or per-spike attribution.',
    'Nested callback sample scopes overlap and must not be added.',
    'One process-global sampler has no public acceptance or external ownership query; observed callbacks qualify activity.',
    'This explicit Deep sampler changes hook dispatch and Windows timer resolution; stopping flushes JIT traces.',
    'No sampled GC state does not exclude an unsampled short collection pause.',
)


def _integer(value, label):
    if value is None or not re.fullmatch(r'\d+', value):
        raise ValueError('Invalid nonnegative VM-state integer: ' + label)
    return int(value)


def _fields(line):
    return dict(re.findall(r'([a-z_]+)=(\d+)', line))


def qualify_vm_states(profile, windows=None):
    """Return qualified activity and delivery distributions, optionally by route.

    `windows` uses the usual benchmark start/movement/finish epoch-ms boundaries.
    No phase distribution is called exact per-spike attribution or performance.
    """
    profile = Path(profile)
    path = profile / 'p9-lua-vm-states.csv'
    status_path = Path(str(path) + '.writer-status.txt')
    status = dict(line.split('=', 1) for line in status_path.read_text(encoding='utf-8-sig').splitlines()
                  if '=' in line)
    for key, expected in (('normal_finish', '1'), ('output_ok', '1'), ('rows_dropped', '0'),
                          ('initialization_failed', '0')):
        if status.get(key) != expected:
            raise ValueError('Incomplete VM-state transport: ' + key)
    for key in ('capacity_dropped', 'contention_dropped', 'oversized_dropped',
                'allocation_dropped', 'io_dropped', 'stopped_dropped'):
        if status.get(key) != '0':
            raise ValueError('Incomplete VM-state transport: ' + key)

    log = (profile / 'openmw.log').read_text(encoding='utf-8-sig', errors='replace')
    starts = [line for line in log.splitlines()
              if 'OptimizedMW Lua VM sampling: requested=1 api_available=1 ' in line]
    finals = [line for line in log.splitlines()
              if 'OptimizedMW Lua VM sampling: final_callbacks=' in line]
    producers = [line for line in log.splitlines()
                 if 'OptimizedMW VM work capture: producer_rows=' in line]
    if len(starts) != 1 or len(finals) != 1 or len(producers) != 1:
        raise ValueError('Missing or ambiguous VM-state owner/completion metadata')
    start, final, produced = map(_fields, (starts[0], finals[0], producers[0]))
    for metadata in (start, final):
        if metadata.get('delivery_is_not_sample_time') != '1' or metadata.get('stop_flushes_jit') != '1':
            raise ValueError('Missing VM-state timing/observer caveat metadata')
    if start.get('interval_ms') != '1' or start.get('acceptance_requires_callbacks') != '1':
        raise ValueError('Unqualified VM-state sampling mode')
    final_callbacks = _integer(final.get('final_callbacks'), 'final_callbacks')
    final_states = {key: _integer(final.get(key), key) for key in STATES}
    producer_rows = _integer(produced.get('producer_rows'), 'producer_rows')
    if _integer(produced.get('callback_deliveries'), 'callback_deliveries') != final_callbacks:
        raise ValueError('Inconsistent VM-state final callback totals')
    if final_callbacks == 0 or sum(final_states.values()) == 0:
        raise ValueError('No observed VM-state callbacks; attempted start is not acceptance')

    text = path.read_text(encoding='utf-8-sig')
    if '# p9_capture_transport=bounded_mpsc_v1' not in text:
        raise ValueError('Missing bounded VM-state transport footer')
    reader = csv.DictReader(line for line in text.splitlines() if line and not line.startswith('#'))
    if tuple(reader.fieldnames or ()) != HEADER:
        raise ValueError('Unknown VM-state CSV header')
    rows = []
    previous = None
    for row in reader:
        if None in row or any(value is None for value in row.values()):
            raise ValueError('Truncated or extra VM-state CSV fields')
        values = {key: _integer(row[key], key) for key in HEADER if key != 'epoch_ms'}
        epoch = float(row['epoch_ms'])
        if not math.isfinite(epoch):
            raise ValueError('Non-finite VM-state delivery clock')
        values['epoch_ms'] = epoch
        if values['sampler_attempted'] != 1 or values['row_seq'] != len(rows) + 1:
            raise ValueError('Inactive sampler or missing/repeated VM-state row sequence')
        for delta, total in [('callback_deliveries', 'callbacks_total'),
                             *((key, key + '_total') for key in STATES)]:
            difference = values[total] - (previous[total] if previous else 0)
            if difference < 0 or values[delta] > difference:
                raise ValueError('Reset or inconsistent VM-state counters: ' + total)
        if previous and (values['frame'] <= previous['frame'] or epoch < previous['epoch_ms']):
            raise ValueError('Repeated/reversed VM-state delivery observations')
        rows.append(values)
        previous = values
    if not rows or len(rows) != producer_rows:
        raise ValueError('Missing or tail-truncated VM-state rows')
    if rows[-1]['callbacks_total'] > final_callbacks or any(
            rows[-1][key + '_total'] > final_states[key] for key in STATES):
        raise ValueError('VM-state CSV totals exceed final shutdown summary')

    deliveries = sum(row['callback_deliveries'] for row in rows)
    observed = {key: sum(row[key] for row in rows) for key in STATES}
    if deliveries == 0 or sum(observed.values()) == 0:
        raise ValueError('No VM-state callbacks delivered during observed Lua updates')
    result = {
        'schema': 1, 'qualified': True,
        'scope': 'statistical_vm_states_delivered_during_lua_updates',
        'exact_per_spike_attribution': False, 'exact_gc_elapsed_time': False,
        'limits': list(LIMITS), 'rows': len(rows), 'producer_rows': producer_rows,
        'callback_deliveries': deliveries, 'delivered_states': observed,
        'delivered_sample_percent': {key: value * 100 / sum(observed.values())
                                     for key, value in observed.items()},
        'whole_vm_callbacks': final_callbacks, 'whole_vm_states': final_states,
        'outside_update_callbacks': final_callbacks - deliveries,
        'outside_update_states': {key: final_states[key] - observed[key] for key in STATES},
    }
    if windows is not None:
        start_ms, finish_ms = float(windows['start']), float(windows['finish'])
        movement_ms = float(windows['movement']) if windows.get('movement') is not None else None
        if not math.isfinite(start_ms) or not math.isfinite(finish_ms) or finish_ms <= start_ms:
            raise ValueError('Invalid VM-state route window')
        if movement_ms is not None and (not math.isfinite(movement_ms) or not start_ms <= movement_ms <= finish_ms):
            raise ValueError('Invalid VM-state movement window')
        phases = {key: {'rows': 0, 'callback_deliveries': 0, 'delivered_states': dict.fromkeys(STATES, 0)}
                  for key in ('fixed_view', 'moving', 'outside_route')}
        for row in rows:
            epoch = row['epoch_ms']
            phase = 'outside_route' if not start_ms + 1000 <= epoch < finish_ms - 1000 else (
                'fixed_view' if movement_ms is not None and epoch < movement_ms - 1000 else 'moving')
            entry = phases[phase]
            entry['rows'] += 1
            entry['callback_deliveries'] += row['callback_deliveries']
            for key in STATES:
                entry['delivered_states'][key] += row[key]
        result['delivery_phase_distributions'] = phases
        result['phase_limit'] = 'Includes loading and callback delivery delay; phase labels are not exact sampled-work origins.'
    return result
