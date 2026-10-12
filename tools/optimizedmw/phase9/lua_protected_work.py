"""Protected-call work within recorded route intervals, not VM cost attribution."""
import bisect
import csv
import math
from pathlib import Path
import re


TOTALS = ('reused_total', 'created_total', 'ordinary_total')
WORKER_FIELDS = ('ordinary_calls', 'reused_calls')


def interval_work(profile, windows):
    root = Path(profile)
    path = root/'p9-lua-protected-work.csv'
    status = dict(line.split('=', 1) for line in
                  Path(str(path)+'.writer-status.txt').read_text(encoding='utf-8-sig').splitlines()
                  if '=' in line)
    for key, expected in (('normal_finish', '1'), ('output_ok', '1'),
                          ('rows_dropped', '0'), ('initialization_failed', '0')):
        if status.get(key) != expected:
            raise ValueError('Incomplete Lua protected-call transport: '+key)
    with path.open(newline='', encoding='utf-8-sig') as stream:
        reader = csv.DictReader(line for line in stream if not line.startswith('#'))
        if not set(('frame', 'epoch_ms', 'row_seq', 'enabled', *TOTALS)).issubset(reader.fieldnames or ()):
            raise ValueError('Unknown Lua protected-call work header')
        worker_fields = set(WORKER_FIELDS).intersection(reader.fieldnames or ())
        if worker_fields and worker_fields != set(WORKER_FIELDS):
            raise ValueError('Incomplete Lua protected-call worker header')
        worker_available = bool(worker_fields)
        rows = list(reader)
    if not rows:
        raise ValueError('No Lua protected-call work')
    previous = None
    for seq, row in enumerate(rows, 1):
        frame = int(row['frame']); totals = tuple(int(row[key]) for key in TOTALS)
        if int(row['row_seq']) != seq or int(row['enabled']) not in (0, 1):
            raise ValueError('Missing or invalid Lua protected-call work sequence')
        if not math.isfinite(float(row['epoch_ms'])) or any(value < 0 for value in totals):
            raise ValueError('Invalid Lua protected-call observation')
        if worker_available:
            worker = tuple(int(row[key]) for key in WORKER_FIELDS)
            if any(value < 0 for value in worker):
                raise ValueError('Invalid Lua protected-call worker observation')
            for key, value in zip(WORKER_FIELDS, worker):
                if value > int(row['ordinary_total' if key == 'ordinary_calls' else 'reused_total']):
                    raise ValueError('Negative Lua protected-call inter-update decomposition')
        if previous and (frame < previous[0] or any(value < before for value, before in zip(totals, previous[1]))):
            raise ValueError('Reset Lua protected-call work sequence')
        if previous and worker_available:
            # Each row's deltas cover that worker update. Cumulative deltas
            # additionally cover manager-VM jobs between consecutive updates.
            # Never turn an inconsistent residual into zero or infer work.
            for key, total_index in (('ordinary_calls', 2), ('reused_calls', 0)):
                if int(row[key]) > totals[total_index] - previous[1][total_index]:
                    raise ValueError('Negative Lua protected-call inter-update decomposition')
        previous = frame, totals
    log = (root/'openmw.log').read_text(encoding='utf-8-sig')
    footers = [line for line in log.splitlines()
               if 'OptimizedMW protected work capture:' in line]
    if len(footers) != 1:
        raise ValueError('Missing or ambiguous Lua protected-call producer footer')
    tokens = re.findall(r'\bproducer_rows=([^\s]+)', footers[0])
    if len(tokens) != 1 or not re.fullmatch(r'[0-9a-fA-F]+', tokens[0]):
        raise ValueError('Invalid Lua protected-call producer row token')
    token = tokens[0]
    bases = [base for base in (10, 16) if (base == 16 or token.isdecimal())
             and int(token, base) == len(rows)]
    if not bases:
        raise ValueError('Missing or truncated Lua protected-call producer rows')
    footer = {'token': token, 'independent_csv_rows': len(rows), 'parsed_rows': len(rows),
              'base': bases[0] if len(bases) == 1 else None,
              'encoding': ('decimal' if bases[0] == 10 else 'unprefixed_hex')
              if len(bases) == 1 else 'decimal_or_hex_equivalent', 'unique_footer': True}
    epochs = {}
    with (root/'v3-frame.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame = int(row['frame']); epoch = float(row['epoch_ms'])
            if frame in epochs or not math.isfinite(epoch):
                raise ValueError('Invalid Lua main-frame clock')
            epochs[frame] = epoch
    contexts = {}
    with (root/'benchmark-context.csv').open(newline='', encoding='utf-8-sig') as stream:
        for row in csv.DictReader(stream):
            frame = int(row['frame'])
            if frame in contexts:
                raise ValueError('Repeated Lua context frame')
            contexts[frame] = all(int(row[key]) == 0 for key in ('begin_flags', 'end_flags', 'observed_flags'))
    frames = sorted(epochs)
    if any(epochs[right] < epochs[left] for left, right in zip(frames, frames[1:])):
        raise ValueError('Reversed Lua main-frame clock')
    route_clock = {}
    for key in ('start', 'finish', 'movement'):
        if key == 'movement' and windows.get(key) is None:
            continue
        route_clock[key] = float(windows[key])
        if not math.isfinite(route_clock[key]):
            raise ValueError('Invalid Lua route clock')
    if route_clock['finish'] <= route_clock['start']:
        raise ValueError('Invalid Lua route clock order')
    if 'movement' in route_clock and not route_clock['start'] <= route_clock['movement'] <= route_clock['finish']:
        raise ValueError('Invalid Lua movement clock order')
    bad_prefix = [0]
    for frame in frames:
        bad_prefix.append(bad_prefix[-1]+(not contexts.get(frame, False)))

    def phase(frame):
        epoch = epochs.get(frame)
        if epoch is None or not route_clock['start']+1000 <= epoch < route_clock['finish']-1000:
            return None
        if not contexts.get(frame, False):
            return None
        return 'fixed_view' if route_clock.get('movement') and epoch < route_clock['movement']-1000 else 'moving'

    phases = {name: {'counts': dict.fromkeys(TOTALS, 0), 'observation_pairs': 0,
                     'maximum_observation_gap_ms': 0.0, 'accepted_observation_ms': 0.0,
                     'attempted_calls': 0, 'worker_attempted_calls': 0 if worker_available else None,
                     'inter_update_attempted_calls': 0 if worker_available else None,
                     'worker_ordinary_calls': 0 if worker_available else None,
                     'worker_reused_calls': 0 if worker_available else None,
                     'inter_update_ordinary_calls': 0 if worker_available else None,
                     'inter_update_reused_calls': 0 if worker_available else None}
              for name in ('fixed_view', 'moving')}
    excluded = 0
    for before, row in zip(rows, rows[1:]):
        first, frame = int(before['frame']), int(row['frame'])
        name = phase(frame)
        if name is None or phase(first) != name or first == frame:
            excluded += 1; continue
        lo = bisect.bisect_right(frames, first); hi = bisect.bisect_left(frames, frame)
        if bad_prefix[lo] != bad_prefix[hi]:
            excluded += 1; continue
        gap = epochs[frame]-epochs[first]
        # Integer-millisecond logging can tie at high frame rates. Those pairs
        # have no measurable elapsed exposure, so do not invent a denominator.
        if gap <= 0:
            excluded += 1; continue
        entry = phases[name]
        entry['observation_pairs'] += 1
        entry['maximum_observation_gap_ms'] = max(entry['maximum_observation_gap_ms'], gap)
        entry['accepted_observation_ms'] += gap
        for key in TOTALS:
            entry['counts'][key] += int(row[key])-int(before[key])
        ordinary = int(row['ordinary_total'])-int(before['ordinary_total'])
        reused = int(row['reused_total'])-int(before['reused_total'])
        entry['attempted_calls'] += ordinary + reused
        if worker_available:
            worker_ordinary, worker_reused = (int(row[key]) for key in WORKER_FIELDS)
            entry['worker_ordinary_calls'] += worker_ordinary
            entry['worker_reused_calls'] += worker_reused
            entry['inter_update_ordinary_calls'] += ordinary - worker_ordinary
            entry['inter_update_reused_calls'] += reused - worker_reused
            entry['worker_attempted_calls'] += worker_ordinary + worker_reused
            entry['inter_update_attempted_calls'] += ordinary + reused - worker_ordinary - worker_reused
    for entry in phases.values():
        seconds = entry['accepted_observation_ms']/1000
        entry['attempted_per_observed_second'] = entry['attempted_calls']/seconds if seconds else None
        for prefix in ('worker', 'inter_update'):
            count = entry[prefix+'_attempted_calls']
            entry[prefix+'_attempted_per_observed_second'] = count/seconds if seconds and count is not None else None
        entry['worker_attempted_per_observation'] = (entry['worker_attempted_calls']/entry['observation_pairs']
            if worker_available and entry['observation_pairs'] else None)
    return {'schema': 1, 'rows': len(rows), 'enabled': int(rows[-1]['enabled']) == 1,
            'producer_footer': footer,
            'worker_decomposition_available': worker_available,
            'excluded_pairs': excluded, 'phases': phases,
            'scope': 'Completed Lua-manager work observed at approximate route intervals',
            'attempt_semantics': 'Counts are attempted native protected jobs in the manager VM, not successful Lua callbacks, equally costly work, or all Lua states. Worker fields cover the current accepted update; inter-update fields are the remaining cumulative delta. Rates use only accepted adjacent main-frame clock gaps, not full phase duration. Legacy headers without worker fields retain total counts but cannot decompose them.',
            'limit': 'Producer decimal/hex encoding is constrained to independently counted contiguous CSV rows, not guessed. Loading/UI intervals excluded here but preserved in raw evidence. Same-frame or tied-clock observations and pairs across phase/context boundaries are excluded. Accepted time is approximate main-frame elapsed exposure, not thread CPU or exact worker execution time. Reused closure counts do not measure saved milliseconds or identify VM/GC cost.'}
