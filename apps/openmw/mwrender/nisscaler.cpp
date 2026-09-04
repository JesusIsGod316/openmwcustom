#include "nisscaler.hpp"

#include <algorithm>
#include <cstring>
#include <string>

#include <osg/BindImageTexture>
#include <osg/GLExtensions>
#include <osg/Image>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/State>
#include <osg/Uniform>

#include <components/debug/debuglog.hpp>

#include "v318_nis_config.hpp"
#include "v318_nis_shader.hpp"

namespace MWRender
{
    namespace
    {
        osg::ref_ptr<osg::Texture2D> makeCoefficientTexture(const float* coefficients)
        {
            osg::ref_ptr<osg::Image> image = new osg::Image;
            image->allocateImage(2, 64, 1, GL_RGBA, GL_FLOAT);
            std::memcpy(image->data(), coefficients, 2u * 64u * 4u * sizeof(float));

            osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
            texture->setInternalFormat(GL_RGBA32F_ARB);
            texture->setSourceFormat(GL_RGBA);
            texture->setSourceType(GL_FLOAT);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
            texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
            texture->setResizeNonPowerOfTwoHint(false);
            return texture;
        }

        void addFloatUniform(osg::StateSet* stateSet, const char* name)
        {
            stateSet->addUniform(new osg::Uniform(name, 0.f));
        }

        void addUIntUniform(osg::StateSet* stateSet, const char* name)
        {
            osg::ref_ptr<osg::Uniform> uniform = new osg::Uniform(osg::Uniform::UNSIGNED_INT, name);
            uniform->set(0u);
            stateSet->addUniform(uniform);
        }

        void setFloat(osg::StateSet* stateSet, const char* name, float value)
        {
            if (auto* uniform = stateSet->getUniform(name))
                uniform->set(value);
        }

        void setUInt(osg::StateSet* stateSet, const char* name, std::uint32_t value)
        {
            if (auto* uniform = stateSet->getUniform(name))
                uniform->set(static_cast<unsigned int>(value));
        }
    }

    NisScaler::NisScaler()
        : mProgram(new osg::Program)
        , mStateSet(new osg::StateSet)
        , mOutputTexture(new osg::Texture2D)
        , mCoefScaler(makeCoefficientTexture(&coef_scale[0][0]))
        , mCoefUsm(makeCoefficientTexture(&coef_usm[0][0]))
    {
        osg::ref_ptr<osg::Shader> shader = new osg::Shader(osg::Shader::COMPUTE, V318Nis::computeShader());
        shader->setName("openmw-v318-nis-1.0.3");
        mProgram->setName("openmw-v318-nis-1.0.3");
        mProgram->addShader(shader);
        mStateSet->setAttributeAndModes(mProgram, osg::StateAttribute::ON);

        mOutputTexture->setSourceFormat(GL_RGBA);
        mOutputTexture->setSourceType(GL_UNSIGNED_BYTE);
        mOutputTexture->setInternalFormat(GL_RGBA8);
        mOutputTexture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        mOutputTexture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        mOutputTexture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        mOutputTexture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        mOutputTexture->setResizeNonPowerOfTwoHint(false);

        mOutputBinding = new osg::BindImageTexture(
            0, mOutputTexture, osg::BindImageTexture::WRITE_ONLY, GL_RGBA8, 0, false, 0);
        mStateSet->setAttributeAndModes(mOutputBinding, osg::StateAttribute::ON);

        mStateSet->setTextureAttribute(1, mCoefScaler);
        mStateSet->setTextureAttribute(2, mCoefUsm);
        mStateSet->addUniform(new osg::Uniform("in_texture", 0));
        mStateSet->addUniform(new osg::Uniform("coef_scaler", 1));
        mStateSet->addUniform(new osg::Uniform("coef_usm", 2));

        for (const char* name : { "kDetectRatio", "kDetectThres", "kMinContrastRatio", "kRatioNorm",
                 "kContrastBoost", "kEps", "kSharpStartY", "kSharpScaleY", "kSharpStrengthMin",
                 "kSharpStrengthScale", "kSharpLimitMin", "kSharpLimitScale", "kScaleX", "kScaleY",
                 "kDstNormX", "kDstNormY", "kSrcNormX", "kSrcNormY" })
            addFloatUniform(mStateSet, name);

        for (const char* name : { "kInputViewportOriginX", "kInputViewportOriginY", "kInputViewportWidth",
                 "kInputViewportHeight", "kOutputViewportOriginX", "kOutputViewportOriginY",
                 "kOutputViewportWidth", "kOutputViewportHeight" })
            addUIntUniform(mStateSet, name);
    }

    NisScaler::~NisScaler() = default;

    void NisScaler::resizeGLObjectBuffers(unsigned int maxSize)
    {
        mProgramStatus.resize(maxSize);
        mLoggedActive.resize(maxSize);
        mProgram->resizeGLObjectBuffers(maxSize);
        mStateSet->resizeGLObjectBuffers(maxSize);
        mOutputTexture->resizeGLObjectBuffers(maxSize);
        mCoefScaler->resizeGLObjectBuffers(maxSize);
        mCoefUsm->resizeGLObjectBuffers(maxSize);
    }

    bool NisScaler::ensureProgram(osg::State& state, osg::GLExtensions* ext) const
    {
        const unsigned int cid = state.getContextID();
        int& status = mProgramStatus[cid];
        if (status != 0)
            return status > 0;

        if (!ext || ext->glslLanguageVersion < 4.30f || !ext->glDispatchCompute || !ext->glBindImageTexture
            || !ext->glMemoryBarrier)
        {
            status = -1;
            Log(Debug::Warning) << "V3.18 NIS unavailable: OpenGL 4.3 compute/image-load-store support is missing; "
                                << "falling back to bilinear upscale.";
            return false;
        }

        mProgram->compileGLObjects(state);
        auto* pcp = mProgram->getPCP(state);
        if (!pcp || !pcp->isLinked())
        {
            std::string info;
            mProgram->getGlProgramInfoLog(cid, info);
            status = -1;
            Log(Debug::Warning) << "V3.18 NIS compute shader failed to link; falling back to bilinear upscale. "
                                << info;
            return false;
        }

        status = 1;
        return true;
    }

    void NisScaler::resizeOutput(int width, int height) const
    {
        if (mOutputWidth == width && mOutputHeight == height)
            return;
        mOutputWidth = width;
        mOutputHeight = height;
        mOutputTexture->setTextureSize(width, height);
        mOutputTexture->dirtyTextureObject();
    }

    osg::Texture* NisScaler::dispatch(osg::RenderInfo& renderInfo, osg::Texture* input, int inputWidth,
        int inputHeight, int outputWidth, int outputHeight, float sharpness) const
    {
        if (!input || inputWidth <= 0 || inputHeight <= 0 || outputWidth <= 0 || outputHeight <= 0)
            return nullptr;
        if (inputWidth == outputWidth && inputHeight == outputHeight)
            return input;

        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();
        if (!ensureProgram(state, ext))
            return nullptr;

        NISConfig config{};
        const bool valid = NVScalerUpdateConfig(config, std::clamp(sharpness, 0.f, 1.f), 0, 0,
            static_cast<std::uint32_t>(inputWidth), static_cast<std::uint32_t>(inputHeight),
            static_cast<std::uint32_t>(inputWidth), static_cast<std::uint32_t>(inputHeight), 0, 0,
            static_cast<std::uint32_t>(outputWidth), static_cast<std::uint32_t>(outputHeight),
            static_cast<std::uint32_t>(outputWidth), static_cast<std::uint32_t>(outputHeight), NISHDRMode::None);
        if (!valid)
        {
            Log(Debug::Warning) << "V3.18 NIS rejected render dimensions " << inputWidth << 'x' << inputHeight << " -> "
                                << outputWidth << 'x' << outputHeight << "; falling back to bilinear upscale.";
            return nullptr;
        }

        resizeOutput(outputWidth, outputHeight);
        mStateSet->setTextureAttribute(0, input);

        setFloat(mStateSet, "kDetectRatio", config.kDetectRatio);
        setFloat(mStateSet, "kDetectThres", config.kDetectThres);
        setFloat(mStateSet, "kMinContrastRatio", config.kMinContrastRatio);
        setFloat(mStateSet, "kRatioNorm", config.kRatioNorm);
        setFloat(mStateSet, "kContrastBoost", config.kContrastBoost);
        setFloat(mStateSet, "kEps", config.kEps);
        setFloat(mStateSet, "kSharpStartY", config.kSharpStartY);
        setFloat(mStateSet, "kSharpScaleY", config.kSharpScaleY);
        setFloat(mStateSet, "kSharpStrengthMin", config.kSharpStrengthMin);
        setFloat(mStateSet, "kSharpStrengthScale", config.kSharpStrengthScale);
        setFloat(mStateSet, "kSharpLimitMin", config.kSharpLimitMin);
        setFloat(mStateSet, "kSharpLimitScale", config.kSharpLimitScale);
        setFloat(mStateSet, "kScaleX", config.kScaleX);
        setFloat(mStateSet, "kScaleY", config.kScaleY);
        setFloat(mStateSet, "kDstNormX", config.kDstNormX);
        setFloat(mStateSet, "kDstNormY", config.kDstNormY);
        setFloat(mStateSet, "kSrcNormX", config.kSrcNormX);
        setFloat(mStateSet, "kSrcNormY", config.kSrcNormY);
        setUInt(mStateSet, "kInputViewportOriginX", config.kInputViewportOriginX);
        setUInt(mStateSet, "kInputViewportOriginY", config.kInputViewportOriginY);
        setUInt(mStateSet, "kInputViewportWidth", config.kInputViewportWidth);
        setUInt(mStateSet, "kInputViewportHeight", config.kInputViewportHeight);
        setUInt(mStateSet, "kOutputViewportOriginX", config.kOutputViewportOriginX);
        setUInt(mStateSet, "kOutputViewportOriginY", config.kOutputViewportOriginY);
        setUInt(mStateSet, "kOutputViewportWidth", config.kOutputViewportWidth);
        setUInt(mStateSet, "kOutputViewportHeight", config.kOutputViewportHeight);

        // Allocate/update the output texture before BindImageTexture applies. This
        // prevents osg::BindImageTexture from using the currently-active sampler
        // unit to lazily allocate the image and accidentally replacing a coefficient
        // sampler binding.
        state.setActiveTextureUnit(3);
        state.applyTextureAttribute(3, mOutputTexture);
        state.setActiveTextureUnit(0);

        state.pushStateSet(mStateSet);
        state.apply();

        const GLuint groupsX = static_cast<GLuint>((outputWidth + 31) / 32);
        const GLuint groupsY = static_cast<GLuint>((outputHeight + 23) / 24);
        ext->glDispatchCompute(groupsX, groupsY, 1);
        ext->glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

        state.popStateSet();
        state.apply();

        const unsigned int cid = state.getContextID();
        if (!mLoggedActive[cid])
        {
            mLoggedActive[cid] = true;
            Log(Debug::Info) << "V3.18 NVIDIA Image Scaling active: " << inputWidth << 'x' << inputHeight << " -> "
                             << outputWidth << 'x' << outputHeight << ", sharpness=" << sharpness
                             << ", SDK=1.0.3, source=" << "35e13ba316c98eeecf16f37eae70ce88019911f6";
        }

        return mOutputTexture;
    }
}
