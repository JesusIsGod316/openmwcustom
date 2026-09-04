#ifndef GAME_RENDER_OBJECTS_H
#define GAME_RENDER_OBJECTS_H

#include <cstddef>
#include <map>
#include <set>
#include <string>

#include <osg/Object>
#include <osg/ref_ptr>

#include "../mwworld/ptr.hpp"
#include "occlusionculling.hpp"

namespace osg
{
    class Group;
}

namespace Resource
{
    class ResourceSystem;
}

namespace MWWorld
{
    class CellStore;
}

namespace SceneUtil
{
    class OcclusionCuller;
    class PositionAttitudeTransform;
    class UnrefQueue;
}

namespace MWRender
{

    class Animation;

    class PtrHolder : public osg::Object
    {
    public:
        PtrHolder(const MWWorld::Ptr& ptr)
            : mPtr(ptr)
        {
        }

        PtrHolder() {}

        PtrHolder(const PtrHolder& copy, const osg::CopyOp& copyop)
            : mPtr(copy.mPtr)
        {
        }

        META_Object(MWRender, PtrHolder)

        MWWorld::Ptr mPtr;
    };

    class Objects
    {
        using PtrAnimationMap = std::map<const MWWorld::LiveCellRefBase*, osg::ref_ptr<Animation>>;

        typedef std::map<const MWWorld::CellStore*, osg::ref_ptr<osg::Group>> CellMap;

        struct HibernatedObject
        {
            osg::ref_ptr<Animation> mAnimation;
            osg::ref_ptr<SceneUtil::PositionAttitudeTransform> mBaseNode;
        };
        using HibernatedObjectMap = std::map<const MWWorld::LiveCellRefBase*, HibernatedObject>;
        struct HibernatedCell
        {
            osg::ref_ptr<osg::Group> mRoot;
            HibernatedObjectMap mObjects;
        };
        using HibernatedCellMap = std::map<const MWWorld::CellStore*, HibernatedCell>;

        CellMap mCellSceneNodes;
        PtrAnimationMap mObjects;
        HibernatedCellMap mHibernatedCells;
        std::set<const MWWorld::LiveCellRefBase*> mRestoredObjects;
        osg::ref_ptr<osg::Group> mRootNode;
        Resource::ResourceSystem* mResourceSystem;
        SceneUtil::UnrefQueue& mUnrefQueue;

        void insertBegin(const MWWorld::Ptr& ptr);

    public:
        Objects(Resource::ResourceSystem* resourceSystem, const osg::ref_ptr<osg::Group>& rootNode,
            SceneUtil::UnrefQueue& unrefQueue);
        ~Objects();

        /// @param allowLight If false, no lights will be created, and particles systems will be removed.
        void insertModel(const MWWorld::Ptr& ptr, const std::string& model, bool allowLight = true);

        void insertNPC(const MWWorld::Ptr& ptr);
        void insertCreature(const MWWorld::Ptr& ptr, const std::string& model, bool weaponsShields);

        Animation* getAnimation(const MWWorld::Ptr& ptr);
        const Animation* getAnimation(const MWWorld::ConstPtr& ptr) const;

        bool removeObject(const MWWorld::Ptr& ptr);
        ///< \return found?

        void removeCell(const MWWorld::CellStore* store);

        // V3.2: preserve only renderer-safe static state. Actors, doors and
        // objects using the animation path continue through normal destruction.
        std::size_t hibernateCell(const MWWorld::CellStore* store);
        std::size_t restoreHibernatedCell(const MWWorld::CellStore* store);
        bool consumeRestoredObject(const MWWorld::Ptr& ptr);
        void clearHibernatedCells();

        /// Updates containing cell for object rendering data
        void updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur);

        void setOcclusionCuller(SceneUtil::OcclusionCuller* culler, float occluderMinRadius, float occluderMaxRadius,
            float occluderShrinkFactor, int occluderMeshResolution, int occluderMaxMeshResolution,
            float occluderInsideThreshold, float occluderMaxDistance, bool enableStaticOccluders,
            bool v34BroadenOcclusion, unsigned int maxTriangles, OcclusionStorage* storage = nullptr);

    private:
        SceneUtil::OcclusionCuller* mOcclusionCuller = nullptr;
        float mOccluderMinRadius = 300.0f;
        float mOccluderMaxRadius = 5000.0f;
        float mOccluderShrinkFactor = 0.5f;
        int mOccluderMeshResolution = 8;
        int mOccluderMaxMeshResolution = 24;
        float mOccluderInsideThreshold = 1.0f;
        float mOccluderMaxDistance = 6144.0f;
        bool mEnableStaticOccluders = true;
        bool mV34BroadenOcclusion = false;
        unsigned int mMaxTriangles = 30000;
        OcclusionStorage* mOcclusionStorage = nullptr;

        void operator=(const Objects&);
        Objects(const Objects&);
    };
}
#endif
