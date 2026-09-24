#!/usr/bin/env python3
"""One-time connector transport materialization, not a gameplay/build generator.

CI tests the full source after applying this exact allowlisted delta, then
publishes that materialized source. Subsequent builds need no patch application.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def run(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True).strip()

def blob(path):
    data = (ROOT / path).read_bytes().replace(b'\r\n', b'\n')
    return hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--apply', action='store_true')
    args = parser.parse_args()
    manifest = json.loads((HERE / 'source-manifest.json').read_text())
    patch = HERE / 'implementation.patch'
    if hashlib.sha256(patch.read_bytes().replace(b'\r\n', b'\n')).hexdigest() != manifest['patch_sha256']:
        raise RuntimeError('Transport patch checksum mismatch')
    entries = manifest['paths']
    old = all(blob(p) == h['before'] for p, h in entries.items())
    new = all(blob(p) == h['after'] for p, h in entries.items())
    if old and args.apply:
        subprocess.run(['git', 'apply', '--check', '--whitespace=error-all', str(patch)], cwd=ROOT, check=True)
        subprocess.run(['git', 'apply', '--whitespace=error-all', str(patch)], cwd=ROOT, check=True)
        new = all(blob(p) == h['after'] for p, h in entries.items())
    if not new:
        raise RuntimeError('Expected exact P1A source; no mixed or unreviewed base accepted')
    # Protect the independently accepted retention and required-content code.
    for path in ('components/settings/ramcache.hpp', 'apps/openmw/mwworld/scene.cpp'):
        if blob(path) != run('git', 'rev-parse', manifest['base'] + ':' + path):
            raise RuntimeError('Protected required-content/retention path changed: ' + path)
    engine = (ROOT / 'apps/openmw/engine.cpp').read_text()
    assert 'if (!mUseVulkanRenderer && static_cast<bool>(Settings::cells().mOpimizedMWHostPressure))' in engine
    assert 'const bool hostMemoryBudget = mUseVulkanRenderer' in engine
    assert 'opimizedmw host pressure = false' in (ROOT / 'files/settings-default.cfg').read_text()
    assert 'legacyAdmission && !mResourceSystem->openGlHostMemoryBudgetEnabled()' in (
        ROOT / 'apps/openmw/mwworld/cellpreloader.cpp').read_text()
    subprocess.run(['git', 'diff', '--check'], cwd=ROOT, check=True)
    print('PASS exact GL-P1A production source, isolated opt-in, preserved retention/required content')

if __name__ == '__main__':
    main()
