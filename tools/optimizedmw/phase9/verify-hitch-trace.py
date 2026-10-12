import csv
import io
import os
from pathlib import Path
import subprocess
import sys

exe, output = sys.argv[1:]
unarmed = Path(output + '-unarmed.csv')
for p in (unarmed, *(Path(str(unarmed)+suffix) for suffix in
                    ('.clock-map.txt','.capture-status.txt','.observer.csv','.observer-status.txt'))):
    if p.exists(): p.unlink()
subprocess.run([exe, str(unarmed), 'unarmed'], check=True)
assert not unarmed.exists() and not Path(str(unarmed)+'.clock-map.txt').exists()
assert not Path(str(unarmed)+'.observer.csv').exists()

def read_status(path):
    result = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        key, value = line.split('=', 1)
        assert key not in result, ('duplicate status', key)
        result[key] = value
    return result

def transport_accounting(status):
    assert status['transport_schema'] == '1'
    count = lambda key: int(status[key])
    producer_loss = sum(count(k) for k in ('capacity_dropped','contention_dropped','allocation_dropped','stopped_dropped'))
    assert count('attempted') == count('accepted') + producer_loss
    assert count('accepted') == count('written') + count('pending') + count('io_dropped')
    assert count('dropped') == producer_loss + count('io_dropped')
    assert count('rows') == count('written')
    assert count('queue_high_water') <= count('capacity')
    if status['transport'] == 'stream_mpsc_v1':
        assert count('queue_capacity') == count('capacity')
    else:
        assert status['transport'] == 'deferred_arena_v1'
        assert count('queue_capacity') == 0
        assert count('rows') <= count('capacity')
    assert count('row_size_bytes') > 0
    assert count('writer_started_ns') <= count('writer_finished_ns')
    assert count('writer_cpu_available') in (0,1)
    for key in ('writer_format_ns','writer_write_ns','writer_serialized_bytes','writer_batches'):
        assert count(key) >= 0

for mode in ('normal', 'overflow', 'stream'):
    path = Path(output + '-' + mode + '.csv')
    subprocess.run([exe, str(path), mode], check=True)
    status = read_status(Path(str(path) + '.capture-status.txt'))
    transport_accounting(status)
    assert status['normal_finish'] == status['output_ok'] == '1'
    assert status['allocation_failed'] == status['active_at_finish'] == '0'
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in path.read_text().splitlines() if not line.startswith('#')))))
    assert len(rows) == int(status['rows'])
    if mode == 'overflow':
        assert len(rows) == 262144 and status['dropped'] == '7'
        continue
    assert status['dropped'] == '0'
    assert status['drain_complete'] == '1'
    assert status['transport'] == ('stream_mpsc_v1' if mode == 'stream' else 'deferred_arena_v1')
    spans = {r['span']: r for r in rows}
    assert len(spans) == len(rows)
    for r in rows:
        if r['parent'] != '0':
            p = spans[r['parent']]
            assert r['thread'] == p['thread']
            assert int(p['start_ns']) <= int(r['start_ns']) <= int(r['end_ns']) <= int(p['end_ns'])
        assert abs(float(r['wall_ms']) - float(r['children_ms']) - float(r['unattributed_ms'])) < 0.00001
    assert sum(r['phase'] == 'worker_child' for r in rows) == 240
    assert len({r['thread'] for r in rows if r['phase'] == 'worker'}) == 4
    assert any(r['phase'] == 'exception' and r['parent'] == '0' for r in rows)
    assert any(r['identity'] == 'identity,with"quote' for r in rows)
    assert any(r['phase'] == 'truncated' and r['identity_truncated'] == '1' for r in rows)
    assert all(float(r['thread_cpu_ms']) >= 0 for r in rows
               if r['parent'] == '0' and r['phase'] not in
               ('request', 'dependency', 'wall_envelope', 'legacy_none_parent'))
    assert all(float(r['thread_cpu_ms']) >= 0 for r in rows if r['phase'] == 'child')
    legacy = next(r for r in rows if r['phase'] == 'asset_argument')
    assert legacy['asset'] == 'legacy-asset.nif' and legacy['identity'] == ''
    request = next(r for r in rows if r['phase'] == 'request')
    dependency = next(r for r in rows if r['phase'] == 'dependency')
    assert dependency['link'] == request['span'] and dependency['parent'] == '0'
    assert dependency['thread'] != request['thread']
    assert request['metric1_name'] == 'requested_bytes' and request['metric1_value'] == '8192'
    assert dependency['metric1_name'] == 'returned_bytes' and dependency['metric1_value'] == '4096'
    assert request['thread_cpu_ms'] == dependency['thread_cpu_ms'] == '-1.000000'
    root_through = next(r for r in rows if r['phase'] == 'root_through_envelope')
    assert float(root_through['thread_cpu_ms']) >= 0
    for phase in ('wall_envelope', 'inner_wall_envelope', 'nested_wall_envelope',
                  'wall_below_legacy_none', 'root_below_cpu_root', 'root_below_legacy_none'):
        assert next(r for r in rows if r['phase'] == phase)['thread_cpu_ms'] == '-1.000000'
    clock = dict(line.split('=', 1) for line in Path(str(path) + '.clock-map.txt').read_text().splitlines())
    assert clock['normal_finish'] == '1'
    if sys.platform == 'win32':
        assert clock['thread_id_kind'] == 'windows_native_tid'
        assert int(clock['qpc_frequency']) > 0
        for prefix in ('start_', 'end_'):
            assert int(clock[prefix+'qpc_before']) <= int(clock[prefix+'qpc_after'])
        assert int(clock['start_steady_ns']) <= int(rows[0]['start_ns']) <= int(clock['end_steady_ns'])
for configured, expected in [('131072', '131072'), ('524288', '524288'), ('999999999', '262144'), ('524288junk', '262144')]:
    path = Path(output + '-capacity-' + configured + '.csv')
    env = os.environ.copy(); env['OPENMW_P9_HITCH_CAPACITY'] = configured
    subprocess.run([exe, str(path), 'normal'], check=True, env=env)
    status = read_status(Path(str(path) + '.capture-status.txt'))
    assert status['capacity'] == expected and status['dropped'] == '0'

# Lifetime volume is streamed without retaining millions of Python dictionaries.
for mode in ('stream-volume','stream-concurrent','stream-overflow','stream-stop','stream-init-failure',
             'stream-failure','stream-zero','stream-active'):
    path = Path(output+'-'+mode+'.csv')
    if mode == 'stream-failure' and path.exists() and path.is_dir():
        path.rmdir()  # Known empty fixture path only, never recursive cleanup.
    subprocess.run([exe,str(path),mode],check=True)
    status = read_status(Path(str(path)+'.capture-status.txt'))
    transport_accounting(status)
    assert status['transport'] == 'stream_mpsc_v1'
    if mode == 'stream-volume':
        assert status['attempted'] == status['accepted'] == status['written'] == '2500000'
        assert status['capacity'] == '131072' and status['dropped'] == status['pending'] == '0'
        assert status['normal_finish'] == status['output_ok'] == status['drain_complete'] == '1'
        assert int(status['writer_thread_id']) > 0
        with path.open(encoding='utf-8-sig',newline='') as handle:
            expected_id = 1
            for row in csv.DictReader(line for line in handle if not line.startswith('#')):
                assert int(row['span']) == expected_id and row['phase'] == 'fixture.volume'
                assert row['parent'] == row['link'] == '0'
                assert row['identity'] == 'volume,"quoted"' and row['asset'] == 'meshes/volume.nif'
                assert row['metric1_name'] == 'fixture_id' and int(row['metric1_value']) == expected_id
                assert int(row['end_ns'])-int(row['start_ns']) == 350000
                assert row['thread_cpu_ms'] == '-1.000000'
                expected_id += 1
            assert expected_id == 2500001
    elif mode == 'stream-concurrent':
        assert status['attempted'] == status['accepted'] == status['written'] == '40000'
        assert status['capacity'] == '128' and status['dropped'] == status['pending'] == '0'
        assert status['output_ok'] == status['drain_complete'] == '1'
        seen = bytearray(40001)
        threads = set()
        with path.open(encoding='utf-8-sig',newline='') as handle:
            for row in csv.DictReader(line for line in handle if not line.startswith('#')):
                identity = int(row['span'])
                assert 1 <= identity <= 40000 and not seen[identity]
                seen[identity] = 1
                threads.add(row['thread'])
                assert row['phase'] == 'fixture.volume' and row['parent'] == row['link'] == '0'
                assert row['identity'] == 'volume,"quoted"' and row['asset'] == 'meshes/volume.nif'
                assert row['metric1_name'] == 'fixture_id' and int(row['metric1_value']) == identity
                assert int(row['end_ns'])-int(row['start_ns']) == 350000
                assert row['thread_cpu_ms'] == '-1.000000'
        assert len(threads) == 4 and sum(seen) == 40000
    elif mode == 'stream-overflow':
        assert status['attempted'] == '15' and status['accepted'] == status['written'] == '8'
        assert status['capacity_dropped'] == status['dropped'] == '7'
    elif mode == 'stream-stop':
        assert status['attempted'] == '2' and status['accepted'] == status['written'] == '1'
        assert status['stopped_dropped'] == status['dropped'] == '1'
    elif mode == 'stream-init-failure':
        assert status['initialization_failed'] == '1' and status['allocation_dropped'] == '3'
        assert status['accepted'] == status['written'] == '0' and status['output_ok'] == '0'
    elif mode == 'stream-failure':
        assert path.is_dir() and status['io_dropped'] == '3' and status['writer_failed'] == '1'
        assert status['accepted'] == '3' and status['written'] == status['output_ok'] == '0'
    elif mode == 'stream-zero':
        assert status['attempted'] == status['accepted'] == status['written'] == status['dropped'] == '0'
        assert status['output_ok'] == status['drain_complete'] == '1'
    elif mode == 'stream-active':
        assert status['active_at_finish'] == '1' and status['output_ok'] == '0'
print('PASS legacy and streaming nested/concurrent/CPU/CSV contracts, 2.5M owned ordered rows, queue reuse, all loss arithmetic, initialization/output faults, stop races, zero rows and active shutdown')
