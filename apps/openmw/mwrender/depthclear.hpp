#ifndef OPENMW_MWRENDER_DEPTHCLEAR_H
#define OPENMW_MWRENDER_DEPTHCLEAR_H

#include <osg/ColorMask>
#include <osgUtil/RenderBin>
#include <osg/FrameBufferObject>
#include <array>
#include <components/sceneutil/depth.hpp>

namespace MWRender
{
    // First-person color/depth accumulation uses the same resource generation
    // as its acquired SceneView, even when presentation has no DYNAMIC leaf.
    class DepthClearCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        using Resources = std::array<osg::ref_ptr<osg::FrameBufferObject>,3>;
        using ResourceProvider = Resources (*)(osg::RenderInfo&);
        explicit DepthClearCallback(ResourceProvider provider = nullptr)
            : mDepth(new SceneUtil::AutoDepth)
            , mStateSet(new osg::StateSet)
            , mProvider(provider)
        {
            mDepth->setWriteMask(true);
            mStateSet->setAttributeAndModes(new osg::ColorMask(false, false, false, false), osg::StateAttribute::ON);
        }

        osg::ref_ptr<DepthClearCallback> ownedFrame(osg::FrameBufferObject* firstPerson,
            osg::FrameBufferObject* opaqueDepth, osg::FrameBufferObject* primary) const
        {
            osg::ref_ptr<DepthClearCallback> result = new DepthClearCallback;
            result->mDepth = mDepth;
            result->mStateSet = mStateSet;
            result->mOwned = true;
            result->mFirstPerson = firstPerson;
            result->mOpaqueDepth = opaqueDepth;
            result->mPrimary = primary;
            return result;
        }

        void drawImplementation(osgUtil::RenderBin* bin, osg::RenderInfo& info,
            osgUtil::RenderLeaf*& previous) override
        {
            osg::State* state = info.getState();
            osg::ref_ptr<osg::FrameBufferObject> firstPerson = mFirstPerson, opaqueDepth = mOpaqueDepth,
                primary = mPrimary;
            if (!mOwned)
            {
                if (!mProvider) return;
                const auto resources = mProvider(info);
                firstPerson = resources[0]; opaqueDepth = resources[1]; primary = resources[2];
            }
            if (!firstPerson || !opaqueDepth || !primary) return;
            state->applyAttribute(mDepth);
            firstPerson->apply(*state);
            glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            bin->drawImplementation(info, previous);
            primary->apply(*state);
            opaqueDepth->apply(*state);
            osg::ref_ptr<osg::StateSet> restore = bin->getStateSet();
            bin->setStateSet(mStateSet);
            bin->drawImplementation(info, previous);
            bin->setStateSet(restore);
            primary->apply(*state);
            state->checkGLErrors("after DepthClearCallback::drawImplementation");
        }

    private:
        osg::ref_ptr<osg::Depth> mDepth;
        osg::ref_ptr<osg::StateSet> mStateSet;
        bool mOwned = false;
        ResourceProvider mProvider;
        osg::ref_ptr<osg::FrameBufferObject> mFirstPerson, mOpaqueDepth, mPrimary;
    };
}
#endif
