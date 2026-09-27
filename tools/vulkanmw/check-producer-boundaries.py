#!/usr/bin/env python3
"""No-new-boundary-violations gate for the exact 348bfe6 producer-queue base.

The original architecture guard is kept unchanged and is still RED at this
checkpoint because of pre-existing FX imports. This comparison is not an
architecture acceptance waiver: only byte-identical inherited files/diagnostics
are allowed. Removing a violation is allowed; adding or relocating one is not.
"""
import argparse
from collections import Counter
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BASE = '348bfe6b087b7c8b83738e12a11a7e10f9e592ef'
INHERITED = {'components/render/backend/vsg/fximagecapture.hpp',
             'components/render/backend/vsg/omwfx.cpp'}


def report(root):
    run = subprocess.run([sys.executable, str(root / 'tools/vulkanmw/check-architecture-boundaries.py')],
                         cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(run.stdout, end='')
    if run.returncode not in (0, 1):
        raise RuntimeError('Architecture guard failed to execute')
    failures = Counter(line.strip() for line in run.stdout.splitlines() if line.startswith('  '))
    if run.returncode == 1 and not failures:
        raise RuntimeError('Architecture guard failed without recognized diagnostics')
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    args = parser.parse_args()
    baseline = args.baseline.resolve()
    print('BASELINE architecture guard:')
    old = report(baseline)
    for diagnostic in old:
        if diagnostic.split(':', 1)[0] not in INHERITED:
            raise AssertionError('Unexpected baseline violation: ' + diagnostic)
    print('CANDIDATE architecture guard:')
    new = report(ROOT)
    if new - old:
        raise AssertionError('New architecture violations: ' + repr(new - old))
    for diagnostic in new:
        path = diagnostic.split(':', 1)[0]
        if (baseline / path).read_bytes() != (ROOT / path).read_bytes():
            raise AssertionError('Inherited violating source was changed: ' + path)
    print(f'NO NEW violations PASS; inherited architecture failures remain: {sum(new.values())}')


if __name__ == '__main__':
    main()
