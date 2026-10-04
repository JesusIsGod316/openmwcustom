#ifndef OPENMW_COMPONENTS_TERRAIN_DRAWABLE_H
#define OPENMW_COMPONENTS_TERRAIN_DRAWABLE_H

#include <osg/Geometry>
#include <osg/observer_ptr>

#include <mutex>

namespace osg
{
    class ClusterCullingCallback;
}

namespace osgUtil
{
    class CullVisitor;
}

namespace SceneUtil
{
    class LightListCallback;
}

namespace Terrain
{

    class CompositeMap;
    class CompositeMapRenderer;

    /**
     * Subclass of Geometry that supports built in multi-pass rendering and built in LightListCallback.
     */
    class TerrainDrawable : public osg::Geometry
    {
    public:
        osg::Object* cloneType() const override { return new TerrainDrawable(); }
        osg::Object* clone(const osg::CopyOp& copyop) const override { return new TerrainDrawable(*this, copyop); }
        bool isSameKindAs(const osg::Object* obj) const override
        {
            return dynamic_cast<const TerrainDrawable*>(obj) != nullptr;
        }
        const char* className() const override { return "TerrainDrawable"; }
        const char* libraryName() const override { return "Terrain"; }

        TerrainDrawable();
        ~TerrainDrawable(); // has to be defined in the cpp file because we only forward declared some members.
        TerrainDrawable(const TerrainDrawable& copy, const osg::CopyOp& copyop);

        void accept(osg::NodeVisitor& nv) override;
        void cull(osgUtil::CullVisitor* cv);

        typedef std::vector<osg::ref_ptr<osg::StateSet>> PassVector;
        void setPasses(const PassVector& passes);
        const PassVector& getPasses() const { return mPasses; }

        void setLightListCallback(SceneUtil::LightListCallback* lightListCallback);

        void createClusterCullingCallback();

        void compileGLObjects(osg::RenderInfo& renderInfo) const override;
        void drawImplementation(osg::RenderInfo& renderInfo) const override;
        // A rendered leaf carries its exact sampled pass; OSG does not push
        // that leaf StateSet onto State's stack before drawing.
        void drawPass(osg::RenderInfo& renderInfo, const osg::StateSet* pass) const;
        // Used by the optional phased ICO path after pass StateAttributes have
        // been compiled as independent operations.
        void compileGeometryGLObjects(osg::RenderInfo& renderInfo) const;

        void setupWaterBoundingBox(float waterheight, float margin);
        const osg::BoundingBox& getWaterBoundingBox() const { return mWaterBoundingBox; }

        void setCompositeMap(CompositeMap* map) { mCompositeMap = map; }
        CompositeMap* getCompositeMap() const { return mCompositeMap; }
        void setCompositeMapRenderer(CompositeMapRenderer* renderer) { mCompositeMapRenderer = renderer; }

    private:
        osg::BoundingBox mWaterBoundingBox;
        PassVector mPasses;
        // RenderLeaf owns each adapter, and each adapter retains this original.
        // Weak caching avoids a cycle and still permits concurrent scene culls.
        mutable std::mutex mPassDrawableMutex;
        mutable std::vector<osg::observer_ptr<osg::Geometry>> mPassDrawables;

        osg::ref_ptr<osg::ClusterCullingCallback> mClusterCullingCallback;

        osg::ref_ptr<SceneUtil::LightListCallback> mLightListCallback;
        osg::ref_ptr<CompositeMap> mCompositeMap;
        osg::ref_ptr<CompositeMapRenderer> mCompositeMapRenderer;
    };

}

#endif
