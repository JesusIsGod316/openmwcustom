"""Exercise actual configure, incremental staging and pinned PBR overrides."""
from pathlib import Path
import importlib.util, json, re, subprocess, sys, zipfile
root = Path(__file__).resolve().parents[3]
out = Path(sys.argv[1]).resolve()

def run(*args): subprocess.run(args, check=True)
run('cmake', '-S', str(Path(__file__).with_name('shader-stage')), '-B', str(out))
run('cmake', '--build', str(out), '--target', 'openmw-shader-resources')
run('ctest', '--test-dir', str(out), '--output-on-failure')
staged = out / 'staged/resources/shaders'
archive_path = out / 'shader-package/v3-rafael-pbr-0.52-overlay.zip'
spec = importlib.util.spec_from_file_location('resources', root/'tools/v4/cp4/shader_resources.py')
resources = importlib.util.module_from_spec(spec); spec.loader.exec_module(resources)
manifest = json.loads((staged/resources.MANIFEST).read_text())
assert manifest['groundcover_patch']['id'] == 'optimizedmw-p8g4-pbr-groundcover'
assert 'compatibility/groundcover_lod.glsl' in manifest['files']

# Resolve ONLY the added controls; retain every original PBR conditional exactly.
def original_control(source):
    stack=[]; active=True; result=[]
    for line in source.splitlines():
        s=line.strip()
        if re.match(r'#\s*(if|ifdef|ifndef)\b', s):
            m=re.fullmatch(r'#if\s+(!?)@(optimizedmwGroundcoverLod|optimizedmwGroundcoverFastWind)',s)
            if m:
                condition=bool(m[1]) # both values are zero in CONTROL
                stack.append((active,condition,True));active=active and condition
            else:
                stack.append((active,True,False))
                if active:result.append(line)
        elif re.match(r'#\s*else\b',s):
            parent,condition,custom=stack[-1]
            if custom: active=parent and not condition
            elif active: result.append(line)
        elif re.match(r'#\s*endif\b',s):
            parent,condition,custom=stack.pop()
            if not custom and active:result.append(line)
            active=parent
        elif active:result.append(line)
    assert not stack
    return '\n'.join(result)

def tokens(s):
    s=re.sub(r'/\*.*?\*/|//[^\n]*','',s,flags=re.S)
    return re.findall(r'\w+|[^\s]',s)

with zipfile.ZipFile(archive_path) as z:
    original={n:z.read(n) for n in z.namelist() if not n.endswith('/')}
    for name,data in original.items():
        actual=(staged/name).read_bytes()
        if name not in manifest['groundcover_patch']['files']:
            assert actual == data, ('unrelated PBR payload changed',name)
        else:
            assert actual != data, ('PBR overwrote the new code',name)
            assert tokens(original_control(actual.decode())) == tokens(data.decode()), ('CONTROL changed',name)
    # Negative control: the prior overlay-wins path has no effective LOD/wind.
    old=dict(original)
    assert b'optimizedmwGroundcoverLod' not in old['compatibility/groundcover.vert']
    patch_path=root/'files/shaders/v3overlay/p8g3/manifest.json'
    resources.apply_groundcover_patch(old,patch_path)
    assert b'optimizedmwGroundcoverLod' in old['compatibility/groundcover.vert']
    try:
        resources.apply_groundcover_patch(old,patch_path)
    except ValueError as e:
        assert 'parent mismatch' in str(e)
    else:raise AssertionError('a stale/double overlay patch must be rejected')

# Removal/corruption must fail verification and incremental production staging must repair it.
helper=staged/'compatibility/groundcover_lod.glsl'
helper.unlink()
try:resources.verify(staged)
except ValueError:pass
else:raise AssertionError('missing LOD helper passed verification')
run('cmake','--build',str(out),'--target','openmw-shader-resources')
resources.verify(staged)
print('P8G3 deployed PBR: two audited overrides; CONTROL token-equivalent; other overlay files byte-identical; missing-helper and stale-patch controls passed')
# Reproduce the Windows autocrlf failure without changing the real checkout.
# The audited payloads must retain LF while an ordinary file still becomes CRLF.
import tempfile
with tempfile.TemporaryDirectory(prefix='p8g3-checkout-') as temp:
    checkout=Path(temp)
    names=['files/shaders/v3overlay/p8g3/'+n for n in manifest['groundcover_patch']['files']]
    def git(*args):
        return subprocess.run(['git','-C',str(checkout),*args],check=True,
                              stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    git('init','-q')
    git('config','core.autocrlf','false')
    for name in names:
        target=checkout/name;target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes((root/name).read_bytes())
    (checkout/'ordinary.txt').write_bytes(b'ordinary\nline endings\n')
    git('add','.')
    git('config','core.autocrlf','true')
    for name in names+['ordinary.txt']:(checkout/name).unlink()
    git('checkout-index','--all','--force')
    for name in names:
        assert b'\r\n' in (checkout/name).read_bytes(), 'negative control did not use CRLF'
        assert (checkout/name).read_bytes() != (root/name).read_bytes()
    (checkout/'.gitattributes').write_bytes((root/'.gitattributes').read_bytes())
    for name in names+['ordinary.txt']:(checkout/name).unlink()
    git('checkout-index','--all','--force')
    for name in names:
        assert (checkout/name).read_bytes() == (root/name).read_bytes(), ('LF payload changed',name)
    assert b'\r\n' in (checkout/'ordinary.txt').read_bytes(), 'unrelated checkout policy was changed'
print('P8G3 Windows-autocrlf regression: old checkout differs; scoped LF policy preserves exact audited bytes')
