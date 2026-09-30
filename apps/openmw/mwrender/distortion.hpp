#ifndef OPENMW_MWRENDER_DISTORTION_H
#define OPENMW_MWRENDER_DISTORTION_H
#include <array>

#include <osgUtil/RenderBin>
#include <osg/FrameBufferObject>

namespace osg
{
    class FrameBufferObject;
}

namespace MWRender
{
    class DistortionCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        using PrimaryProvider = osg::ref_ptr<osg::FrameBufferObject> (*)(osg::RenderInfo&);
        explicit DistortionCallback(PrimaryProvider provider = nullptr) : mPrimaryProvider(provider) {}
        void drawImplementation(
            osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override;

        void setFBO(const osg::ref_ptr<osg::FrameBufferObject>& fbo, size_t frameId) { mFBO[frameId] = fbo; }
        void setOriginalFBO(const osg::ref_ptr<osg::FrameBufferObject>& fbo, size_t frameId)
        {
            mOriginalFBO[frameId] = fbo;
        }
        osg::ref_ptr<DistortionCallback> ownedFrame(osg::FrameBufferObject* distortion,
            osg::FrameBufferObject* original, osg::FrameBufferObject* primary) const
        {
            osg::ref_ptr<DistortionCallback> result = new DistortionCallback;
            result->mOwnedDistortion = distortion;
            result->mOwnedOriginal = original;
            result->mOwnedPrimary = primary;
            return result;
        }

    private:
        osg::ref_ptr<osg::FrameBufferObject> mOwnedDistortion, mOwnedOriginal, mOwnedPrimary;
        PrimaryProvider mPrimaryProvider;
        std::array<osg::observer_ptr<osg::FrameBufferObject>, 2> mFBO;
        std::array<osg::observer_ptr<osg::FrameBufferObject>, 2> mOriginalFBO;
    };
}
#endif
