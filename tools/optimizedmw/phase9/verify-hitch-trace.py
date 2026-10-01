import csv
import io
from pathlib import Path
import subprocess
import sys

exe, output = sys.argv[1:]
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
        assert len(rows) == 131072 and status['dropped'] == '7'
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
    assert all(float(r['thread_cpu_ms']) >= 0 for r in rows if r['parent'] == '0')
print('PASS nested spans, same-thread parents, exceptions, concurrent producers, CPU roots, CSV identity, truncation and overflow')
