#include "watersurface.hpp"
#include "viewpipelinebinding.hpp"

#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Draw.h>
#include <vsg/maths/transform.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/Switch.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace RenderVsg
{
    namespace
    {
        constexpr const char* WaterVertexSource = R"(
#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec2 waterUv;

layout(push_constant) uniform TransformBlock
{
    mat4 projection;
    mat4 modelView;
} transformBlock;

void main()
{
    vec4 clip = transformBlock.projection * transformBlock.modelView * vec4(inPosition, 1.0);
    gl_Position = clip;
    waterUv = inPosition.xy;
}
)";

        constexpr const char* WaterFragmentSource = R"(
#version 450
layout(location = 0) in vec2 waterUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D reflectionImage;
layout(set = 0, binding = 1) uniform sampler2D refractionImage;
layout(set = 0, binding = 3) uniform sampler2D normalMap;

layout(set = 0, binding = 2, std140) uniform WaterParameters
{
    // x=time, y=underwater, z=has reflection, w=has refraction.
    vec4 state;
    vec4 fogColor;
    vec4 skyColor;
    // xy=inverse main extent, z=legacy composition control, w=unused.
    vec4 target;
    // xyz=camera world position, w=plane radius.
    vec4 camera;
    // x=water height, y=normal map enabled, z=rain intensity, w=unused.
    vec4 surface;
} water;

// Wave scales/weights and dielectric response follow compatibility/water.frag
// and lib/water/fresnel.glsl. Rain collision ripples/depth absorption are separate.
vec2 normalCoords(vec2 uv, float scale, float speed, float time, vec2 timer, vec3 previousNormal)
{
    vec2 chop = abs(previousNormal.z) > 0.0001 ? previousNormal.xy / previousNormal.z : vec2(0.0);
    return uv * (75.0 * scale) + vec2(0.5, -0.8) * time * (0.2 * speed) - chop * 0.05 + time * timer;
}

float dielectric(vec3 incoming, vec3 normal, float eta)
{
    float c = abs(dot(incoming, normal));
    float g = eta * eta - 1.0 + c * c;
    if (g <= 0.0) return 1.0;
    g = sqrt(g);
    float A = (g - c) / (g + c);
    float B = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return clamp(0.5 * A * A * (1.0 + B * B), 0.0, 1.0);
}

void main()
{
    float phase = water.state.x;
    vec2 ripple = vec2(sin(waterUv.y * 170.0 + phase * 0.9),
        cos(waterUv.x * 145.0 - phase * 0.7)) * 0.006;
    vec3 waterNormal = vec3(0,0,1);
    vec2 worldXY = water.camera.xy + waterUv * water.camera.w;
    if (water.surface.y > 0.5)
    {
        vec2 UV = worldXY * (3.0 / (8192.0 * 5.0));
        vec3 n0 = texture(normalMap, normalCoords(UV,0.05,0.04,phase,vec2(-0.015,-0.005),vec3(0))).rgb * 2.0 - 1.0;
        vec3 n1 = texture(normalMap, normalCoords(UV,0.1,0.08,phase,vec2(0.02,0.015),n0)).rgb * 2.0 - 1.0;
        vec3 n2 = texture(normalMap, normalCoords(UV,0.25,0.07,phase,vec2(-0.04,-0.03),n1)).rgb * 2.0 - 1.0;
        vec3 n3 = texture(normalMap, normalCoords(UV,0.5,0.09,phase,vec2(0.03,0.04),n2)).rgb * 2.0 - 1.0;
        vec3 n4 = texture(normalMap, normalCoords(UV,1.0,0.4,phase,vec2(-0.02,0.1),n3)).rgb * 2.0 - 1.0;
        vec3 n5 = texture(normalMap, normalCoords(UV,2.0,0.7,phase,vec2(0.1,-0.06),n4)).rgb * 2.0 - 1.0;
        float rain = clamp(water.surface.z,0.0,1.0);
        vec3 n = (n0+n1)*0.1 + (n2+n3)*mix(0.1,0.2,rain) + (n4+n5)*mix(0.1,0.3,rain);
        float bump = mix(0.5,2.5,rain);
        waterNormal = normalize(vec3(-n.xy*bump,n.z));
        ripple = waterNormal.xy * 0.10;
    }
    // Fragment coordinates remain valid when the large plane crosses the
    // camera/near plane. Interpolating abs(clip.w)-divided corner UVs does not.
    vec2 uv = clamp(gl_FragCoord.xy * water.target.xy + ripple, vec2(0.002), vec2(0.998));
    // FrameProducer reflects forward/up then rebuilds a right-handed camera.
    // Its right axis is reversed relative to the raw mirror transform. Undo
    // that horizontal reversal when projecting its image onto the water.
    vec2 reflectionUv = water.target.z > 0.5 ? uv : vec2(1.0 - uv.x, uv.y);
    vec3 reflection = texture(reflectionImage, reflectionUv).rgb;
    vec2 refractionUv = water.surface.y > 0.5
        ? clamp(gl_FragCoord.xy * water.target.xy - waterNormal.xy * 0.07, vec2(0.002),vec2(0.998)) : uv;
    vec3 refraction = texture(refractionImage, refractionUv).rgb;
    if (water.state.z < 0.5)
        reflection = water.skyColor.rgb;
    if (water.state.w < 0.5)
        refraction = water.fogColor.rgb * vec3(0.55, 0.72, 0.78);
    float fresnel = clamp(0.32 + abs(ripple.x + ripple.y) * 12.0, 0.22, 0.68);
    if (water.surface.y > 0.5)
        fresnel = dielectric(normalize(vec3(worldXY,water.surface.x) - water.camera.xyz), waterNormal,
            water.state.y > 0.5 ? 1.0 / 1.333 : 1.333);
    vec3 color = mix(refraction, reflection, fresnel);
    if (water.state.y > 0.5)
        color = mix(color, water.fogColor.rgb, 0.46);
    // The refraction target already contains the transmitted scene. Blending
    // it again with the main scene counts that background twice.
    float alpha = water.state.w > 0.5 && water.target.z < 0.5
        ? 1.0 : (water.state.y > 0.5 ? 0.72 : 0.82);
    outColor = vec4(color, alpha);
}
)";

        [[nodiscard]] vsg::ref_ptr<vsg::ImageInfo> imageInfo(
            vsg::ref_ptr<vsg::Sampler> sampler, vsg::ref_ptr<vsg::ImageView> image, const vsg::ubvec4& fallback)
        {
            if (image)
                return vsg::ImageInfo::create(sampler, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            auto data = vsg::ubvec4Array2D::create(1, 1, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
            (*data)(0, 0) = fallback;
            return vsg::ImageInfo::create(sampler, data, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }

    WaterSurface WaterSurface::create(
        vsg::ref_ptr<vsg::ImageView> reflection, vsg::ref_ptr<vsg::ImageView> refraction,
        vsg::ref_ptr<vsg::Data> normalMap)
    {
        WaterSurface result;
        result.mHasReflection = reflection.valid();
        result.mHasRefraction = refraction.valid();
        result.mLegacyComposition = std::getenv("OPENMW_V4_LEGACY_WATER_COMPOSITION_CONTROL") != nullptr;
        result.mHasNormalMap = normalMap.valid();

        auto vertexShader = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", std::string(WaterVertexSource));
        auto fragmentShader
            = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", std::string(WaterFragmentSource));
        if (!vertexShader || !fragmentShader)
            return result;

        auto sampler = vsg::Sampler::create();
        sampler->minFilter = VK_FILTER_LINEAR;
        sampler->magFilter = VK_FILTER_LINEAR;
        sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler->addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;

        auto descriptorLayout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
            { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        });
        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{ descriptorLayout },
            vsg::PushConstantRanges{ { VK_SHADER_STAGE_VERTEX_BIT, 0, 128 } });

        vsg::VertexInputState::Bindings bindings{
            { 0, sizeof(vsg::vec3), VK_VERTEX_INPUT_RATE_VERTEX },
        };
        vsg::VertexInputState::Attributes attributes{
            { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 },
        };
        auto rasterization = vsg::RasterizationState::create();
        rasterization->cullMode = VK_CULL_MODE_NONE;
        auto depth = vsg::DepthStencilState::create();
        depth->depthTestEnable = VK_TRUE;
        depth->depthWriteEnable = VK_FALSE;
        depth->depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        auto blend = vsg::ColorBlendState::create();
        blend->attachments[0].blendEnable = VK_TRUE;
        blend->attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend->attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend->attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend->attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;

        vsg::GraphicsPipelineStates states{ vsg::VertexInputState::create(bindings, attributes),
            vsg::InputAssemblyState::create(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP),
            vsg::ViewportState::create(0, 0, 1, 1), rasterization, vsg::MultisampleState::create(), blend, depth,
            vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR) };

        auto stateGroup = vsg::StateGroup::create();
        auto pipeline = vsg::GraphicsPipeline::create(
            pipelineLayout, vsg::ShaderStages{ vertexShader, fragmentShader }, states);
        pipeline->setValue("openmw.pipeline.family", "water-surface");
        stateGroup->add(ViewPipelineBinding::create(std::move(pipeline)));
        // Keep effect parameters out of VSG's 128-byte matrix push constants.
        // Dynamic descriptors are uploaded by the viewer's transfer task.
        result.mParameters = vsg::vec4Array::create(6);
        result.mParameters->properties.dataVariance = vsg::DYNAMIC_DATA;
        auto normalSampler = vsg::Sampler::create();
        normalSampler->addressModeU = normalSampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        if (!normalMap)
        {
            auto flat = vsg::ubvec4Array2D::create(1,1, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
            (*flat)(0,0) = vsg::ubvec4(128,128,255,255);
            normalMap = flat;
        }
        auto descriptorSet = vsg::DescriptorSet::create(descriptorLayout,
            vsg::Descriptors{
                vsg::DescriptorImage::create(imageInfo(sampler, reflection, { 80, 110, 140, 255 }), 0, 0,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                vsg::DescriptorImage::create(imageInfo(sampler, refraction, { 35, 75, 85, 255 }), 1, 0,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                vsg::DescriptorBuffer::create(result.mParameters, 2, 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
                vsg::DescriptorImage::create(normalSampler, normalMap, 3, 0,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            });
        stateGroup->add(vsg::BindDescriptorSet::create(
            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, descriptorSet));

        auto vertices = vsg::vec3Array::create(
            { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f },
                { -1.0f, 1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } });
        stateGroup->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ vertices }));
        stateGroup->addChild(vsg::Draw::create(4, 1, 0, 0));

        result.mPlacement = vsg::MatrixTransform::create();
        result.mPlacement->addChild(stateGroup);
        result.mRoot = vsg::Switch::create();
        result.mRoot->addChild(false, result.mPlacement);
        return result;
    }

    WaterSurface::operator bool() const noexcept
    {
        return mRoot && mPlacement && mParameters;
    }

    vsg::ref_ptr<vsg::Node> WaterSurface::node() const noexcept
    {
        return mRoot;
    }

    void WaterSurface::update(const RenderCore::FrameEnvironmentState& environment,
        const RenderCore::FrameView& mainView, double simulationTime) noexcept
    {
        if (!*this)
            return;
        mRoot->setAllChildren(environment.waterEnabled);
        const double radius = std::clamp(mainView.current.projection.farPlane * 1.25, 8192.0, 200000.0);
        mPlacement->matrix = vsg::translate(mainView.current.worldPosition.x, mainView.current.worldPosition.y,
                                  environment.waterHeight)
            * vsg::scale(radius, radius, 1.0);
        const vsg::vec4 values[6]{
            { static_cast<float>(std::fmod(simulationTime, 4096.0)), environment.underwater ? 1.0f : 0.0f,
                mHasReflection ? 1.0f : 0.0f, mHasRefraction ? 1.0f : 0.0f },
            { environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, environment.fogColor.a },
            { environment.skyColor.r, environment.skyColor.g, environment.skyColor.b, environment.skyColor.a },
            { 1.0f / static_cast<float>(mainView.extent.width),
                1.0f / static_cast<float>(mainView.extent.height), mLegacyComposition ? 1.0f : 0.0f, 0.0f },
            {static_cast<float>(mainView.current.worldPosition.x), static_cast<float>(mainView.current.worldPosition.y),
                static_cast<float>(mainView.current.worldPosition.z), static_cast<float>(radius)},
            {static_cast<float>(environment.waterHeight), mHasNormalMap ? 1.0f : 0.0f,
                environment.precipitationIntensity, 0.0f},
        };
        bool changed = false;
        for (std::size_t i = 0; i < 6; ++i)
        {
            changed = changed || (*mParameters)[i] != values[i];
            (*mParameters)[i] = values[i];
        }
        if (changed)
            mParameters->dirty();
    }
}
