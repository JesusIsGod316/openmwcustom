#!/usr/bin/env python3
"""Small source guards complement, not replace, native builds/runtime tests."""
from pathlib import Path
import hashlib
ROOT = Path(__file__).resolve().parents[3]
def text(path): return (ROOT/path).read_text(encoding='utf-8')
engine=text('apps/openmw/engine.cpp')
assert 'mOpimizedMWSpeculativeBudget' in engine and 'enableOpenGlSpeculativeBudget' in engine
cfg=text('files/settings-default.cfg')
assert 'opimizedmw host pressure = false' in cfg and 'opimizedmw speculative budget = false' in cfg
scene=text('components/resource/scenemanager.cpp')
assert scene.count('catch (const SpeculativeDeferred&) { throw; }') == 3
cell=text('apps/openmw/mwworld/cellpreloader.cpp')
assert 'deferRelease(entry.mWorkItem, item.retainedEstimate())' in cell
assert 'retainedCells' in cell and '!pair.second.mRetired' in cell
assert 'auto byteJob = std::move(mByteJob)' in cell
terrain=cell[cell.index('    class TerrainPreloadItem'):cell.index('    /// Worker thread item: update the resource')]
assert 'SpeculativeScope' not in terrain and 'mWorld->preload(' in terrain
assert 'OPENMW_ENABLE_V4_VULKAN_RUNTIME "Compile and link the experimental V4 VSG/Vulkan runtime into OpenMW" OFF' in text('CMakeLists.txt')
for path in ['components/settings/ramcache.hpp']:
    assert hashlib.sha256((ROOT/path).read_bytes().replace(b"\r\n", b"\n")).hexdigest() == 'e1d934ad187332e4dfb2520eee540c87a27bf3aa532e75b55ce86f6c321f324b'
print('P1B integration guards pass: opt-in controls, deferral propagation, retirement ownership, required-terrain boundary, retention profile. Not gameplay proof.')
