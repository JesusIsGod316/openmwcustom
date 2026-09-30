#ifndef OPENMW_MWRENDER_PINGPONGCANVAS_H
#define OPENMW_MWRENDER_PINGPONGCANVAS_H

#include <array>
#include <memory>
#include <optional>
#include <limits>

#include <osg/FrameBufferObject>
#include <osg/Geometry>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/buffered_value>

#include <components/fx/technique.hpp>

#include "luminancecalculator.hpp"
#include "nisscaler.hpp"
#include "temporalmotion.hpp"

namespace Shader
{
    class ShaderManager;
}

namespace osgUtil { class CullVisitor; }

namespace MWRender
{
    struct TemporalCanvasOwner;
    struct PostFxTargetGeneration
    {
        std::vector<Fx::Types::RenderTarget> attachments;
        // Only the verified single graphics context's serialized draw owner
        // reads/writes this field. Cull never tests initialization state.
        unsigned initializedContext = std::numeric_limits<unsigned>::max();
    };
    class PingPongCanvas : public osg::Geometry
    {
    public:
        PingPongCanvas(
            Shader::ShaderManager& shaderManager, const std::shared_ptr<LuminanceCalculator>& luminanceCalculator);

        void accept(osg::NodeVisitor& visitor) override;
        // A shared two-SceneView owner is installed only after the main
        // renderer's actual CullVisitor identity/lifecycle has been verified.
        void setTemporalOwner(std::shared_ptr<TemporalCanvasOwner> owner) { mTemporalOwner = std::move(owner); }
        void setTemporalOwnershipAvailable(bool value, std::string reason = "unverified_owner")
        {
            mTemporalOwnershipAvailable = value;
            mOwnershipFallback = std::move(reason);
        }
        void setOwnedFxState(osg::StateSet* value) { mOwnedFxState = value; }
        static std::shared_ptr<TemporalCanvasOwner> createTemporalOwner();
        static void finalizeTemporalOwner(const std::shared_ptr<TemporalCanvasOwner>& owner,
            osgUtil::CullVisitor* visitor, osg::StateSet* fxState);

        void drawGeometry(osg::RenderInfo& renderInfo) const;

        void drawImplementation(osg::RenderInfo& renderInfo) const override;

        void resizeGLObjectBuffers(unsigned int maxSize) override;
        void releaseGLObjects(osg::State* state = nullptr) const override;

        void setTemporalMotion(std::shared_ptr<TemporalMotion> motion, osg::Program* debugProgram);
        void setTemporalCamera(TemporalCamera camera) { mTemporalCamera = std::move(camera); }

        void dirty() { mDirty = true; }

        void setDirtyAttachments(const std::vector<Fx::Types::RenderTarget>& attachments)
        {
            mDeclaredAttachments = attachments;
            mDirtyAttachments = attachments;
        }

        const Fx::DispatchArray& getPasses() { return mPasses; }

        void setPasses(Fx::DispatchArray&& passes, std::shared_ptr<PostFxTargetGeneration> generation = {});
        void setTargetGenerationSingleContext(bool qualified) { mTargetGenerationSingleContext = qualified; }

        void setMask(bool underwater, bool exterior);

        void setTextureScene(osg::ref_ptr<osg::Texture> tex) { mTextureScene = tex; }

        void setTextureDepth(osg::ref_ptr<osg::Texture> tex) { mTextureDepth = tex; }

        void setTextureNormals(osg::ref_ptr<osg::Texture> tex) { mTextureNormals = tex; }

        void setTextureDistortion(osg::ref_ptr<osg::Texture> tex) { mTextureDistortion = tex; }

        void setCalculateAvgLum(bool enabled) { mAvgLum = enabled; }

        void setPostProcessing(bool enabled) { mPostprocessing = enabled; }

        const osg::ref_ptr<osg::Texture>& getSceneTexture(size_t frameId) const { return mTextureScene; }

    private:
        PingPongCanvas(const PingPongCanvas& source);
        void copyFrameInputs(const PingPongCanvas& source);
        std::shared_ptr<TemporalCanvasOwner> mTemporalOwner;
        bool mTemporalOwnershipAvailable = false;
        bool mOwnedSubmission = false;
        std::string mOwnershipFallback = "unverified_owner";
        osg::ref_ptr<osg::StateSet> mOwnedFxState;
        osg::ref_ptr<osg::StateSet> mDrawFxState;
        osg::ref_ptr<osg::StateSet> mOwnedFxBindings = new osg::StateSet;
        std::vector<std::string> mOwnedTechniqueNames;
        std::shared_ptr<PostFxTargetGeneration> mTargetGeneration;
        bool mOwnedNis = false;
        float mOwnedNisSharpness = 0.0f;
        std::shared_ptr<TemporalMotion> mTemporalMotion;
        TemporalCamera mTemporalCamera;
        osg::ref_ptr<osg::StateSet> mMotionViewState;
        bool mAvgLum = false;
        bool mPostprocessing = false;

        Fx::DispatchArray mPasses;
        Fx::FlagsType mMask = 0;

        osg::ref_ptr<osg::Program> mFallbackProgram;
        osg::ref_ptr<osg::Program> mMultiviewResolveProgram;
        osg::ref_ptr<osg::StateSet> mFallbackStateSet;
        osg::ref_ptr<osg::StateSet> mMultiviewResolveStateSet;

        osg::ref_ptr<osg::Texture> mTextureScene;
        osg::ref_ptr<osg::Texture> mTextureDepth;
        osg::ref_ptr<osg::Texture> mTextureNormals;
        osg::ref_ptr<osg::Texture> mTextureDistortion;

        mutable bool mDirty = false;
        mutable std::vector<Fx::Types::RenderTarget> mDirtyAttachments;
        std::vector<Fx::Types::RenderTarget> mDeclaredAttachments;
        bool mTargetGenerationSingleContext = false;
        mutable osg::ref_ptr<osg::Viewport> mRenderViewport;
        mutable osg::ref_ptr<osg::FrameBufferObject> mMultiviewResolveFramebuffer;
        mutable osg::ref_ptr<osg::FrameBufferObject> mDestinationFBO;
        mutable std::array<osg::ref_ptr<osg::FrameBufferObject>, 3> mFbos;
        mutable std::shared_ptr<LuminanceCalculator> mLuminanceCalculator;
        mutable std::shared_ptr<NisScaler> mNisScaler;
        mutable osg::buffered_object<osg::State::UniformMap> mEmptyUniformStacks;
    };
}

#endif
