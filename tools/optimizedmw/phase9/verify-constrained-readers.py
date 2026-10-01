"""Actual decoder/parser compatibility in separate processes with identical assets."""
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys

exe, prefix, source, archive = sys.argv[1:]
archive = Path(archive)
assert archive.is_file(), 'required installed BSA fixture is missing'
base_env = dict(os.environ)
results = {}
for name, refill, resident in (('CONTROL', '8192', '0'), ('I1', '65536', '0'),
                               ('I2', '8192', '1'), ('I1I2', '65536', '1')):
    stem = Path(prefix + '-' + name)
    output = Path(str(stem) + '.csv')
    for suffix in ('', '.clock-map.txt', '.capture-status.txt'):
        Path(str(output) + suffix).unlink(missing_ok=True)
    # This exact generated fixture path is the only archive written by the
    # executable. The input installed archive is always opened read-only.
    Path(str(stem) + '-real-writer.bsa').unlink(missing_ok=True)
    env = dict(base_env, OPENMW_P9_FILE_REFILL_BYTES=refill, OPENMW_P9_FILE_RESIDENT_SEEK=resident,
               OPENMW_P9_FILE_BUFFER_BUDGET_BYTES='33554432', OPENMW_P9_HITCH_SPANS_FILE=str(output))
    run = subprocess.run([exe, str(stem), source, str(archive)], env=env, text=True,
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=45)
    print(run.stdout.strip())
    if run.stderr.strip():
        print(run.stderr.strip())
    run.check_returncode()
    lines = [line for line in run.stdout.splitlines() if line.startswith(('IMAGE ', 'NIF ', 'BSA '))]
    assert len(lines) == 10, 'fixture did not report all image/NIF/BSA cases'
    results[name] = lines
    if name != 'CONTROL':
        assert lines == results['CONTROL'], f'{name} changed an authoritative parser result'
    status = dict(line.split('=', 1) for line in Path(str(output) + '.capture-status.txt').read_text().splitlines())
    assert status['dropped'] == status['allocation_failed'] == status['active_at_finish'] == '0'
    assert status['output_ok'] == status['normal_finish'] == '1'
    rows = list(csv.DictReader(io.StringIO('\n'.join(
        line for line in output.read_text().splitlines() if not line.startswith('#')))))
    def metrics(row):
        return {row[f'metric{i}_name']: int(row[f'metric{i}_value'])
                for i in range(1, 5) if row[f'metric{i}_name']}
    opens = {row['span'] for row in rows if row['phase'] == 'file.open_size' and metrics(row)['opened'] == 1}
    for phase in ('file.stream_totals', 'file.stream_policy', 'file.stream_buffer', 'file.stream_io', 'file.stream_consumption'):
        family = [row for row in rows if row['phase'] == phase]
        assert {row['link'] for row in family} == opens and len(family) == len(opens)
    policies = [metrics(row) for row in rows if row['phase'] == 'file.stream_policy']
    assert policies and all(row['requested_refill_bytes'] == int(refill) and row['requested_resident_seek'] == int(resident)
                            for row in policies)
    if name in ('I1', 'I1I2'):
        assert any(row['effective_refill_bytes'] == 65536 for row in policies)
    if name in ('I2', 'I1I2'):
        assert all(row['effective_resident_seek'] == 1 for row in policies)
    print(f'PASS {name}: identical complete image bytes/metadata, NIF hash/record/root/geometry, and BSA index; {len(opens)} traced streams')
with archive.open('rb') as stream:
    provenance_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
Path(prefix + '-results.json').write_text(json.dumps(dict(archive=str(archive), archive_sha256=provenance_hash,
                                                        parser_results=results), indent=2))
print('PASS actual DDS/TGA/NIF/BSA parser compatibility in all four isolated cells')
