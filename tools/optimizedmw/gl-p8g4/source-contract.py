#!/usr/bin/env python3
"""P8G4 integration/package guards supplement, not replace, native tests."""
from pathlib import Path
import re,json,hashlib
root=Path(__file__).resolve().parents[3]
def read(p):return (root/p).read_text()
def need(ok,label):
 if not ok:raise SystemExit('P8G4 contract failed: '+label)
g=read('components/sceneutil/groundcoverbatch.hpp');o=read('apps/openmw/mwrender/objectpaging.cpp')
h=read('components/debug/v3hitchtelemetry.hpp');e=read('apps/openmw/engine.cpp');c=read('components/resource/benchmarkcapture.hpp')
s=read('components/sceneutil/shadowproxygroup.cpp');l=read('tools/optimizedmw/gl-p8g4/OptimizedMW_Test.ps1')
for setting in ['optimizedmw cull input reuse','optimizedmw lod2','optimizedmw active shadow batching']:
 need(setting+' = false' in read('files/settings-default.cfg'),'default-off '+setting)
need('inline static thread_local CullInputScope* current' in g and '~CullInputScope()' in g,'bounded traversal scope')
need('view == camera->getViewMatrix()' in g and 'modelView == *cv.getModelViewMatrix()' in g,'exact view/transform cache inputs')
need('No per-geometry StateSet/uniform copies on LOD2' in g and 'mLod2States->state(bucket)' in g,'shared policy state')
need('const bool p3ShadowStaticBatching' in o and '&& compile && !p2RequiredReadiness && SceneUtil::PagingWorkScope::optionalOptimization()' in o,'no required-path shadow construction')
need('p3ShadowRoot->accept(stateToCompile)' in o,'proxy GL resources included in compilation')
need('SMALL_FEATURE_CULLING' in s and 'normalNode()' in s,'preserve far-caster pruning')
need('mCleanFrames.push' in h and 'mCleanFrames.prepare(131072)' in h,'bounded raw frame capture')
need('if (!cleanCapture && stats.is_open())' in e and 'frameNumber % 60 == 0' in e,'no clean-mode text dump; sampled resource stats')
need(e.index('state().finish();')<e.index('numericCapture.finish();'),'finish gameplay timer before I/O')
need('getAttribute' in c and 'glGetQuery' not in c and 'glFinish' not in c,'CPU-side delayed results only')
need('readFrame' in c and 'NaN' in c and 'render_dropped' in c,'explicit origin, missing and overflow')
for mode in ['REFERENCE','COMBINED','CULL-CPU','LOD2','SHADOW-BATCH','LEGACY-LOGGING']:need(mode in l,'launcher mode '+mode)
for key,value in [('number of shadow maps','3'),('shadow map resolution','2048'),('maximum shadow map distance','4096')]:
 need("'"+key+"' '"+value+"'" in l,'normal shadow cohort '+key)
need("$FastWind='0'" in l and "$FrontToBack='false'" in l,'parked paths stay off')
need('settings_restore_verified' in l and '$originalEnv' in l,'settings/environment restored')
need('Add-Content -LiteralPath $memoryCsv' not in l,'memory sampler avoids gameplay I/O')
cm=read('CMakeLists.txt')
installed=re.findall(r'"\$\{CMAKE_SOURCE_DIR\}/([^"\n]+\.bat)"',cm)
need(installed==['tools/optimizedmw/gl-p8g4/START-OptimizedMW-Test.bat'],'one public BAT whitelist')
manifest=json.loads(read('files/shaders/v3overlay/p8g3/manifest.json'))
need(manifest['id']=='optimizedmw-p8g4-pbr-groundcover','deployed P8G4 identity')
for name,entry in manifest['files'].items():
 need(hashlib.sha256((root/'files/shaders/v3overlay/p8g3'/name).read_bytes()).hexdigest()==entry['after_sha256'],'deployed payload '+name)
print('P8G4 source, telemetry, shadow-quality and single-launcher contracts passed')
