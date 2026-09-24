#!/usr/bin/env python3
"""One-time disposable CI integration; normal builds require committed source."""
from pathlib import Path, PurePosixPath
import hashlib
import json
import os
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent

def git(*args):
    return subprocess.check_output(['git', *args], cwd=ROOT)

def blob(data):
    return hashlib.sha1(b'blob '+str(len(data)).encode()+b'\0'+data).hexdigest()

def head_blob(path):
    r = subprocess.run(['git','rev-parse','--verify','HEAD:'+path],cwd=ROOT,capture_output=True,text=True)
    return r.stdout.strip() if r.returncode == 0 else None

def main():
    manifest = json.loads((HERE/'source-manifest.json').read_text(encoding='utf-8'))
    paths = manifest['files']
    for name in paths:
        p = PurePosixPath(name)
        if p.is_absolute() or '..' in p.parts or not p.parts:
            raise RuntimeError('unsafe source path')
    actual = {p:head_blob(p) for p in paths}
    complete = all(actual[p] == v['after'] for p,v in paths.items())
    if '--verify-only' in sys.argv:
        if not complete:
            raise RuntimeError('full build requires already committed tested source')
        print('Committed P2 source and license identities verified; no patch applied.')
        return
    if not complete:
        if os.environ.get('GITHUB_ACTIONS') != 'true':
            raise RuntimeError('materialization allowed only in disposable Actions checkout')
        subprocess.run(['git','diff','--exit-code'],cwd=ROOT,check=True)
        subprocess.run(['git','diff','--cached','--exit-code'],cwd=ROOT,check=True)
        for p,v in paths.items():
            permitted = (v['before'],v['after']) if v['before'] else (None,v['after'])
            if actual[p] not in permitted:
                raise RuntimeError('unexpected original blob: '+p)
            if actual[p] is None and (ROOT/p).exists():
                raise RuntimeError('untracked destination already exists: '+p)
        # Normalize only verified, allowlisted source bytes, after identity checks.
        # Windows checkout CRLF is not used as input to a canonical Git patch.
        subprocess.run(['git','config','core.autocrlf','false'],cwd=ROOT,check=True)
        for p in paths:
            if actual[p] is not None:
                (ROOT/p).write_bytes(git('show','HEAD:'+p))
        changed = {p:v for p,v in paths.items() if v['before'] is not None}
        if all(actual[p] == v['before'] for p,v in changed.items()):
            patch = git('show','HEAD:tools/opimizedmw/gl-p2/integration.patch')
            if hashlib.sha256(patch).hexdigest() != manifest['patch_sha256']:
                raise RuntimeError('integration patch checksum mismatch')
            temporary = HERE/'integration.tmp.patch'
            temporary.write_bytes(patch)
            try:
                subprocess.run(['git','apply','--check',str(temporary)],cwd=ROOT,check=True)
                subprocess.run(['git','apply',str(temporary)],cwd=ROOT,check=True)
            finally:
                temporary.unlink(missing_ok=True)
        elif not all(actual[p] == v['after'] for p,v in changed.items()):
            raise RuntimeError('mixed original integration state')
        license_info = manifest['upstream_license']
        license_path = ROOT/license_info['path']
        if not license_path.exists():
            with urllib.request.urlopen(license_info['url'],timeout=60) as response:
                data = response.read()
            if blob(data) != license_info['git_blob']:
                raise RuntimeError('upstream license identity mismatch')
            license_path.parent.mkdir(parents=True,exist_ok=True)
            license_path.write_bytes(data)
    for p,v in paths.items():
        if blob((ROOT/p).read_bytes()) != v['after']:
            raise RuntimeError('materialized source hash mismatch: '+p)
    subprocess.run(['git','add','--',*paths],cwd=ROOT,check=True)
    for p,v in paths.items():
        if git('rev-parse',':'+p).decode().strip() != v['after']:
            raise RuntimeError('indexed source hash mismatch: '+p)
    checked = [p for p in paths if p != manifest['upstream_license']['path']]
    subprocess.run(['git','diff','--cached','--check','--',*checked],cwd=ROOT,check=True)
    output = ROOT/'p2-evidence'
    output.mkdir(exist_ok=True)
    (output/'tested-tree.txt').write_bytes(git('write-tree'))
    (output/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    (output/'OSGPL-LICENSE.txt').write_bytes((ROOT/manifest['upstream_license']['path']).read_bytes())
    print('Exact P2 integration staged for native verification; required waits remain.')

if __name__ == '__main__':
    main()
