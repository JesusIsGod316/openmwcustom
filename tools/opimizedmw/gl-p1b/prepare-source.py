#!/usr/bin/env python3
"""One-time CI materialization; final engine builds use committed C++ directly."""
from pathlib import Path, PurePosixPath
import gzip
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)

def main():
    manifest = json.loads((HERE/'source-manifest.json').read_text())
    parts = ('0', '1', '2', '30', '31', '32', '33', '4')
    data = b''.join((HERE/f'implementation.patch.gz.part{i}').read_bytes() for i in parts)
    if hashlib.sha256(data).hexdigest() != manifest['patch_sha256']:
        raise RuntimeError('source transport checksum mismatch')
    paths = manifest['files']
    for rel in paths:
        path = PurePosixPath(rel)
        if path.is_absolute() or '..' in path.parts or not path.parts:
            raise RuntimeError('unsafe source path')
    actual = {}
    for rel in paths:
        result = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD:'+rel], cwd=ROOT, capture_output=True, text=True)
        actual[rel] = result.stdout.strip() if result.returncode == 0 else None
    if not all(actual[p] == paths[p]['after'] for p in paths):
        if not all(actual[p] == paths[p]['before'] for p in paths):
            raise RuntimeError('source does not match the exact P1A before-blobs')
        patch = gzip.decompress(data)
        target = HERE/'transport.tmp.patch'
        target.write_bytes(patch)
        try:
            subprocess.run(['git', 'apply', '--check', str(target)], cwd=ROOT, check=True)
            subprocess.run(['git', 'apply', str(target)], cwd=ROOT, check=True)
        finally:
            target.unlink(missing_ok=True)
    for rel in paths:
        if git('hash-object', rel).decode().strip() != paths[rel]['after']:
            raise RuntimeError('materialized source hash mismatch: '+rel)
    subprocess.run(['git', 'add', '--', *paths], cwd=ROOT, check=True)
    for rel in paths:
        if git('rev-parse', ':'+rel).decode().strip() != paths[rel]['after']:
            raise RuntimeError('indexed source hash mismatch: '+rel)
    subprocess.run(['git', 'diff', '--cached', '--check'], cwd=ROOT, check=True)
    output = ROOT/'p1b-evidence'; output.mkdir(exist_ok=True)
    (output/'tested-tree.txt').write_bytes(git('write-tree'))
    (output/'source-manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print('Exact allowlisted P1B source materialized and indexed; no runtime patch dependency.')

if __name__ == '__main__':
    main()
