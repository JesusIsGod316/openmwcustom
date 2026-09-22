"""Source ownership guards for the combined repair, not a runtime acceptance test.

Pinned bodies are the unchanged required producers from GitHub 3de01e9b48.
An intentional later change requires an explicit review/update of this guard.
"""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parents[3]

def body(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i+1]
    raise ValueError('Unclosed function in source guard')

PRESERVED = [('apps/openmw/mwworld/scene.cpp', 'void Scene::loadCell', 'e6d670c5b8899902c1cc28fd7308ab2fe844ef20906d7b80b9ca4d5720492235'), ('apps/openmw/mwworld/scene.cpp', 'void Scene::unloadCell', '28951507c73854ca604f8474c43fc3ded18960f9d7b56d20aaf11a87e1ef0727'), ('apps/openmw/mwrender/v4enginerenderbridge.cpp', 'bool V4EngineRenderBridge::synchronizeExteriorTerrain', '8b07b4fa58ce52f2d23e6726f8702f24ec37436fc0511199126627ff21d14ad1'), ('apps/openmw/mwrender/v4enginerenderbridge.cpp', 'bool V4EngineRenderBridge::synchronizeGroundcover', '813ee4f349565d6ed4f56181ff9797596835a70d2491ac614721d9dd9b1b0b5e')]
for path, signature, expected in PRESERVED:
    text = (ROOT / path).read_text(encoding='utf-8')
    assert hashlib.sha256(body(text, signature).encode()).hexdigest() == expected, signature
    print('PASS unchanged required producer:', signature)
scene = (ROOT / 'apps/openmw/mwworld/scene.cpp').read_text()
preload = (ROOT / 'apps/openmw/mwworld/cellpreloader.cpp').read_text()
lifecycle = (ROOT / 'apps/openmw/mwrender/v4scenerenderlifecycle.cpp').read_text()
neutral = (ROOT / 'apps/openmw/mwworld/scenerenderlifecycle.hpp').read_text()
assert 'virtual bool usesLegacyTerrainPreload() const noexcept { return true; }' in neutral
assert 'OPENMW_V4_LEGACY_TERRAIN_PRELOAD_CONTROL' in lifecycle
assert 'OPENMW_V4_LEGACY_TERRAIN_FRONTLOAD' in lifecycle
assert 'return usesLegacyTerrainFrontload()' in body(lifecycle, 'bool V4SceneRenderLifecycle::usesLegacyTerrainPreload')
assert 'if (!mPreloader->usesLegacyTerrain()) return;' in body(scene, 'void Scene::preloadTerrain')
assert 'if (!mUseLegacyTerrain) return;' in body(preload, 'void CellPreloader::setTerrainPreloadPositions')
assert 'if (mPreloader->usesLegacyTerrain() && (v39NeedsInitialFrontload' in scene
assert 'if (mPreloader->usesLegacyTerrain())\n            mRendering.getPagedRefnums' in scene
assert 'if (mUseLegacyTerrain)\n                mTerrainView = mTerrain->createView();' in preload
assert 'if (mUseLegacyTerrain)\n                        mTerrain->cacheCell' in preload
assert 'mPreloadedObjects.insert(mLandManager->getLand(mCellLocation));' in preload
update = body(preload, 'void CellPreloader::updateCache')
assert 'waitTillDone' not in update and 'syncTerrainLoad' not in update
assert 'item->isDone() && !static_cast<const PreloadItem&>(*item).fullyPrepared()' in update
assert 'released.push_back(std::move(' in update
assert 'mReleasedPreloads.clear();' in preload
assert preload.index('mReleasedPreloads.clear();') < preload.index('mResourceSystem->updateCache(mReferenceTime);')
print('PASS legacy-only preload gates, independent controls, and completed-owner release without a new wait')
engine = (ROOT / 'apps/openmw/engine.cpp').read_text()
assert 'const bool hostMemoryBudget = mUseVulkanRenderer' in engine
assert 'OPENMW_V4_LEGACY_HOST_RETENTION_CONTROL' in engine
resources = (ROOT / 'components/resource/resourcesystem.cpp').read_text()
assert resources.index('manager->trimCache(maximum)') < resources.index('mSceneManager->trimCache(maximum)') < resources.index('mImageManager->trimCache(maximum)')
print('PASS Vulkan-only host policy and retaining-owner-before-image trim ordering')
print('Combined memory/terrain source guards: PASS; production compile and gameplay still required')
