"""Validate the delivered package, not merely the source tree."""
import base64
import hashlib
import json
import os
from pathlib import Path
import sys

root=Path(sys.argv[1]); source=Path(__file__).resolve().parents[3]
files=['openmw.exe','START-OptimizedMW-Test.bat','OptimizedMW_Test.ps1',
       'OptimizedMW_Benchmark_Report.ps1','OptimizedMW-P8U1-README.txt']
for name in files:
    assert (root/name).is_file(),name
assert sorted(p.name for p in root.glob('*.bat'))==['START-OptimizedMW-Test.bat']
assert os.environ['EXPECTED_COMMIT'] in (root/'CI-ID.txt').read_text()
manifest=json.loads((root/'resources/shaders/shader-package.json').read_text())
expected=json.loads((source/'files/shaders/v3overlay/p8g3/manifest.json').read_text())
assert expected['id']=='optimizedmw-p8g4-pbr-groundcover'
assert manifest['groundcover_patch']==expected
for name in files[2:]:
    assert (root/name).read_bytes()==(source/'tools/optimizedmw/p8u1'/name).read_bytes(),name
assert (root/'START-OptimizedMW-Test.bat').read_bytes()==(source/'tools/optimizedmw/gl-p8g4/START-OptimizedMW-Test.bat').read_bytes()
# Installed Windows source files have scoped LF attributes so cross-platform hashes agree.
settings=base64.b64decode((root/'defaults.bin').read_bytes(), validate=True).decode('utf-8')
for key in ('optimizedmw canonical terrain textures','optimizedmw composite slicing',
            'optimizedmw object userdata cache','warm sounds','optimizedmw setting consistency'):
    assert key+' = false' in settings,key
files += ['resources/shaders/compatibility/groundcover.vert','resources/shaders/compatibility/groundcover.frag',
          'resources/shaders/compatibility/groundcover_lod.glsl']
lines=[hashlib.sha256((root/n).read_bytes()).hexdigest()+'  '+n for n in files]
Path('P8U1-INSTALLED-SHA256.txt').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
