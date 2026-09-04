#ifndef OPENMW_MWRENDER_NISSCALER_H
#define OPENMW_MWRENDER_NISSCALER_H

#include <osg/Program>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/buffered_value>

namespace osg
{
    class BindImageTexture;
    class GLExtensions;
    class RenderInfo;
    class State;
}

namespace MWRender
{
    class NisScaler
    {
    public:
        NisScaler();
        ~NisScaler();

        // Returns a native-resolution NIS output texture on success. Returns
        // nullptr when NIS is unavailable/invalid so the caller can explicitly
        // fall back to the ordinary bilinear presentation path.
        osg::Texture* dispatch(osg::RenderInfo& renderInfo, osg::Texture* input, int inputWidth, int inputHeight,
            int outputWidth, int outputHeight, float sharpness) const;

        void resizeGLObjectBuffers(unsigned int maxSize);

    private:
        bool ensureProgram(osg::State& state, osg::GLExtensions* ext) const;
        void resizeOutput(int width, int height) const;

        mutable osg::ref_ptr<osg::Program> mProgram;
        mutable osg::ref_ptr<osg::StateSet> mStateSet;
        mutable osg::ref_ptr<osg::Texture2D> mOutputTexture;
        mutable osg::ref_ptr<osg::Texture2D> mCoefScaler;
        mutable osg::ref_ptr<osg::Texture2D> mCoefUsm;
        mutable osg::ref_ptr<osg::BindImageTexture> mOutputBinding;
        mutable osg::buffered_value<int> mProgramStatus; // 0 unknown, 1 ready, -1 unavailable/failed
        mutable osg::buffered_value<unsigned char> mLoggedActive;
        mutable int mOutputWidth = 0;
        mutable int mOutputHeight = 0;
    };
}

#endif
