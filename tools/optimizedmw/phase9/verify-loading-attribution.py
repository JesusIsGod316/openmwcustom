"""Exercise production logical-I/O instrumentation and causal-link semantics."""
import csv
import io
from pathlib import Path
import subprocess
import sys

exe, prefix = sys.argv[1:]
for mode in ('unarmed', 'armed'):
    stem = Path(prefix + '-' + mode)
    stem.parent.mkdir(parents=True, exist_ok=True)
    path = Path(str(stem) + '.csv')
    for suffix in ('', '.clock-map.txt', '.capture-status.txt'):
        Path(str(path) + suffix).unlink(missing_ok=True)
    subprocess.run([exe, str(stem), mode], check=True)
    if mode == 'unarmed':
        assert not path.exists(), 'normal path unexpectedly recorded'
        continue
    status = dict(line.split('=', 1) for line in Path(str(path) + '.capture-status.txt').read_text().splitlines())
    assert status['dropped'] == status['allocation_failed'] == status['active_at_finish'] == '0'
    assert status['normal_finish'] == status['output_ok'] == '1'
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in path.read_text().splitlines() if not line.startswith('#')))))
    assert len(rows) == int(status['rows'])
    spans = {r['span']: r for r in rows}
    assert len(spans) == len(rows)
    for row in rows:
        if row['parent'] != '0':
            parent = spans[row['parent']]
            assert row['thread'] == parent['thread']
            assert int(parent['start_ns']) <= int(row['start_ns']) <= int(row['end_ns']) <= int(parent['end_ns'])
        if row['link'] != '0':
            assert row['link'] in spans, 'causal ID refers to filtered or missing span'

    def metrics(row):
        return {row[f'metric{i}_name']: int(row[f'metric{i}_value'])
                for i in range(1, 5) if row[f'metric{i}_name']}

    hashes = {r['asset']: r for r in rows if r['phase'] == 'asset.hash'}
    for size in (0, 1, 128, 1000, 4096, 4097, 5000):
        data = metrics(hashes[f'logical/hash-{size}.nif'])
        assert data['logical_bytes'] == size
        assert data['hash_blocks'] == (size + 4095) // 4096
        assert data['rewound'] == 1
    failure = metrics(hashes['logical/read-failure.nif'])
    assert failure['logical_bytes'] == 4096 and failure['hash_blocks'] == 1 and failure['rewound'] == 0
    opens = [r for r in rows if r['phase'] == 'file.open_size']
    successful = next(r for r in opens if r['asset'].endswith('-physical,asset.nif'))
    missing = next(r for r in opens if r['asset'].endswith('-missing.nif'))
    assert metrics(successful) == {'region_origin': 17, 'region_size': 4097, 'opened': 1}
    assert metrics(missing)['opened'] == 0
    totals = next(r for r in rows if r['phase'] == 'file.stream_totals')
    assert totals['link'] == successful['span'] and totals['asset'] == successful['asset']
    data = metrics(totals)
    assert data['read_calls'] == 4 and data['returned_bytes'] == 8194 and data['seek_calls'] == 3
    assert data['read_call_ns'] >= 0
    for read in (r for r in rows if r['phase'] == 'file.read'):
        data = metrics(read)
        assert read['link'] == successful['span'] and read['asset'] == successful['asset']
        assert data['file_offset'] in (17, 4114)
        assert data['returned_bytes'] <= data['requested_bytes'] <= 4097
        assert data['read_completed'] == 1
    job = next(r for r in rows if r['phase'] == 'fixture.terrain_create')
    request = next(r for r in rows if r['phase'] == 'fixture.terrain_request')
    work = next(r for r in rows if r['phase'] == 'fixture.terrain_work')
    assert job['link'] == request['span']
    assert work['parent'] == '0' and work['thread'] != job['thread']
    for row in (r for r in rows if r['phase'].startswith('fixture.terrain_')
                and r['phase'] not in ('fixture.terrain_create', 'fixture.terrain_request')):
        assert row['link'] == job['span']
print('PASS causal job links, same-thread nesting, original hash/rewind, logical totals and disabled path')
