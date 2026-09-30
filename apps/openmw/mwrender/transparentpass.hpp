#ifndef OPENMW_MWRENDER_TRANSPARENTPASS_H
#define OPENMW_MWRENDER_TRANSPARENTPASS_H

#include <array>
#include <memory>

#include <osg/FrameBufferObject>
#include <osg/StateSet>

#include <osgUtil/RenderBin>

namespace Shader
{
    class ShaderManager;
}

namespace Stereo
{
    class MultiviewFramebufferResolve;
}

namespace MWRender
{
    class TransparentDepthBinCallback : public osgUtil::RenderBin::DrawCallback
    {
    public:
        TransparentDepthBinCallback(Shader::ShaderManager& shaderManager, bool postPass);
        // The caller supplies a compiled state that stays immutable for the
        // callback lifetime; also permits isolated production GL validation.
        TransparentDepthBinCallback(osg::StateSet* immutableState, bool postPass);
        ~TransparentDepthBinCallback() override;
        osg::ref_ptr<TransparentDepthBinCallback> ownedFrame(unsigned frameId) const;

        void drawImplementation(
            osgUtil::RenderBin* bin, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*& previous) override;

        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mFbo;
        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mMsaaFbo;
        std::array<osg::ref_ptr<osg::FrameBufferObject>, 2> mOpaqueFbo;

        std::array<std::unique_ptr<Stereo::MultiviewFramebufferResolve>, 2> mMultiviewResolve;

    private:
        explicit TransparentDepthBinCallback(const TransparentDepthBinCallback& source, unsigned frameId);
        osg::ref_ptr<osg::StateSet> mStateSet;
        bool mPostPass;
    };

}

#endif
