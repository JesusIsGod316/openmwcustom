"""Process-isolated differential production fixtures for independent I1/I2 and fallback cells."""
import csv
import io
import os
from pathlib import Path
import subprocess
import sys

exe, prefix = sys.argv[1:]
cells = (
    ('CONTROL-unarmed', '8192', '0', '33554432', False),
    ('CONTROL', '8192', '0', '33554432', True),
    ('I1', '65536', '0', '33554432', True),
    ('I2', '8192', '1', '33554432', True),
    ('I1I2', '65536', '1', '33554432', True),
    ('zero-cap', '65536', '1', '0', True),
    ('state-only-cap', '65536', '1', '400', True),
    ('buffer-below-cap', '65536', '0', '65536', True),
    ('invalid-options', '32768', 'yes', '999999999999999999999999', True),
)


def metrics(row):
    return {row[f'metric{i}_name']: int(row[f'metric{i}_value'])
            for i in range(1, 5) if row[f'metric{i}_name']}


for name, refill, resident, cap, armed in cells:
    stem = Path(prefix + '-' + name)
    stem.parent.mkdir(parents=True, exist_ok=True)
    output = Path(str(stem) + '.csv')
    for suffix in ('', '.clock-map.txt', '.capture-status.txt'):
        Path(str(output) + suffix).unlink(missing_ok=True)
    env = dict(os.environ)
    env.update(OPENMW_P9_FILE_REFILL_BYTES=refill, OPENMW_P9_FILE_RESIDENT_SEEK=resident,
               OPENMW_P9_FILE_BUFFER_BUDGET_BYTES=cap,
               OPENMW_P9_HITCH_SPANS_FILE=str(output) if armed else '0')
    subprocess.run([exe, str(stem)], env=env, check=True, timeout=90)
    if not armed:
        assert not output.exists(), 'CONTROL unexpectedly recorded a trace'
        continue
    status = dict(line.split('=', 1) for line in Path(str(output) + '.capture-status.txt').read_text().splitlines())
    assert status['normal_finish'] == status['output_ok'] == '1'
    assert status['dropped'] == status['allocation_failed'] == status['active_at_finish'] == '0'
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in output.read_text().splitlines() if not line.startswith('#')))))
    assert len(rows) == int(status['rows'])
    opens = {row['span']: row for row in rows if row['phase'] == 'file.open_size' and metrics(row)['opened'] == 1}
    assert len({row['span'] for row in rows}) == len(rows)
    phases = ('file.stream_totals', 'file.stream_policy', 'file.stream_buffer',
              'file.stream_io', 'file.stream_consumption')
    grouped = {phase: {} for phase in phases}
    budgets = []
    for row in rows:
        if row['phase'] in grouped:
            assert row['link'] in opens
            assert row['asset'] == opens[row['link']]['asset']
            assert row['link'] not in grouped[row['phase']], 'duplicate open-linked row'
            grouped[row['phase']][row['link']] = metrics(row)
        elif row['phase'] == 'file.stream_budget':
            budgets.append(metrics(row))
    for phase in phases:
        assert grouped[phase].keys() == opens.keys(), f'missing complete per-open {phase}'
    requested_refill = 65536 if refill == '65536' else 8192
    requested_resident = int(resident == '1')
    limit = int(cap) if cap.isdigit() and int(cap) <= 33554432 else 33554432
    for span in opens:
        policy = grouped['file.stream_policy'][span]
        buffer = grouped['file.stream_buffer'][span]
        total = grouped['file.stream_totals'][span]
        native = grouped['file.stream_io'][span]
        assert policy['requested_refill_bytes'] == requested_refill
        assert policy['requested_resident_seek'] == requested_resident
        assert policy['effective_refill_bytes'] in (8192, requested_refill)
        assert policy['effective_resident_seek'] <= requested_resident
        assert buffer['budget_limit_bytes'] == limit
        assert buffer['owned_candidate_bytes'] <= limit
        if policy['effective_refill_bytes'] == 65536:
            assert buffer['owned_candidate_bytes'] >= 65536 + 64 + 128
        assert set(total) == {'read_calls', 'returned_bytes', 'read_call_ns', 'seek_calls'}
        initial_seek = int(metrics(opens[span])['region_origin'] != 0)
        assert native['native_seek_calls'] >= total['seek_calls'] + initial_seek
        if not (requested_refill == 65536 or requested_resident):
            assert buffer['owned_candidate_bytes'] == buffer['buffer_cap_hits'] == buffer['buffer_allocation_failures'] == 0
        if limit == 0:
            assert policy['effective_refill_bytes'] == 8192 and policy['effective_resident_seek'] == 0
            assert buffer['owned_candidate_bytes'] == 0
    if not (requested_refill == 65536 or requested_resident):
        assert not budgets, 'CONTROL touched candidate budget reporting'
    else:
        assert len(budgets) == len(opens)
        for budget in budgets:
            assert budget['budget_in_use_bytes'] <= limit and budget['budget_peak_bytes'] <= limit
        assert budgets[-1]['budget_in_use_bytes'] == budgets[-1]['active_candidate_streams'] == budgets[-1]['active_candidate_buffers'] == 0
    sequential = next(span for span, row in opens.items() if row['asset'].endswith('-sequential.bin'))
    effective = grouped['file.stream_policy'][sequential]['effective_refill_bytes']
    total = grouped['file.stream_totals'][sequential]
    assert total['returned_bytes'] == 1048579
    assert total['read_calls'] == (1048579 + effective - 1) // effective + 1
    assert grouped['file.stream_consumption'][sequential]['net_consumed_bytes'] == 1048579
    reuse = next(span for span, row in opens.items() if row['asset'].endswith('-reuse.bin'))
    retained = grouped['file.stream_policy'][reuse]['effective_resident_seek']
    assert grouped['file.stream_totals'][reuse]['returned_bytes'] == (4097 if retained else 8194)
    if retained:
        assert grouped['file.stream_io'][reuse]['native_seek_calls'] == 1
        assert grouped['file.stream_io'][reuse]['native_tell_calls'] == 0
    print(f'PASS {name}: {len(opens)} closed streams, full diagnostics, sequential reads={total["read_calls"]}, refill={effective}')
print('PASS all I1/I2/OFF/cap/failure/invalid-option production stream cells')
