"""Validate the actual Phase 9 Windows installation and source/runtime identities."""
import base64
import hashlib
import json
import os
from pathlib import Path
import sys

root=Path(sys.argv[1]); source=Path(__file__).resolve().parents[3]
public=source/'tools/optimizedmw/phase9'
files=['openmw.exe','CI-ID.txt','START-OptimizedMW-Test.bat','OptimizedMW_Test.ps1',
       'OptimizedMW_Benchmark_Report.ps1','OptimizedMW_ProfileArchive.ps1','OptimizedMW-Phase9-README.txt']
for name in files:
    assert (root/name).is_file() and (root/name).stat().st_size>0,name
assert (root/'openmw.exe').read_bytes()[:2]==b'MZ', 'not a Windows PE executable'
assert sorted(p.name.lower() for p in root.glob('*.bat'))==['start-optimizedmw-test.bat']
assert os.environ['EXPECTED_COMMIT'] in (root/'CI-ID.txt').read_text()
manifest=json.loads((root/'resources/shaders/shader-package.json').read_text())
expected=json.loads((source/'files/shaders/v3overlay/p8g3/manifest.json').read_text())
assert expected['id']=='optimizedmw-p8g4-pbr-groundcover'
assert manifest['groundcover_patch']==expected, 'retained PBR overlay changed'
for name in files[3:]:
    assert (root/name).read_bytes()==(public/name).read_bytes(),name
assert (root/'START-OptimizedMW-Test.bat').read_bytes()==(source/'tools/optimizedmw/gl-p8g4/START-OptimizedMW-Test.bat').read_bytes()
settings=base64.b64decode((root/'defaults.bin').read_bytes(),validate=True).decode('utf-8')
for key in ('optimizedmw canonical terrain textures','optimizedmw composite slicing',
            'optimizedmw object userdata cache','warm sounds','optimizedmw setting consistency'):
    assert key+' = false' in settings,key
for name in ('groundcover.vert','groundcover.frag','groundcover_lod.glsl','temporal_camera_motion.vert',
             'temporal_camera_motion.frag','temporal_motion_view.vert','temporal_motion_view.frag','temporal_dynamic_motion.vert','temporal_dynamic_motion.frag'):
    relative='compatibility/'+name; deployed=root/'resources/shaders'/relative
    assert deployed.is_file() and hashlib.sha256(deployed.read_bytes()).hexdigest()==manifest['files'][relative],relative
    if name.startswith('temporal_'):
        assert deployed.read_bytes()==(source/'files/shaders'/relative).read_bytes(),relative
    files.append('resources/shaders/'+relative)
lines=[hashlib.sha256((root/n).read_bytes()).hexdigest()+'  '+n for n in files]
Path('Phase9-INSTALLED-SHA256.txt').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
