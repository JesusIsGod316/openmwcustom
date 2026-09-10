#ifndef OPENMW_MWRENDER_GROUNDCOVER_H
#define OPENMW_MWRENDER_GROUNDCOVER_H

#include <atomic>

#include <components/esm3/loadcell.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/terrain/quadtreeworld.hpp>
#include <components/vfs/pathutil.hpp>

namespace MWWorld
{
    class ESMStore;
    class GroundcoverStore;
}
namespace osg
{
    class Program;
}

namespace SceneUtil
{
    class OcclusionCuller;
}

namespace MWRender
{
    typedef std::tuple<osg::Vec2f, float> GroundcoverChunkId; // Center, Size
    class Groundcover : public Resource::GenericResourceManager<GroundcoverChunkId>,
                        public Terrain::QuadTreeWorld::ChunkManager
    {
    public:
        Groundcover(Resource::SceneManager* sceneManager, float density, float viewDistance,
            const MWWorld::GroundcoverStore& store);
        ~Groundcover();

        osg::ref_ptr<osg::Node> getChunk(float size, const osg::Vec2f& center, unsigned char lod, unsigned int lodFlags,
            bool activeGrid, const osg::Vec3f& viewPoint, bool compile) override;

        unsigned int getNodeMask() override;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, bool coarseChunkOcclusion);

        struct GroundcoverEntry
        {
            ESM::RefNum mRefNum;
            ESM::Position mPos;
            float mScale;

            GroundcoverEntry(const ESM::CellRef& ref)
                : mRefNum(ref.mRefNum)
                , mPos(ref.mPos)
                , mScale(ref.mScale)
            {
            }
        };

        using InstanceMap = std::map<VFS::Path::Normalized, std::vector<GroundcoverEntry>, std::less<>>;

        // Backend-neutral callers reuse the exact winning-file merge, density,
        // and border selection used by the established OpenGL renderer.
        [[nodiscard]] InstanceMap collectInstances(float size, const osg::Vec2f& center) const;

    private:
        Resource::SceneManager* mSceneManager;
        osg::ref_ptr<SceneUtil::OcclusionCuller> mOcclusionCuller;
        bool mV35CoarseChunkOcclusion = false;
        float mDensity;
        osg::ref_ptr<osg::StateSet> mStateset;
        osg::ref_ptr<osg::Program> mProgramTemplate;
        const MWWorld::GroundcoverStore& mGroundcoverStore;
        std::atomic_uint64_t mV314CompileQueued{ 0 };

        osg::ref_ptr<osg::Node> createChunk(InstanceMap& instances, const osg::Vec2f& center);
        void collectInstances(InstanceMap& instances, float size, const osg::Vec2f& center) const;
    };
}

#endif
