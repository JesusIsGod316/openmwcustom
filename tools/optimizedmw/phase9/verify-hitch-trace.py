import csv
import io
import os
from pathlib import Path
import subprocess
import sys

exe, output = sys.argv[1:]
unarmed = Path(output + '-unarmed.csv')
for p in (unarmed, Path(str(unarmed)+'.clock-map.txt'), Path(str(unarmed)+'.capture-status.txt')):
    if p.exists(): p.unlink()
subprocess.run([exe, str(unarmed), 'unarmed'], check=True)
assert not unarmed.exists() and not Path(str(unarmed)+'.clock-map.txt').exists()
for mode in ('normal', 'overflow'):
    path = Path(output + '-' + mode + '.csv')
    subprocess.run([exe, str(path), mode], check=True)
    status = dict(line.split('=', 1) for line in Path(str(path) + '.capture-status.txt').read_text().splitlines())
    assert status['normal_finish'] == status['output_ok'] == '1'
    assert status['allocation_failed'] == status['active_at_finish'] == '0'
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in path.read_text().splitlines() if not line.startswith('#')))))
    assert len(rows) == int(status['rows'])
    if mode == 'overflow':
        assert len(rows) == 262144 and status['dropped'] == '7'
        continue
    assert status['dropped'] == '0'
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
    status = dict(line.split('=', 1) for line in Path(str(path) + '.capture-status.txt').read_text().splitlines())
    assert status['capacity'] == expected and status['dropped'] == '0'
print('PASS nested spans, same-thread parents, exceptions, concurrent producers, CPU roots, CSV identity, truncation and overflow')
