#!/usr/bin/env python3
"""Verify the evidence written at exit by the real GL/startup fixtures."""
import csv
from pathlib import Path
import sys

root = Path(sys.argv[1])
def rows(name):
    with (root / name).open(newline='', encoding='utf-8') as f:
        return list(csv.DictReader(f))
def status(name):
    return dict(line.split('=', 1) for line in (root / name).read_text().splitlines() if '=' in line)

leaf = status('startup-leaf.csv.status.txt')
assert int(leaf['visitor_instances']) == 2 and int(leaf['total_calls']) >= 5, leaf
assert leaf['coverage_valid'] == '1' and leaf['rows_dropped'] == '0', leaf
frames = rows('startup-leaf.csv.frames.csv')
assert sum(int(r['calls']) for r in frames) == int(leaf['total_calls'])
api = rows('gl-gl.csv')
slow = [r for r in api if r['api'] == 'glBufferSubData' and r['frame'] == '2']
assert len(slow) == 1 and slow[0]['phase'] == '2' and slow[0]['drawable'] == '123', slow
assert float(slow[0]['duration_ms']) >= 2.0, slow
assert [int(slow[0][f'arg{i}']) for i in range(3)] == [34962, 0, 4], slow
aggregate = [r for r in rows('gl-gl.csv.frames.csv') if r['frame'] == '2' and r['category'] == 'buffer']
assert len(aggregate) == 1 and int(aggregate[0]['calls']) == 1, aggregate
assert abs(float(aggregate[0]['total_ms']) - float(slow[0]['duration_ms'])) < 0.00001
assert int(status('startup-gl.csv.status.txt')['calls']) > 0
assert int(status('prep-shared.txt')['calls']) > 0
print('PASS: real post-exit live coverage, aggregate accounting, delayed-call frame/phase/drawable/arguments, shared-prep engagement')
