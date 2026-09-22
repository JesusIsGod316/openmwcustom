#!/usr/bin/env python3
"""Reproduce the prior header collisions and check the private OS boundary.

Runs syntax-only negative controls on real baseline headers. The Windows control
uses the actual installed SDK, not a substitute. Real Qt positive tests are in
the accompanying CMake project; the negative Qt control uses its empty macro.
"""
import argparse
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', help='Git ref whose diagnostic headers must reproduce the failures')
    parser.add_argument('--output', type=Path, help='Preserve compiler control logs here')
    args = parser.parse_args()
    header = (ROOT / 'components/debug/runtimeprocessmemory.hpp').read_text()
    assert '#include' not in header, 'The public OS-probe declaration must not import platform or recorder headers'
    assert 'void sampleProcessMemory() noexcept;' in header
    manifest = (ROOT / 'components/CMakeLists.txt').read_text()
    assert re.search(r'add_component_dir\s*\(debug\s+[^)]*\bruntimeprocessmemory\b', manifest)
    for name in ('runtimediagnostics.hpp', 'gameplaydiagnostics.hpp'):
        text = (ROOT / 'components/debug' / name).read_text()
        assert not re.search(r'\bemit\s*\(', text), f'{name}: Qt-sensitive API returned'
    print('PASS private platform boundary and macro-safe diagnostic APIs')
    if not args.baseline:
        return
    compiler = shutil.which('cl' if os.name == 'nt' else os.environ.get('CXX', 'c++'))
    if not compiler:
        raise RuntimeError('A real C++ compiler is required for baseline controls')
    out = args.output.resolve() if args.output else None
    if out:
        out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='openmw-header-controls-') as tmp:
        temp = Path(tmp)
        for name in ('runtimediagnostics.hpp', 'runtimeprocessmemory.hpp'):
            result = subprocess.run(['git', 'show', f'{args.baseline}:components/debug/{name}'],
                                    cwd=ROOT, check=True, capture_output=True)
            (temp / name).write_bytes(result.stdout)
        controls = {'qt-keyword': '#define emit\n#include "runtimediagnostics.hpp"\n'}
        if os.name == 'nt':
            controls['windows-sdk'] = '''#include "runtimeprocessmemory.hpp"
struct ClipRange {
    float mNear = 0, mFar = 0;
    void updateSettings(float near, float far) { mNear = near; mFar = far; }
};
'''
        for name, source in controls.items():
            path = temp / f'{name}.cpp'
            path.write_text(source)
            flags = (['/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/permissive-',
                      '/D_CRT_SECURE_NO_WARNINGS', '/Zs'] if os.name == 'nt' else
                     ['-std=c++20', '-pthread', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-fsyntax-only'])
            result = subprocess.run([compiler, *flags, str(path)], capture_output=True, text=True, cwd=temp)
            log = result.stdout + result.stderr
            if out:
                (out / f'baseline-{name}.log').write_text(log)
            assert result.returncode != 0, f'{name}: old defect unexpectedly compiled'
            assert ('runtimediagnostics.hpp' in log if name == 'qt-keyword' else f'{name}.cpp' in log), log
            print(f'PASS baseline {name} collision reproduced with {Path(compiler).name}')
        if os.name != 'nt':
            print('WINDOWS SDK negative control unavailable on this host; required in Windows CI')


if __name__ == '__main__':
    main()
