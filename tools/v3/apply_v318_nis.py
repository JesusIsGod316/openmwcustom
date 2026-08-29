import hashlib
import os
import urllib.request
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

NIS_COMMIT = "35e13ba316c98eeecf16f37eae70ce88019911f6"
NIS_SCALER_BLOB = "02f645c2c01b0235d340d25c6cfc913000f7cc1b"
NIS_CONFIG_BLOB = "b8982217d7c4ad99a4725af54336d7a5b24de443"
NIS_BASE = f"https://raw.githubusercontent.com/NVIDIAGameWorks/NVIDIAImageScaling/{NIS_COMMIT}/NIS"


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"V3.18 NIS {label} anchor mismatch in {path}: found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


def git_blob_sha(data: bytes) -> str:
    prefix = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(prefix + data).hexdigest()


def fetch_pinned(name: str, expected_blob: str) -> bytes:
    url = f"{NIS_BASE}/{name}"
    with urllib.request.urlopen(url, timeout=45) as response:
        data = response.read()
    actual = git_blob_sha(data)
    if actual != expected_blob:
        raise RuntimeError(
            f"Pinned NIS source identity mismatch for {name}: expected git blob {expected_blob}, got {actual}"
        )
    return data


scaler_bytes = fetch_pinned("NIS_Scaler.h", NIS_SCALER_BLOB)
config_bytes = fetch_pinned("NIS_Config.h", NIS_CONFIG_BLOB)
scaler = scaler_bytes.decode("utf-8")
config = config_bytes.decode("utf-8")

# NVIDIA's GLSL wrapper targets Vulkan-style separate texture/sampler objects.
# The algorithm body itself is portable GLSL. Adapt only the texture-access
# macros to ordinary OpenGL combined sampler2D objects; keep the pinned NIS 1.0.3
# filter/scaling math untouched.
replacements = {
    "#define NVTEX_LOAD(x, pos) texelFetch(sampler2D(x, samplerLinearClamp), pos, 0)":
        "#define NVTEX_LOAD(x, pos) texelFetch(x, pos, 0)",
    "#define NVTEX_SAMPLE(x, sampler, pos) textureLod(sampler2D(x, sampler), pos, 0)":
        "#define NVTEX_SAMPLE(x, sampler, pos) textureLod(x, pos, 0)",
    "#define NVTEX_SAMPLE_RED(x, sampler, pos) textureGather(sampler2D(x, sampler), pos, 0)":
        "#define NVTEX_SAMPLE_RED(x, sampler, pos) textureGather(x, pos, 0)",
    "#define NVTEX_SAMPLE_GREEN(x, sampler, pos) textureGather(sampler2D(x, sampler), pos, 1)":
        "#define NVTEX_SAMPLE_GREEN(x, sampler, pos) textureGather(x, pos, 1)",
    "#define NVTEX_SAMPLE_BLUE(x, sampler, pos) textureGather(sampler2D(x, sampler), pos, 2)":
        "#define NVTEX_SAMPLE_BLUE(x, sampler, pos) textureGather(x, pos, 2)",
    "#define saturate(x) clamp(x, 0, 1)":
        "#define saturate(x) clamp(x, 0.0, 1.0)",
}
for old, new in replacements.items():
    if scaler.count(old) != 1:
        raise RuntimeError(f"Pinned NIS scaler adaptation anchor mismatch: {old}")
    scaler = scaler.replace(old, new, 1)

shader_prefix = r'''#version 430 core
// OpenMW Custom V3.18 OpenGL wrapper around NVIDIA Image Scaling SDK 1.0.3.
// NVIDIA algorithm source below is pinned verbatim except for the documented
// combined-sampler OpenGL macro adaptation performed by apply_v318_nis.py.
#define NIS_HLSL 0
#define NIS_HLSL_6_2 0
#define NIS_GLSL 1
#define NIS_SCALER 1
#define NIS_HDR_MODE 0
#define NIS_USE_HALF_PRECISION 0
#define NIS_VIEWPORT_SUPPORT 1
#define NIS_TEXTURE_GATHER 0
#define NIS_CLAMP_OUTPUT 1
#define NIS_BLOCK_WIDTH 32
#define NIS_BLOCK_HEIGHT 24
#define NIS_THREAD_GROUP_SIZE 128

uniform float kDetectRatio;
uniform float kDetectThres;
uniform float kMinContrastRatio;
uniform float kRatioNorm;
uniform float kContrastBoost;
uniform float kEps;
uniform float kSharpStartY;
uniform float kSharpScaleY;
uniform float kSharpStrengthMin;
uniform float kSharpStrengthScale;
uniform float kSharpLimitMin;
uniform float kSharpLimitScale;
uniform float kScaleX;
uniform float kScaleY;
uniform float kDstNormX;
uniform float kDstNormY;
uniform float kSrcNormX;
uniform float kSrcNormY;
uniform uint kInputViewportOriginX;
uniform uint kInputViewportOriginY;
uniform uint kInputViewportWidth;
uniform uint kInputViewportHeight;
uniform uint kOutputViewportOriginX;
uniform uint kOutputViewportOriginY;
uniform uint kOutputViewportWidth;
uniform uint kOutputViewportHeight;

uniform sampler2D in_texture;
uniform sampler2D coef_scaler;
uniform sampler2D coef_usm;
layout(rgba8, binding = 0) writeonly uniform image2D out_texture;

'''
shader_suffix = r'''

layout(local_size_x = NIS_THREAD_GROUP_SIZE, local_size_y = 1, local_size_z = 1) in;
void main()
{
    NVScaler(gl_WorkGroupID.xy, gl_LocalInvocationID.x);
}
'''
compute_shader = shader_prefix + scaler + shader_suffix
if ')V318NIS"' in compute_shader:
    raise RuntimeError("Unexpected raw-string terminator in pinned NIS shader")

# Preserve the exact pinned configuration implementation and coefficient arrays in
# the generated source snapshot. This is included by nisscaler.cpp only.
nis_config_path = ROOT / "apps/openmw/mwrender/v318_nis_config.hpp"
nis_config_path.write_text(config, encoding="utf-8", newline="\n")

nis_shader_path = ROOT / "apps/openmw/mwrender/v318_nis_shader.hpp"
nis_shader_path.write_text(
    '#pragma once\n\nnamespace MWRender::V318Nis\n{\n'
    'inline constexpr const char* kComputeShader = R"V318NIS(' + compute_shader + ')V318NIS";\n'
    '}\n',
    encoding="utf-8",
    newline="\n",
)

nis_hpp = r'''#ifndef OPENMW_MWRENDER_NISSCALER_H
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
        mutable osg::buffered_value<bool> mLoggedActive;
        mutable int mOutputWidth = 0;
        mutable int mOutputHeight = 0;
    };
}

#endif
'''
(ROOT / "apps/openmw/mwrender/nisscaler.hpp").write_text(nis_hpp, encoding="utf-8", newline="\n")

nis_cpp = r'''#include "nisscaler.hpp"

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
            texture->setInternalFormat(GL_RGBA32F);
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
        osg::ref_ptr<osg::Shader> shader = new osg::Shader(osg::Shader::COMPUTE, V318Nis::kComputeShader);
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
'''
(ROOT / "apps/openmw/mwrender/nisscaler.cpp").write_text(nis_cpp, encoding="utf-8", newline="\n")

# Add the new source pair to OpenMW's mwrender source group.
cmake = ROOT / "apps/openmw/CMakeLists.txt"
replace_once(
    cmake,
    '    postprocessor pingpongcull luminancecalculator pingpongcanvas transparentpass precipitationocclusion ripples\n',
    '    postprocessor pingpongcull luminancecalculator pingpongcanvas nisscaler transparentpass precipitationocclusion ripples\n',
    "CMake source list",
)

# Enable the real provider only after the source exists.
video = ROOT / "components/settings/categories/video.hpp"
replace_once(
    video,
    'makeEnumSanitizerString({ "bilinear" })',
    'makeEnumSanitizerString({ "bilinear", "nis" })',
    "upscaler enum",
)

canvas_hpp = ROOT / "apps/openmw/mwrender/pingpongcanvas.hpp"
replace_once(
    canvas_hpp,
    '#include "luminancecalculator.hpp"\n',
    '#include "luminancecalculator.hpp"\n#include "nisscaler.hpp"\n',
    "canvas NIS include",
)
replace_once(
    canvas_hpp,
    '        mutable std::shared_ptr<LuminanceCalculator> mLuminanceCalculator;\n'
    '        mutable osg::buffered_object<osg::State::UniformMap> mEmptyUniformStacks;',
    '        mutable std::shared_ptr<LuminanceCalculator> mLuminanceCalculator;\n'
    '        mutable std::shared_ptr<NisScaler> mNisScaler;\n'
    '        mutable osg::buffered_object<osg::State::UniformMap> mEmptyUniformStacks;',
    "canvas NIS member",
)

canvas_cpp = ROOT / "apps/openmw/mwrender/pingpongcanvas.cpp"
replace_once(
    canvas_cpp,
    '#include <components/shader/shadermanager.hpp>\n',
    '#include <components/shader/shadermanager.hpp>\n#include <components/settings/values.hpp>\n',
    "canvas settings include",
)
replace_once(
    canvas_cpp,
    '        , mMultiviewResolveStateSet(new osg::StateSet)\n'
    '        , mLuminanceCalculator(luminanceCalculator)\n',
    '        , mMultiviewResolveStateSet(new osg::StateSet)\n'
    '        , mLuminanceCalculator(luminanceCalculator)\n'
    '        , mNisScaler(std::make_shared<NisScaler>())\n',
    "canvas NIS constructor",
)
replace_once(
    canvas_cpp,
    '        osg::Geometry::resizeGLObjectBuffers(maxSize);\n'
    '        mEmptyUniformStacks.resize(maxSize);',
    '        osg::Geometry::resizeGLObjectBuffers(maxSize);\n'
    '        mEmptyUniformStacks.resize(maxSize);\n'
    '        mNisScaler->resizeGLObjectBuffers(maxSize);',
    "canvas NIS GL buffer resize",
)

# Early/no-PostFX presentation path.
replace_once(
    canvas_cpp,
    '            state.applyTextureAttribute(0, mTextureScene);\n'
    '            resolveViewport->apply(state);',
    '            osg::Texture* presentationTexture = mTextureScene;\n'
    '            if (scaledOutput && Settings::video().mUpscaler.get() == "nis")\n'
    '            {\n'
    '                if (osg::Texture* nisTexture = mNisScaler->dispatch(renderInfo, mTextureScene,\n'
    '                        mTextureScene->getTextureWidth(), mTextureScene->getTextureHeight(),\n'
    '                        static_cast<int>(resolveViewport->width()), static_cast<int>(resolveViewport->height()),\n'
    '                        Settings::video().mUpscalerSharpness))\n'
    '                    presentationTexture = nisTexture;\n'
    '            }\n'
    '            state.applyTextureAttribute(0, presentationTexture);\n'
    '            resolveViewport->apply(state);',
    "no-PostFX NIS presentation",
)

# Main PostFX final presentation path created by the P0 render-scale layer.
replace_once(
    canvas_cpp,
    '            state.applyTextureAttribute(0, finalTexture);\n'
    '            drawGeometry(renderInfo);',
    '            osg::Texture* presentationTexture = finalTexture;\n'
    '            if (Settings::video().mUpscaler.get() == "nis")\n'
    '            {\n'
    '                if (osg::Texture* nisTexture = mNisScaler->dispatch(renderInfo, finalTexture,\n'
    '                        finalTexture->getTextureWidth(), finalTexture->getTextureHeight(),\n'
    '                        static_cast<int>(resolveViewport->width()), static_cast<int>(resolveViewport->height()),\n'
    '                        Settings::video().mUpscalerSharpness))\n'
    '                    presentationTexture = nisTexture;\n'
    '            }\n'
    '            state.applyTextureAttribute(0, presentationTexture);\n'
    '            drawGeometry(renderInfo);',
    "PostFX NIS presentation",
)

provenance = ROOT / "V3.18-NIS-PROVENANCE.txt"
provenance.write_text(
    "\n".join(
        [
            "NVIDIA Image Scaling SDK 1.0.3",
            f"source_commit={NIS_COMMIT}",
            f"NIS_Scaler.h_git_blob={NIS_SCALER_BLOB}",
            f"NIS_Config.h_git_blob={NIS_CONFIG_BLOB}",
            "integration=custom OpenGL 4.3/OSG compute wrapper",
            "algorithm_changes=OpenGL combined-sampler macro adaptation only",
            "precision=fp32",
            "block=32x24",
            "threads=128",
            "output=RGBA8",
            "fallback=explicit bilinear",
            "",
        ]
    ),
    encoding="utf-8",
    newline="\n",
)

checks = {
    ROOT / "apps/openmw/mwrender/nisscaler.cpp": (
        "NVScalerUpdateConfig", "glDispatchCompute", "glMemoryBarrier", "falling back to bilinear upscale",
        "35e13ba316c98eeecf16f37eae70ce88019911f6"),
    ROOT / "apps/openmw/mwrender/v318_nis_shader.hpp": (
        "#version 430 core", "NVScaler(gl_WorkGroupID.xy", "layout(rgba8, binding = 0)"),
    canvas_cpp: ("mNisScaler->dispatch", 'mUpscaler.get() == "nis"'),
    video: ('"bilinear", "nis"',),
    cmake: ("pingpongcanvas nisscaler transparentpass",),
}
for path, markers in checks.items():
    text = path.read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.18 NIS generated source missing {marker!r} in {path}")

print(f"V3.18 NVIDIA Image Scaling 1.0.3 OpenGL compute layer applied from pinned commit {NIS_COMMIT}")
