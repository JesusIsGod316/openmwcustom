#include "skybackdrop.hpp"

#include <vsg/commands/Draw.h>
#include <vsg/core/Array.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/Switch.h>
#include <vsg/state/ColorBlendState.h>
#include <vsg/state/DepthStencilState.h>
#include <vsg/state/DynamicState.h>
#include <vsg/state/GraphicsPipeline.h>
#include <vsg/state/InputAssemblyState.h>
#include <vsg/state/MultisampleState.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/PushConstants.h>
#include <vsg/state/RasterizationState.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/state/VertexInputState.h>
#include <vsg/state/ViewportState.h>

#include <algorithm>
#include <string>

namespace RenderVsg
{
    namespace
    {
        constexpr const char* SkyVertexSource = R"(
#version 450
layout(location = 0) out vec2 skyUv;

void main()
{
    const vec2 positions[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    const vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 1.0, 1.0);
    skyUv = position * 0.5 + 0.5;
}
)";

        constexpr const char* SkyFragmentSource = R"(
#version 450
layout(location = 0) in vec2 skyUv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform SkyParameters
{
    vec4 zenithColor;
    vec4 horizonColor;
    vec4 weather;
    vec4 sunScreen;
    vec4 sunColor;
} sky;

void main()
{
    // Vulkan's positive viewport height maps NDC -Y to the top of the image.
    const float height = smoothstep(0.0, 0.82, clamp(1.0 - skyUv.y, 0.0, 1.0));
    vec3 color = mix(sky.horizonColor.rgb, sky.zenithColor.rgb, height);
    const float stormWeight = clamp(sky.weather.y, 0.0, 1.0) * 0.18;
    const float precipitationWeight = clamp(sky.weather.z, 0.0, 1.0) * 0.10;
    color *= 1.0 - stormWeight - precipitationWeight;
    const vec2 sunDelta = vec2((skyUv.x - sky.sunScreen.x) * sky.sunScreen.w, skyUv.y - sky.sunScreen.y);
    const float sunDisc = (1.0 - smoothstep(0.018, 0.026, length(sunDelta))) * sky.sunScreen.z;
    color = mix(color, sky.sunColor.rgb, sunDisc * clamp(sky.sunColor.a, 0.0, 1.0));
    outColor = vec4(color, 1.0);
}
)";

        [[nodiscard]] vsg::vec4 colorValue(const RenderCore::Color& color) noexcept
        {
            return { color.r, color.g, color.b, color.a };
        }
    }

    SkyBackdrop SkyBackdrop::create()
    {
        SkyBackdrop result;
        auto vertexShader
            = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", std::string(SkyVertexSource));
        auto fragmentShader
            = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", std::string(SkyFragmentSource));
        if (!vertexShader || !fragmentShader)
            return result;

        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{},
            vsg::PushConstantRanges{ VkPushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, 128 },
                VkPushConstantRange{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80 } });

        auto rasterization = vsg::RasterizationState::create();
        rasterization->cullMode = VK_CULL_MODE_NONE;
        auto depth = vsg::DepthStencilState::create();
        depth->depthTestEnable = VK_FALSE;
        depth->depthWriteEnable = VK_FALSE;

        vsg::GraphicsPipelineStates states{ vsg::VertexInputState::create(),
            vsg::InputAssemblyState::create(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
            vsg::ViewportState::create(0, 0, 1, 1), rasterization, vsg::MultisampleState::create(),
            vsg::ColorBlendState::create(), depth,
            vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR) };

        auto stateGroup = vsg::StateGroup::create();
        stateGroup->add(vsg::BindGraphicsPipeline::create(vsg::GraphicsPipeline::create(
            pipelineLayout, vsg::ShaderStages{ vertexShader, fragmentShader }, states)));

        result.mParameters = vsg::vec4Array::create(5);
        result.mParameters->properties.dataVariance = vsg::DYNAMIC_DATA;
        (*result.mParameters)[0] = vsg::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        (*result.mParameters)[1] = vsg::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        (*result.mParameters)[2] = vsg::vec4(0.0f, 0.0f, 0.0f, 0.0f);
        (*result.mParameters)[3] = vsg::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        (*result.mParameters)[4] = vsg::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        stateGroup->add(vsg::PushConstants::create(VK_SHADER_STAGE_FRAGMENT_BIT, 0, result.mParameters.get()));
        stateGroup->addChild(vsg::Draw::create(3, 1, 0, 0));

        result.mRoot = vsg::Switch::create();
        result.mRoot->addChild(false, stateGroup);
        return result;
    }

    vsg::ref_ptr<vsg::Node> SkyBackdrop::node() const noexcept
    {
        return mRoot;
    }

    void SkyBackdrop::update(
        const RenderCore::FrameEnvironmentState& environment, const RenderCore::FrameView& view) noexcept
    {
        if (!*this)
            return;

        mRoot->setAllChildren(environment.skyEnabled && !environment.interior);
        const glm::vec4 sunView = view.current.view * glm::vec4(-environment.sunDirection, 0.0f);
        const glm::vec4 sunClip = view.current.projection.matrix * sunView;
        const bool sunInFront = environment.sunVisible && sunClip.w > 0.0001f;
        const glm::vec2 sunNdc = sunInFront ? glm::vec2(sunClip) / sunClip.w : glm::vec2(0.0f);
        const float aspect = static_cast<float>(view.extent.width) / static_cast<float>(view.extent.height);
        const vsg::vec4 values[5]{ colorValue(environment.skyColor), colorValue(environment.fogColor),
            vsg::vec4(std::clamp(environment.nightSkyFactor, 0.0f, 1.0f), environment.storm ? 1.0f : 0.0f,
                std::clamp(environment.precipitationIntensity, 0.0f, 1.0f),
                std::clamp(environment.cloudBlendFactor, 0.0f, 1.0f)),
            vsg::vec4(sunNdc.x * 0.5f + 0.5f, sunNdc.y * 0.5f + 0.5f, sunInFront ? 1.0f : 0.0f, aspect),
            colorValue(environment.sunDiscColor) };
        bool changed = false;
        for (std::size_t i = 0; i < 5; ++i)
        {
            changed = changed || (*mParameters)[i] != values[i];
            (*mParameters)[i] = values[i];
        }
        if (changed)
            mParameters->dirty();
    }
}
