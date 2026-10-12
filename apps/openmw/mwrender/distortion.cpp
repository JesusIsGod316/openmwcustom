#include "distortion.hpp"

#include <osg/FrameBufferObject>
#include <osgUtil/RenderStage>

namespace MWRender
{
    void DistortionCallback::drawImplementation(
        osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous)
    {
        osg::State* state = renderInfo.getState();
        unsigned frameId = state->getFrameStamp()->getFrameNumber() % 2;

        osg::ref_ptr<osg::FrameBufferObject> primary = mOwnedPrimary;
        if (!primary && mPrimaryProvider) primary = mPrimaryProvider(renderInfo);
        if (!primary || bin->getStage()->getFrameBufferObject() != primary)
            return;

        osg::ref_ptr<osg::FrameBufferObject> distortion = mOwnedDistortion ? mOwnedDistortion.get() : mFBO[frameId].get();
        osg::ref_ptr<osg::FrameBufferObject> original = mOwnedOriginal ? mOwnedOriginal.get() : mOriginalFBO[frameId].get();
        if (!distortion || !original) return;
        distortion->apply(*state);

        const osg::Texture* tex
            = distortion->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0).getTexture();

        glViewport(0, 0, tex->getTextureWidth(), tex->getTextureHeight());
        glClearColor(0.0, 0.0, 0.0, 1.0);
        glColorMask(true, true, true, true);
        state->haveAppliedAttribute(osg::StateAttribute::Type::COLORMASK);
        glClear(GL_COLOR_BUFFER_BIT);

        bin->drawImplementation(renderInfo, previous);

        tex = original->getAttachment(osg::FrameBufferObject::BufferComponent::COLOR_BUFFER0).getTexture();
        glViewport(0, 0, tex->getTextureWidth(), tex->getTextureHeight());
        original->apply(*state);
    }
}
