#include "pass.hpp"

#include <sstream>
#include <string>
#include <unordered_set>

#include <osg/BindImageTexture>
#include <osg/FrameBufferObject>
#include <osg/Program>
#include <osg/Shader>
#include <osg/State>
#include <osg/StateSet>

#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/settings/values.hpp>
#include <components/stereo/multiview.hpp>
#include <components/version/version.hpp>

#include "stateupdater.hpp"
#include "technique.hpp"

namespace
{
    constexpr char s_DefaultVertex[] = R"GLSL(
#if OMW_USE_BINDINGS
    omw_In vec2 omw_Vertex;
#endif
omw_Out vec2 omw_TexCoord;

void main()
{
    omw_Position = vec4(omw_Vertex.xy, 0.0, 1.0);
    omw_TexCoord = omw_Position.xy * 0.5 + 0.5;
})GLSL";

    constexpr char s_DefaultVertexMultiview[] = R"GLSL(
layout(num_views = 2) in;
#if OMW_USE_BINDINGS
    omw_In vec2 omw_Vertex;
#endif
omw_Out vec2 omw_TexCoord;

void main()
{
    omw_Position = vec4(omw_Vertex.xy, 0.0, 1.0);
    omw_TexCoord = omw_Position.xy * 0.5 + 0.5;
})GLSL";

}

namespace Fx
{
    Pass::Pass(Pass::Type type, Pass::Order order, bool ubo)
        : mCompiled(false)
        , mType(type)
        , mOrder(order)
        , mLegacyGLSL(true)
        , mUBO(ubo)
    {
    }

    std::string Pass::getPassHeader(Technique& technique, std::string_view preamble, bool fragOut,
        VulkanShaderSources* vulkan)
    {
        const bool legacy = !vulkan && mLegacyGLSL;
        std::string header = R"GLSL(
#version @version @profile
@extensions

@uboStruct

#define OMW_API_VERSION @apiVersion
#define OMW_REVERSE_Z @reverseZ
#define OMW_RADIAL_FOG @radialFog
#define OMW_EXPONENTIAL_FOG @exponentialFog
#define OMW_HDR @hdr
#define OMW_NORMALS @normals
#define OMW_USE_BINDINGS @useBindings
#define OMW_MULTIVIEW @multiview
#define omw_In @in
#define omw_Out @out
#define omw_Position @position
#define omw_Texture1D @texture1D
#define omw_Texture2D @texture2D
#define omw_Texture2DArray @texture2DArray
#define omw_Texture3D @texture3D
#define omw_Vertex @vertex
#define omw_FragColor @fragColor

@fragBinding

uniform @builtinSampler omw_SamplerLastShader;
uniform @builtinSampler omw_SamplerLastPass;
uniform @builtinSampler omw_SamplerDepth;
uniform @builtinSampler omw_SamplerNormals;
uniform @builtinSampler omw_SamplerDistortion;

uniform vec4 omw_PointLights[@pointLightCount];
uniform int omw_PointLightsCount;

#if OMW_MULTIVIEW
uniform mat4 projectionMatrixMultiView[2];
uniform mat4 invProjectionMatrixMultiView[2];
#endif

int omw_GetPointLightCount()
{
    return omw_PointLightsCount;
}

vec3 omw_GetPointLightWorldPos(int index)
{
    return omw_PointLights[(index * 3)].xyz;
}

vec3 omw_GetPointLightDiffuse(int index)
{
    return omw_PointLights[(index * 3) + 1].xyz;
}

vec3 omw_GetPointLightAttenuation(int index)
{
    return omw_PointLights[(index * 3) + 2].xyz;
}

float omw_GetPointLightRadius(int index)
{
    return omw_PointLights[(index * 3) + 2].w;
}

#if @ubo
    layout(std140) uniform _data { _omw_data omw; };
#else
    uniform _omw_data omw;
#endif


mat4 omw_ProjectionMatrix()
{
#if OMW_MULTIVIEW
    return projectionMatrixMultiView[gl_ViewID_OVR];
#else
    return omw.projectionMatrix;
#endif
}

mat4 omw_InvProjectionMatrix()
{
#if OMW_MULTIVIEW
    return invProjectionMatrixMultiView[gl_ViewID_OVR];
#else
    return omw.invProjectionMatrix;
#endif
}

    float omw_GetDepth(vec2 uv)
    {
#if OMW_MULTIVIEW
        float depth = omw_Texture2DArray(omw_SamplerDepth, vec3(uv, gl_ViewID_OVR)).r;
#else
        float depth = omw_Texture2D(omw_SamplerDepth, uv).r;
#endif
#if OMW_REVERSE_Z
        return 1.0 - depth;
#else
        return depth;
#endif
    }

    vec4 omw_GetDistortion(vec2 uv)
    {
#if OMW_MULTIVIEW
        return omw_Texture2DArray(omw_SamplerDistortion, vec3(uv, gl_ViewID_OVR));
#else
        return omw_Texture2D(omw_SamplerDistortion, uv);
#endif
    }

    vec4 omw_GetLastShader(vec2 uv)
    {
#if OMW_MULTIVIEW
        return omw_Texture2DArray(omw_SamplerLastShader, vec3(uv, gl_ViewID_OVR));
#else
        return omw_Texture2D(omw_SamplerLastShader, uv);
#endif
    }

    vec4 omw_GetLastPass(vec2 uv)
    {
#if OMW_MULTIVIEW
        return omw_Texture2DArray(omw_SamplerLastPass, vec3(uv, gl_ViewID_OVR));
#else
        return omw_Texture2D(omw_SamplerLastPass, uv);
#endif
    }

    vec3 omw_GetNormals(vec2 uv)
    {
#if OMW_MULTIVIEW
        return omw_Texture2DArray(omw_SamplerNormals, vec3(uv, gl_ViewID_OVR)).rgb * 2.0 - 1.0;
#else
        return omw_Texture2D(omw_SamplerNormals, uv).rgb * 2.0 - 1.0;
#endif
    }

    vec3 omw_GetNormalsWorldSpace(vec2 uv)
    {
        return (vec4(omw_GetNormals(uv), 0.0) * omw.viewMatrix).rgb;
    }

    vec3 omw_GetWorldPosFromUV(vec2 uv)
    {
        float depth = omw_GetDepth(uv);
#if (OMW_REVERSE_Z == 1)
        float flippedDepth = 1.0 - depth;
#else
        float flippedDepth = depth * 2.0 - 1.0;
#endif
        vec4 clip_space = vec4(uv * 2.0 - 1.0, flippedDepth, 1.0);
        vec4 world_space = omw.invViewMatrix * (omw.invProjectionMatrix * clip_space);
        return world_space.xyz / world_space.w;
    }

    float omw_GetLinearDepth(vec2 uv)
    {
#if (OMW_REVERSE_Z == 1)
        float depth = omw_GetDepth(uv);
        float dist = omw.near * omw.far / (omw.far + depth * (omw.near - omw.far));
#else
        float depth = omw_GetDepth(uv) * 2.0 - 1.0;
        float dist = 2.0 * omw.near * omw.far / (omw.far + omw.near - depth * (omw.far - omw.near));
#endif

        return dist;
    }

float omw_EstimateFogCoverageFromUV(vec2 uv)
    {
#if OMW_RADIAL_FOG
        vec3 uvPos = omw_GetWorldPosFromUV(uv);
        float dist = length(uvPos - omw.eyePos.xyz);
#else
        float dist = omw_GetLinearDepth(uv);
#endif
#if OMW_EXPONENTIAL_FOG
        float fogValue = 1.0 - exp(-2.0 * max(0.0, dist - omw.fogNear/2.0) / (omw.fogFar - omw.fogNear/2.0));
#else
        float fogValue = clamp((dist - omw.fogNear) / (omw.fogFar - omw.fogNear), 0.0, 1.0);
#endif

        return fogValue;
    }

#if OMW_HDR
    uniform sampler2D omw_EyeAdaptation;
#endif

    float omw_GetEyeAdaptation()
    {
#if OMW_HDR
        return omw_Texture2D(omw_EyeAdaptation, vec2(0.5, 0.5)).r;
#else
        return 1.0;
#endif
    }
)GLSL";

        std::stringstream extBlock;
        for (const auto& extension : technique.getGLSLExtensions())
            extBlock << "#ifdef " << extension << '\n'
                     << "\t#extension " << extension << ": enable" << '\n'
                     << "#endif" << '\n';

        const std::vector<std::pair<std::string, std::string>> defines
            = { { "@pointLightCount", std::to_string(SceneUtil::PPLightBuffer::sMaxPPLightsArraySize) },
                  { "@apiVersion", std::to_string(Version::getPostprocessingApiRevision()) },
                  { "@version", vulkan ? "450" : std::to_string(technique.getGLSLVersion()) },
                  { "@multiview", !vulkan && Stereo::getMultiview() ? "1" : "0" },
                  { "@builtinSampler", !vulkan && Stereo::getMultiview() ? "sampler2DArray" : "sampler2D" },
                  { "@profile", vulkan ? "core" : technique.getGLSLProfile() }, { "@extensions", extBlock.str() },
                  { "@uboStruct", StateUpdater::getStructDefinition() }, { "@ubo", mUBO ? "1" : "0" },
                  { "@normals", technique.getNormals() ? "1" : "0" },
                  { "@reverseZ", vulkan || SceneUtil::AutoDepth::isReversed() ? "1" : "0" },
                  { "@radialFog", Settings::fog().mRadialFog ? "1" : "0" },
                  { "@exponentialFog", Settings::fog().mExponentialFog ? "1" : "0" },
                  { "@hdr", technique.getHDR() ? "1" : "0" }, { "@in", legacy ? "varying" : "in" },
                  { "@out", legacy ? "varying" : "out" }, { "@position", "gl_Position" },
                  { "@texture1D", legacy ? "texture1D" : "texture" },
                  // Note, @texture2DArray must be defined before @texture2D since @texture2D is a perfect prefix of
                  // texture2DArray
                  { "@texture2DArray", legacy ? "texture2DArray" : "texture" },
                  { "@texture2D", legacy ? "texture2D" : "texture" },
                  { "@texture3D", legacy ? "texture3D" : "texture" },
                  { "@vertex", vulkan ? "(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0)"
                                      : legacy ? "gl_Vertex" : "_omw_Vertex" },
                  { "@fragColor", legacy ? "gl_FragColor" : "_omw_FragColor" },
                  { "@useBindings", legacy ? "0" : "1" },
                  { "@fragBinding", vulkan ? (fragOut ? "layout(location=0) out vec4 omw_FragColor;" : "")
                                            : legacy ? "" : "out vec4 omw_FragColor;" } };

        for (const auto& [define, value] : defines)
            for (size_t pos = header.find(define); pos != std::string::npos; pos = header.find(define))
                header.replace(pos, define.size(), value);

        if (vulkan)
        {
            const auto replace = [&](std::string_view before, const std::string& after)
            {
                const auto pos = header.find(before);
                if (pos == std::string::npos) throw std::logic_error("OMWFX Vulkan header contract changed");
                header.replace(pos, before.size(), after);
            };
            replace("layout(std140) uniform _data { _omw_data omw; };",
                "layout(std140,set=0,binding=0) uniform _data { _omw_data omw; };");
            replace("uniform _omw_data omw;",
                "layout(std140,set=0,binding=0) uniform _data { _omw_data omw; };");
            replace("uniform vec4 omw_PointLights[", "layout(std140,set=0,binding=1) uniform _lights { vec4 omw_PointLights[");
            replace("uniform int omw_PointLightsCount;", "int omw_PointLightsCount; };");
            const std::array<std::string, 6> builtins = {"omw_SamplerLastShader", "omw_SamplerLastPass",
                "omw_SamplerDepth", "omw_SamplerNormals", "omw_SamplerDistortion", "omw_EyeAdaptation"};
            for (const auto& name : builtins)
            {
                const auto binding = vulkan->samplers.size() + 3;
                vulkan->samplers.push_back(name);
                const std::string declaration = "uniform sampler2D " + name + ";";
                replace(declaration, "layout(set=0,binding=" + std::to_string(binding) + ") " + declaration);
            }
        }

        const auto sampler = [&](const std::string& declaration, const std::string& name)
        {
            if (vulkan)
            {
                const auto binding = vulkan->samplers.size() + 3;
                vulkan->samplers.push_back(name);
                header.append("layout(set=0,binding=" + std::to_string(binding) + ") ");
            }
            header.append(declaration);
        };
        std::unordered_set<std::string> targets;
        for (const auto& target : mRenderTargets)
            if (!vulkan || (!target.empty() && targets.insert(target).second))
                sampler("uniform sampler2D " + target + ";", target);

        std::string parameters;
        for (auto& uniform : technique.getUniformMap())
            if (auto glsl = uniform->getGLSL())
            {
                if (uniform->mSamplerType) sampler(*glsl, uniform->mName);
                else if (vulkan && glsl->starts_with("uniform ")) parameters.append(glsl->substr(8));
                else header.append(*glsl);
            }
        if (!parameters.empty())
            header.append("layout(std140,set=0,binding=2) uniform _parameters { " + parameters + " };\n");

        header.append(preamble);

        return header;
    }

    VulkanShaderSources Pass::getVulkanSources(Technique& technique)
    {
        if (!mCompiled || mType != Type::Pixel)
            throw std::runtime_error("Vulkan post-processing requires a compiled pixel pass: " + mName);
        VulkanShaderSources result;
        auto vertex = mSharedBody + mVertexBody;
        auto fragment = mSharedBody + mFragmentBody;
        qualifyVulkanInterfaces(vertex, fragment);
        VulkanShaderSources fragmentBindings;
        result.vertex = getPassHeader(technique, {}, false, &result) + vertex;
        result.fragment = getPassHeader(technique, {}, true, &fragmentBindings) + fragment;
        if (result.samplers != fragmentBindings.samplers)
            throw std::logic_error("OMWFX stage descriptor bindings differ");
        return result;
    }

    void Pass::prepareStateSet(osg::StateSet* stateSet, const std::string& name) const
    {
        osg::ref_ptr<osg::Program> program = new osg::Program;
        if (mType == Type::Pixel)
        {
            program->addShader(new osg::Shader(*mVertex));
            program->addShader(new osg::Shader(*mFragment));
        }
        else if (mType == Type::Compute)
        {
            program->addShader(new osg::Shader(*mCompute));
        }

        if (mUBO)
            program->addBindUniformBlock("_data", static_cast<int>(Resource::SceneManager::UBOBinding::PostProcessor));

        program->setName(name);

        if (!mLegacyGLSL)
        {
            program->addBindFragDataLocation("_omw_FragColor", 0);
            program->addBindAttribLocation("_omw_Vertex", 0);
        }

        stateSet->setAttribute(program);

        if (mBlendSource && mBlendDest)
            stateSet->setAttributeAndModes(new osg::BlendFunc(mBlendSource.value(), mBlendDest.value()));

        if (mBlendEq)
            stateSet->setAttributeAndModes(new osg::BlendEquation(mBlendEq.value()));
    }

    void Pass::dirty()
    {
        mVertex = nullptr;
        mFragment = nullptr;
        mCompute = nullptr;
        mVertexBody.clear();
        mFragmentBody.clear();
        mSharedBody.clear();
        mCompiled = false;
    }

    void Pass::compile(Technique& technique, std::string_view preamble)
    {
        if (mCompiled)
            return;

        mLegacyGLSL = technique.getGLSLVersion() < 330;

        if (mType == Type::Pixel)
        {
            if (!mVertex)
                mVertex = new osg::Shader(
                    osg::Shader::VERTEX, Stereo::getMultiview() ? s_DefaultVertexMultiview : s_DefaultVertex);

            mVertexBody = mVertex->getShaderSource();
            mFragmentBody = mFragment->getShaderSource();
            mSharedBody = preamble;
            mVertex->setShaderSource(getPassHeader(technique, preamble).append(mVertex->getShaderSource()));
            mFragment->setShaderSource(getPassHeader(technique, preamble, true).append(mFragment->getShaderSource()));

            mVertex->setName(mName);
            mFragment->setName(mName);
        }
        else if (mType == Type::Compute)
        {
            mCompute->setShaderSource(getPassHeader(technique, preamble).append(mCompute->getShaderSource()));
            mCompute->setName(mName);
        }

        mCompiled = true;
    }

}
