"""Protected-call work within recorded route intervals, not VM cost attribution."""
import bisect
import csv
import math
from pathlib import Path
import re


TOTALS = ('reused_total', 'created_total', 'ordinary_total')


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
        if previous and (frame < previous[0] or any(value < before for value, before in zip(totals, previous[1]))):
            raise ValueError('Reset Lua protected-call work sequence')
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
    bad_prefix = [0]
    for frame in frames:
        bad_prefix.append(bad_prefix[-1]+(not contexts.get(frame, False)))

    def phase(frame):
        epoch = epochs.get(frame)
        if epoch is None or not windows['start']+1000 <= epoch < windows['finish']-1000:
            return None
        if not contexts.get(frame, False):
            return None
        return 'fixed_view' if windows.get('movement') and epoch < windows['movement']-1000 else 'moving'

    phases = {name: {'counts': dict.fromkeys(TOTALS, 0), 'observation_pairs': 0,
                     'maximum_observation_gap_ms': 0.0} for name in ('fixed_view', 'moving')}
    excluded = 0
    for before, row in zip(rows, rows[1:]):
        first, frame = int(before['frame']), int(row['frame'])
        name = phase(frame)
        if name is None or phase(first) != name or first == frame:
            excluded += 1; continue
        lo = bisect.bisect_right(frames, first); hi = bisect.bisect_left(frames, frame)
        if bad_prefix[lo] != bad_prefix[hi]:
            excluded += 1; continue
        entry = phases[name]
        entry['observation_pairs'] += 1
        entry['maximum_observation_gap_ms'] = max(entry['maximum_observation_gap_ms'], epochs[frame]-epochs[first])
        for key in TOTALS:
            entry['counts'][key] += int(row[key])-int(before[key])
    return {'schema': 1, 'rows': len(rows), 'enabled': int(rows[-1]['enabled']) == 1,
            'producer_footer': footer,
            'excluded_pairs': excluded, 'phases': phases,
            'scope': 'Completed Lua-manager work observed at approximate route intervals',
            'limit': 'Producer decimal/hex encoding is constrained to independently counted contiguous CSV rows, not guessed. Loading/UI intervals excluded here but preserved in raw evidence. Reused closure counts do not measure saved milliseconds or identify VM/GC cost.'}
