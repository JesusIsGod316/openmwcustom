#include "watersurface.hpp"

#include <vsg/commands/BindVertexBuffers.h>
#include <vsg/commands/Draw.h>
#include <vsg/maths/transform.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/Switch.h>
#include <vsg/state/BindDescriptorSet.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DescriptorImage.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/ImageInfo.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/Sampler.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace RenderVsg
{
    namespace
    {
        constexpr const char* WaterVertexSource = R"(
#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 0) out vec2 waterUv;
layout(location = 1) noperspective out vec2 screenUv;

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
    screenUv = clip.xy / max(abs(clip.w), 0.0001) * 0.5 + 0.5;
}
)";

        constexpr const char* WaterFragmentSource = R"(
#version 450
layout(location = 0) in vec2 waterUv;
layout(location = 1) noperspective in vec2 screenUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D reflectionImage;
layout(set = 0, binding = 1) uniform sampler2D refractionImage;

layout(push_constant) uniform WaterParameters
{
    // x=time, y=underwater, z=has reflection, w=has refraction.
    vec4 state;
    vec4 fogColor;
    vec4 skyColor;
    // xy=inverse main extent, z=unused, w=unused.
    vec4 target;
} water;

void main()
{
    float phase = water.state.x;
    vec2 ripple = vec2(sin(waterUv.y * 170.0 + phase * 0.9),
        cos(waterUv.x * 145.0 - phase * 0.7)) * 0.006;
    vec2 uv = clamp(screenUv + ripple, vec2(0.002), vec2(0.998));
    vec3 reflection = texture(reflectionImage, uv).rgb;
    vec3 refraction = texture(refractionImage, uv).rgb;
    if (water.state.z < 0.5)
        reflection = water.skyColor.rgb;
    if (water.state.w < 0.5)
        refraction = water.fogColor.rgb * vec3(0.55, 0.72, 0.78);
    float fresnel = clamp(0.32 + abs(ripple.x + ripple.y) * 12.0, 0.22, 0.68);
    vec3 color = mix(refraction, reflection, fresnel);
    if (water.state.y > 0.5)
        color = mix(color, water.fogColor.rgb, 0.46);
    outColor = vec4(color, water.state.y > 0.5 ? 0.72 : 0.82);
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
        vsg::ref_ptr<vsg::ImageView> reflection, vsg::ref_ptr<vsg::ImageView> refraction)
    {
        WaterSurface result;
        result.mHasReflection = reflection.valid();
        result.mHasRefraction = refraction.valid();

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
        });
        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{ descriptorLayout },
            vsg::PushConstantRanges{ { VK_SHADER_STAGE_VERTEX_BIT, 0, 128 },
                { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 } });

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
        stateGroup->add(vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
            pipelineLayout, vsg::ShaderStages{ vertexShader, fragmentShader }, states)));
        auto descriptorSet = vsg::DescriptorSet::create(descriptorLayout,
            vsg::Descriptors{
                vsg::DescriptorImage::create(imageInfo(sampler, reflection, { 80, 110, 140, 255 }), 0, 0,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
                vsg::DescriptorImage::create(imageInfo(sampler, refraction, { 35, 75, 85, 255 }), 1, 0,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER),
            });
        stateGroup->add(vsg::BindDescriptorSet::create(
            VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, descriptorSet));

        result.mParameters = vsg::vec4Array::create(4);
        result.mParameters->properties.dataVariance = vsg::DYNAMIC_DATA;
        stateGroup->add(vsg::PushConstants::create(
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, result.mParameters.get()));
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
        const vsg::vec4 values[4]{
            { static_cast<float>(std::fmod(simulationTime, 4096.0)), environment.underwater ? 1.0f : 0.0f,
                mHasReflection ? 1.0f : 0.0f, mHasRefraction ? 1.0f : 0.0f },
            { environment.fogColor.r, environment.fogColor.g, environment.fogColor.b, environment.fogColor.a },
            { environment.skyColor.r, environment.skyColor.g, environment.skyColor.b, environment.skyColor.a },
            { 1.0f / static_cast<float>(mainView.extent.width),
                1.0f / static_cast<float>(mainView.extent.height), 0.0f, 0.0f },
        };
        bool changed = false;
        for (std::size_t i = 0; i < 4; ++i)
        {
            changed = changed || (*mParameters)[i] != values[i];
            (*mParameters)[i] = values[i];
        }
        if (changed)
            mParameters->dirty();
    }
}
