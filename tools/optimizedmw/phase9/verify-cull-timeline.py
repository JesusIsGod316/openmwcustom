"""Exercise production cull queues and verify nested timing identities."""
import csv
import io
import os
from pathlib import Path
import subprocess
import sys

subprocess.run([sys.argv[1], '--startup', '--threaded'], check=True)
path = Path(os.environ['OPENMW_P9_HITCH_SPANS_FILE'])
rows = list(csv.DictReader(io.StringIO('\n'.join(
    line for line in path.read_text().splitlines() if not line.startswith('#')))))
spans = {int(row['span']): row for row in rows}
for row in rows:
    if int(row['parent']):
        parent = spans[int(row['parent'])]
        assert parent['thread'] == row['thread']
        assert int(parent['start_ns']) <= int(row['start_ns']) <= int(row['end_ns']) <= int(parent['end_ns'])
    assert abs(float(row['wall_ms']) - float(row['children_ms']) - float(row['unattributed_ms'])) < .00001
phases = {row['phase'] for row in rows}
assert {'renderer.cull', 'renderer.sceneview_traversal', 'renderer.draw'} <= phases, phases
traversals = [row for row in rows if row['phase'] == 'renderer.sceneview_traversal']
assert len(traversals) >= 5
assert all(float(row['wall_ms']) >= 2 and float(row['thread_cpu_ms']) >= 0 for row in traversals)
status = dict(line.split('=', 1) for line in Path(str(path)+'.capture-status.txt').read_text().splitlines())
assert status['dropped'] == status['active_at_finish'] == '0'
assert status['normal_finish'] == status['output_ok'] == '1'
print('PASS production cull traversal delay, threaded pixels/completion, same-thread parents, CPU observations and normal shutdown')
