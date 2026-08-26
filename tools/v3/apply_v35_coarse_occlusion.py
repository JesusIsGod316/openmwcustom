import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.5 match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.5 patched {rel} ({count} match(es))")


# Coarse groundcover-chunk MSOC. This deliberately leaves V3.4's broad individual-occluder mode off.
replace_exact(
    "components/settings/categories/camera.hpp",
    '''        SettingValue<bool> mV34BroadenOcclusion{ mIndex, "Camera", "v3.4 broaden occlusion" };''',
    '''        SettingValue<bool> mV34BroadenOcclusion{ mIndex, "Camera", "v3.4 broaden occlusion" };
        SettingValue<bool> mV35CoarseGroundcoverOcclusion{ mIndex, "Camera", "v3.5 coarse groundcover occlusion" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.4 broaden occlusion = false

[Cells]''',
    '''v3.4 broaden occlusion = false
# V3.5: test whole groundcover chunks against the already-built MSOC depth buffer before traversing their instances.
# This targets a larger unit of GPU work with one cheap AABB test. Disabled by default.
v3.5 coarse groundcover occlusion = false

[Cells]''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''namespace osg
{
    class Program;
}

namespace MWRender''',
    '''namespace osg
{
    class Program;
}
namespace SceneUtil
{
    class OcclusionCuller;
}

namespace MWRender''',
)
replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''        unsigned int getNodeMask() override;

        void reportStats''',
    '''        unsigned int getNodeMask() override;
        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, bool enabled);

        void reportStats''',
)
replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''        const MWWorld::GroundcoverStore& mGroundcoverStore;

        osg::ref_ptr<osg::Node> createChunk''',
    '''        const MWWorld::GroundcoverStore& mGroundcoverStore;
        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;
        bool mCoarseOcclusion = false;

        osg::ref_ptr<osg::Node> createChunk''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>''',
    '''#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/nodecallback.hpp>
#include <components/sceneutil/occlusionculling.hpp>''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        class ViewDistanceCallback : public SceneUtil::NodeCallback<ViewDistanceCallback>
        {''',
    '''        class GroundcoverOcclusionCallback
            : public SceneUtil::NodeCallback<GroundcoverOcclusionCallback, osg::Node*, osgUtil::CullVisitor*>
        {
        public:
            explicit GroundcoverOcclusionCallback(SceneUtil::OcclusionCuller* culler)
                : mCuller(culler)
            {
            }

            void operator()(osg::Node* node, osgUtil::CullVisitor* cv)
            {
                if (!mCuller || !mCuller->isFrameActive())
                {
                    traverse(node, cv);
                    return;
                }

                const osg::BoundingSphere& bs = node->getBound();
                if (!bs.valid())
                {
                    traverse(node, cv);
                    return;
                }

                osg::Matrixd viewInverse;
                viewInverse.invert(cv->getCurrentCamera()->getViewMatrix());
                const osg::Matrixd modelToWorld = *cv->getModelViewMatrix() * viewInverse;
                const osg::Vec3f center = bs.center() * modelToWorld;
                const float r = bs.radius();
                const osg::BoundingBox worldBB(center.x() - r, center.y() - r, center.z() - r,
                    center.x() + r, center.y() + r, center.z() + r);

                if (!mCuller->testVisibleAABB(worldBB))
                    return;

                traverse(node, cv);
            }

        private:
            osg::ref_ptr<SceneUtil::OcclusionCuller> mCuller;
        };

        class ViewDistanceCallback : public SceneUtil::NodeCallback<ViewDistanceCallback>
        {''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''            osg::ref_ptr<osg::Node> node = createChunk(instances, center);
            mCache->addEntryToObjectCache(id, node.get());
            return node;''',
    '''            osg::ref_ptr<osg::Node> node = createChunk(instances, center);
            if (node && mCoarseOcclusion && mOcclusionCuller)
                node->addCullCallback(new GroundcoverOcclusionCallback(mOcclusionCuller));
            mCache->addEntryToObjectCache(id, node.get());
            return node;''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''    Groundcover::~Groundcover() = default;

    void Groundcover::collectInstances''',
    '''    Groundcover::~Groundcover() = default;

    void Groundcover::setOcclusionCuller(SceneUtil::OcclusionCuller* culler, bool enabled)
    {
        mOcclusionCuller = culler;
        mCoarseOcclusion = enabled;
        mCache->clear();
    }

    void Groundcover::collectInstances''',
)

replace_exact(
    "apps/openmw/mwrender/renderingmanager.cpp",
    '''                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());''',
    '''                newChunkMgr.mGroundcover = std::make_unique<Groundcover>(
                    mResourceSystem->getSceneManager(), density, groundcoverDistance, mGroundCoverStore);
                if (mOcclusionCuller)
                    newChunkMgr.mGroundcover->setOcclusionCuller(
                        mOcclusionCuller, Settings::camera().mV35CoarseGroundcoverOcclusion);
                quadTreeWorld->addChunkManager(newChunkMgr.mGroundcover.get());''',
)

# Extend V3.4 launcher with an isolated coarse-group experiment and a combined known-good configuration.
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''$V34BroadenOcclusion = 'false'
$OccluderMinRadius = '400' ''',
    '''$V34BroadenOcclusion = 'false'
$V35CoarseGroundcoverOcclusion = 'false'
$OccluderMinRadius = '400' ''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
do { $choice = Read-Host 'Enter 1 through 18' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18'))''',
    '''Write-Host ' 18 = V3.4 full combined: MSOC + Lua fast path + aggressive far shadow'
Write-Host ' 19 = V3.5 coarse groundcover-chunk MSOC (normal V3 occluder budget)'
Write-Host ' 20 = V3.5 coarse MSOC + Lua fast path + far shadow divisor 4'
do { $choice = Read-Host 'Enter 1 through 20' } until ($choice -in @('1','2','3','4','5','6','7','8','9','10','11','12','13','14','15','16','17','18','19','20'))''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
}''',
    '''    '18' { $Experiment = 'v34-full-combined'; $V34BroadenOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4'; $OccluderMinRadius = '250'; $OccluderMaxDistance = '8192'; $OcclusionMaxTriangles = '45000' }
    '19' { $Experiment = 'v35-coarse-groundcover-msoc'; $V35CoarseGroundcoverOcclusion = 'true' }
    '20' { $Experiment = 'v35-coarse-msoc-lua-shadow'; $V35CoarseGroundcoverOcclusion = 'true'; $LuaIdleTimerFastPath = 'true'; $FarShadowResolutionDivisor = '4' }
}''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "occlusion_occluder_min_radius=$OccluderMinRadius",''',
    '''    "v34_broaden_occlusion=$V34BroadenOcclusion",
    "v35_coarse_groundcover_occlusion=$V35CoarseGroundcoverOcclusion",
    "occlusion_occluder_min_radius=$OccluderMinRadius",''',
)
replace_exact(
    "tools/v3/launchers/V3_Lab.ps1",
    '''    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius''',
    '''    Set-IniValue $SettingsPath 'Camera' 'v3.4 broaden occlusion' $V34BroadenOcclusion
    Set-IniValue $SettingsPath 'Camera' 'v3.5 coarse groundcover occlusion' $V35CoarseGroundcoverOcclusion
    Set-IniValue $SettingsPath 'Camera' 'occlusion occluder min radius' $OccluderMinRadius''',
)

print("V3.5 coarse groundcover occlusion layer applied.")
