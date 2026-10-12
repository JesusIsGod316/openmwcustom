#include "terraindrawable.hpp"


#include <algorithm>
#include <osg/ClusterCullingCallback>
#include <osgUtil/CullVisitor>

#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/drawphasetrace.hpp>
#include <components/resource/texturestreaming.hpp>
#include <components/resource/assetpolicy.hpp>

#include "compositemaprenderer.hpp"

namespace Terrain
{
    namespace
    {
        struct SampledPass
        {
            const TerrainDrawable* drawable = nullptr;
            const osg::StateSet* pass = nullptr;
        };
        thread_local SampledPass sSampledPass;

        class PassScope
        {
        public:
            PassScope(const TerrainDrawable& drawable, const osg::StateSet* pass)
                : mPrevious(sSampledPass)
            {
                sSampledPass = { &drawable, pass };
            }
            ~PassScope() { sSampledPass = mPrevious; }
        private:
            SampledPass mPrevious;
        };

        class PassDrawable final : public osg::Geometry
        {
        public:
            PassDrawable(TerrainDrawable& drawable, const osg::StateSet* pass)
                : osg::Geometry(drawable, osg::CopyOp::SHALLOW_COPY)
                , mDrawable(&drawable), mPass(pass)
            {
                // The original manages its callback, VBO/VAO and display lists.
                // This leaf contributes only retained ownership and exact pass.
                // Shipped OSG's OSGUTIL_RENDERBACKEND_USE_REF_PTR RenderLeaf
                // retains this adapter until submission reset; its strong
                // original reference preserves geometry through that draw.
                setDrawCallback(nullptr);
                setUseDisplayList(false);
                // Geometry's override may assign buffer objects to shared
                // arrays, even when disabling VBOs. Do not mutate source data.
                osg::Drawable::setUseVertexBufferObjects(false);
                setUseVertexArrayObject(false);
            }
            void drawImplementation(osg::RenderInfo& info) const override { mDrawable->drawPass(info, mPass); }
            void compileGLObjects(osg::RenderInfo& info) const override { mDrawable->compileGeometryGLObjects(info); }
            void releaseGLObjects(osg::State* state) const override { mDrawable->releaseGLObjects(state); }
            void resizeGLObjectBuffers(unsigned size) override { mDrawable->resizeGLObjectBuffers(size); }
        private:
            osg::ref_ptr<TerrainDrawable> mDrawable;
            osg::ref_ptr<const osg::StateSet> mPass;
        };
    }

    TerrainDrawable::TerrainDrawable() {}

    TerrainDrawable::~TerrainDrawable() = default;

    TerrainDrawable::TerrainDrawable(const TerrainDrawable& copy, const osg::CopyOp& copyop)
        : osg::Geometry(copy, copyop)
        , mPasses(copy.mPasses)
        , mLightListCallback(copy.mLightListCallback)
    {
    }

    void TerrainDrawable::accept(osg::NodeVisitor& nv)
    {
        if (nv.getVisitorType() != osg::NodeVisitor::CULL_VISITOR)
        {
            osg::Geometry::accept(nv);
        }
        else if (nv.validNodeMask(*this))
        {
            nv.pushOntoNodePath(this);
            cull(static_cast<osgUtil::CullVisitor*>(&nv));
            nv.popFromNodePath();
        }
    }

    inline float distance(const osg::Vec3& coord, const osg::Matrix& matrix)
    {
        return -(coord[0] * static_cast<float>(matrix(0, 2)) + coord[1] * static_cast<float>(matrix(1, 2))
            + coord[2] * static_cast<float>(matrix(2, 2)) + static_cast<float>(matrix(3, 2)));
    }

    // canot use ClusterCullingCallback::cull: viewpoint != eyepoint
    //  !osgfixpotential!
    bool clusterCull(osg::ClusterCullingCallback* cb, const osg::Vec3f& eyePoint, bool shadowcam)
    {
        const float deviation = cb->getDeviation();
        const osg::Vec3& controlPoint = cb->getControlPoint();
        osg::Vec3 normal = cb->getNormal();
        if (shadowcam)
            normal = normal * -1; // inverting for shadowcam frontfaceculing
        if (deviation <= -1.0f)
            return false;
        const osg::Vec3 eyeControlPoint = eyePoint - controlPoint;
        const float radius = eyeControlPoint.length();
        if (radius < cb->getRadius())
            return false;
        return (eyeControlPoint * normal) / radius < deviation;
    }

    void TerrainDrawable::cull(osgUtil::CullVisitor* cv)
    {
        SceneUtil::DrawPhaseTrace::SubmissionScope submission(*cv, *this);
        const osg::BoundingBox& bb = getBoundingBox();

        if (_cullingActive && cv->isCulled(getBoundingBox()))
            return;

        bool shadowcam = cv->getCurrentCamera()->getName() == "ShadowCamera";

        if (cv->getCullingMode() & osg::CullStack::CLUSTER_CULLING
            && clusterCull(mClusterCullingCallback, cv->getEyePoint(), shadowcam))
            return;

        osg::RefMatrix& matrix = *cv->getModelViewMatrix();

        if (cv->getComputeNearFarMode() != osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR && bb.valid())
        {
            if (!cv->updateCalculatedNearFar(matrix, *this, false))
                return;
        }

        float depth = bb.valid() ? distance(bb.center(), matrix) : 0.0f;
        if (osg::isNaN(depth))
            return;

        if (shadowcam)
        {
            cv->addDrawableAndDepth(this, &matrix, depth);
            return;
        }

        if (mCompositeMap && mCompositeMapRenderer)
        {
            mCompositeMapRenderer->setImmediate(mCompositeMap);
            mCompositeMapRenderer = nullptr;
        }

        bool pushedLight = mLightListCallback && mLightListCallback->pushLightState(this, cv);

        osg::StateSet* stateset = getStateSet();
        if (stateset)
            cv->pushStateSet(stateset);

        for (std::size_t i = 0; i < mPasses.size(); ++i)
        {
            cv->pushStateSet(mPasses[i]);
            if (Resource::AssetPolicy::enabled(Resource::AssetPolicy::TexturePreparation
                    | Resource::AssetPolicy::TextureStreaming))
            {
                osg::ref_ptr<osg::Geometry> drawable;
                {
                    std::lock_guard lock(mPassDrawableMutex);
                    mPassDrawables.resize(mPasses.size());
                    if (!mPassDrawables[i].lock(drawable))
                    {
                        drawable = new PassDrawable(*this, mPasses[i]);
                        mPassDrawables[i] = drawable;
                    }
                }
                cv->addDrawableAndDepth(drawable, &matrix, depth);
            }
            else
                cv->addDrawableAndDepth(this, &matrix, depth);
            cv->popStateSet();
        }

        if (stateset)
            cv->popStateSet();
        if (pushedLight)
            cv->popStateSet();
    }

    void TerrainDrawable::createClusterCullingCallback()
    {
        mClusterCullingCallback = new osg::ClusterCullingCallback(this);
    }

    void TerrainDrawable::setPasses(const TerrainDrawable::PassVector& passes)
    {
        mPasses = passes;
        std::lock_guard lock(mPassDrawableMutex);
        mPassDrawables.clear();
    }

    void TerrainDrawable::setLightListCallback(SceneUtil::LightListCallback* lightListCallback)
    {
        mLightListCallback = lightListCallback;
    }

    void TerrainDrawable::setupWaterBoundingBox(float waterheight, float margin)
    {
        osg::Vec3Array* vertices = static_cast<osg::Vec3Array*>(getVertexArray());
        for (unsigned int i = 0; i < vertices->size(); ++i)
        {
            const osg::Vec3f& vertex = (*vertices)[i];
            if (vertex.z() <= waterheight)
                mWaterBoundingBox.expandBy(vertex);
        }
        if (mWaterBoundingBox.valid())
        {
            const osg::BoundingBox& bb = getBoundingBox();
            mWaterBoundingBox.xMin() = std::max(bb.xMin(), mWaterBoundingBox.xMin() - margin);
            mWaterBoundingBox.yMin() = std::max(bb.yMin(), mWaterBoundingBox.yMin() - margin);
            mWaterBoundingBox.xMax() = std::min(bb.xMax(), mWaterBoundingBox.xMax() + margin);
            mWaterBoundingBox.xMax() = std::min(bb.xMax(), mWaterBoundingBox.xMax() + margin);
        }
    }

    void TerrainDrawable::compileGeometryGLObjects(osg::RenderInfo& renderInfo) const
    {
        osg::Geometry::compileGLObjects(renderInfo);
    }

    void TerrainDrawable::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        osg::Geometry::drawImplementation(renderInfo);
        if(auto* state=renderInfo.getState();state && sSampledPass.drawable==this && sSampledPass.pass
            && std::any_of(getPrimitiveSetList().begin(),getPrimitiveSetList().end(),
            [](const auto& primitive) { return primitive && primitive->getNumIndices()>=3; }))
            Resource::TextureStreaming::recordDrawUse(*state,sSampledPass.pass);
    }

    void TerrainDrawable::drawPass(osg::RenderInfo& renderInfo, const osg::StateSet* pass) const
    {
        PassScope scope(*this, pass);
        draw(renderInfo);
    }

    void TerrainDrawable::compileGLObjects(osg::RenderInfo& renderInfo) const
    {
        for (PassVector::const_iterator it = mPasses.begin(); it != mPasses.end(); ++it)
        {
            osg::StateSet* stateset = *it;
            stateset->compileGLObjects(*renderInfo.getState());
        }

        compileGeometryGLObjects(renderInfo);
    }

}
