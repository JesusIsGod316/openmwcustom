#!/usr/bin/env python3
from pathlib import Path
import re
root=Path(__file__).resolve().parents[3]
def read(path): return (root/path).read_text()
def require(ok, name):
    if not ok: raise SystemExit('P8G3 source contract failed: '+name)
g=read('apps/openmw/mwrender/groundcover.cpp')
h=read('components/sceneutil/groundcoverbatch.hpp')
p=read('components/sceneutil/groundcoverpolicy.hpp')
v=read('files/shaders/compatibility/groundcover.vert')
f=read('files/shaders/compatibility/groundcover.frag')
r=read('apps/openmw/mwrender/renderingmanager.cpp')
s=read('files/settings-default.cfg')
l=read('tools/optimizedmw/gl-p8g3/OptimizedMW_GL-P8G3_Test.ps1')
for key in ['hierarchy','density lod','front to back']:
    require('optimizedmw '+key+' = false' in s, 'default-off '+key)
require('optimizedmw fast wind = -1' in s, 'P8G2 wind compatibility')
for x in ['optimizedmwGroundcoverFastWind','optimizedmwGroundcoverLod']:
    require(x in r and x in v, 'live shader define '+x)
require('if (derived &&' in g and 'InstancingVisitor visitor(entries, worldCenter)' in g, 'stock fallback preserved')
require('std::this_thread::get_id() != mOwnerThread' in g, 'no owner thread fork/join')
require('remainingExtraDraws = 32' in g and 'maxExtraDraws / drawables' in p, 'draw budget')
require('!culler->isFrameActive()' in g and 'Constants::SceneCamera' in g, 'view-local occlusion')
require('std::try_to_lock' in h and 'if (desired <= previous)' in p, 'conservative no-wait LOD')
require('setNumInstances' not in h[h.index('class LodCallback'):h.index('class EligibilityVisitor')], 'no cull-time mutation')
require('DEEP_COPY_ARRAYS' not in h, 'shared immutable vertex data')
require('setVertexAttribArray(8' not in g+h and 'setVertexAttribArray(9' not in g+h, 'no UV alias')
require('aRotation.w' in v and 'p8g3LodParams.x > 0.0' in v, 'rank plus unsupported-geometry fallback')
require('gl_FragData[0].a *= p8g3Coverage;' in f, 'LOD alpha before original test')
require('SORT_BY_STATE_THEN_FRONT_TO_BACK' in h and 'setDefaultRenderBinSortMode' not in h, 'local sorting only')
for mode in ['CONTROL','HIER-CULL','HIER-LOD','FAST-WIND-ONLY','ORDER-ONLY','HIER-LOD-WIND','FULL']:
    require(mode in l, 'launcher '+mode)
require("$GroundcoverGpuPath='0'" in l and "$GroundcoverGpuPath='1'" not in l, 'unpromoted shader math not forced')
require('SHADOW-LITE' not in l and 'number of shadow maps' not in l, 'normal shadow quality')
require('settings_restore_verified' in l and '$originalEnv' in l, 'launcher restore')
for file in ['START-OptimizedMW-GL-P8G3-Test.bat','OptimizedMW_GL-P8G3_Test.ps1']:
    require(file in read('CMakeLists.txt'), 'packaged '+file)
print('P8G3 source integration contracts passed; runtime and native tests are separate')

cm = read('files/shaders/CMakeLists.txt')
require('compatibility/groundcover_lod.glsl' in cm, 'LOD helper is actually packaged')
require(cm.count('--groundcover-patch "${_groundcover_patch}"') == 2, 'configure and incremental PBR deployment')
pv = read('files/shaders/v3overlay/p8g3/compatibility/groundcover.vert')
pf = read('files/shaders/v3overlay/p8g3/compatibility/groundcover.frag')
require('optimizedmwGroundcoverLod' in pv and 'optimizedmwGroundcoverFastWind' in pv, 'deployed shader switches')
require('alpha *= p8g3Coverage;' in pf and pf.index('alpha *= p8g3Coverage;') > pf.index('alpha = mix(alpha'), 'PBR edge refinement cannot restore omitted grass')
require('ProcessLighting' in pf and 'GROUNDCOVER_SSS' in pf and 'Gust' in pv, 'retain PBR shader semantics')
require('40.0 * std::abs(w)' in p, 'PBR long-wind lean covered')
