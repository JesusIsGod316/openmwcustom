#include "uipipeline.hpp"

#include <cstring>
#include <string>
#include <vector>

#include <components/debug/debuglog.hpp>
namespace RenderVsg
{
    namespace
    {
        // MyGUI's vertices arrive in clip space already; just pass position through, flipping Y for Vulkan's
        // Y-down NDC (MyGUI computes for a GL Y-up target). z is clamped into [0,1] so nothing is depth-clipped
        // (depth test is off anyway — layering is by draw order).
        const char* const kUiVertexSource = R"(
#version 450
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec4 fragColor;
layout(location = 1) out vec2 fragUV;

void main()
{
    gl_Position = vec4(inPosition.x, -inPosition.y, clamp(inPosition.z, 0.0, 1.0), 1.0);
    fragColor = inColor;
    fragUV = inUV;
}
)";

        const char* const kUiFragmentSource = R"(
#version 450
layout(location = 0) in vec4 fragColor;
layout(location = 1) in vec2 fragUV;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) out vec4 outColor;

void main()
{
    outColor = texture(tex, fragUV) * fragColor;
}
)";

        // One interleaved GUI vertex, byte-identical to MyGUI::Vertex.
        struct GuiVertex
        {
            float x, y, z;
            uint32_t color; // ColourABGR: byte0=R,1=G,2=B,3=A (matches VK_FORMAT_R8G8B8A8_UNORM on little-endian)
            float u, v;
        };
        static_assert(sizeof(GuiVertex) == 24, "GuiVertex must match MyGUI::Vertex stride");

        uint32_t packABGR(int r, int g, int b, int a)
        {
            return (uint32_t(a) << 24) | (uint32_t(b) << 16) | (uint32_t(g) << 8) | uint32_t(r);
        }
    }

    UiPipeline createUiPipeline(uint32_t /*viewportWidth*/, uint32_t /*viewportHeight*/)
    {
        auto vertexShader
            = vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", std::string(kUiVertexSource));
        auto fragmentShader
            = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", std::string(kUiFragmentSource));
        if (!vertexShader || !fragmentShader)
        {
            Log(Debug::Error) << "VSG UI pipeline: shader compilation failed";
            return {};
        }

        // Set 0: binding 0 = the widget texture / font atlas (sampled in the fragment stage).
        auto descriptorSetLayout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            VkDescriptorSetLayoutBinding{
                0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr },
        });

        // The 128-byte vertex push range is unused by the shader (see the header) — it only keeps this layout
        // push-constant-compatible with the mesh pipeline while the overlay is recorded inside the 3D View.
        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{ descriptorSetLayout },
            vsg::PushConstantRanges{ VkPushConstantRange{ VK_SHADER_STAGE_VERTEX_BIT, 0, 128 } });

        // One interleaved binding (stride 24) → three attributes at MyGUI::Vertex offsets.
        vsg::VertexInputState::Bindings vertexBindings{
            VkVertexInputBindingDescription{ 0, sizeof(GuiVertex), VK_VERTEX_INPUT_RATE_VERTEX },
        };
        vsg::VertexInputState::Attributes vertexAttributes{
            VkVertexInputAttributeDescription{ 0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0 }, // position @0
            VkVertexInputAttributeDescription{ 1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12 }, // colour   @12
            VkVertexInputAttributeDescription{ 2, 0, VK_FORMAT_R32G32_SFLOAT, 16 }, // uv       @16
        };

        auto rasterization = vsg::RasterizationState::create();
        rasterization->cullMode = VK_CULL_MODE_NONE;

        // Straight-alpha over blend; premultiplied alpha for the alpha channel so nested render targets composite.
        auto colorBlend = vsg::ColorBlendState::create();
        colorBlend->attachments[0].blendEnable = VK_TRUE;
        colorBlend->attachments[0].srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        colorBlend->attachments[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlend->attachments[0].colorBlendOp = VK_BLEND_OP_ADD;
        colorBlend->attachments[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlend->attachments[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        colorBlend->attachments[0].alphaBlendOp = VK_BLEND_OP_ADD;

        // UI never tests or writes depth — it always draws over the 3D scene, in submission order.
        auto depth = vsg::DepthStencilState::create();
        depth->depthTestEnable = VK_FALSE;
        depth->depthWriteEnable = VK_FALSE;

        // Deliberately omit an explicit ViewportState. The UI lives inside the main VSG View and declares
        // viewport/scissor dynamic, so RenderGraph's live main-view viewport owns both state and resize updates.
        // Baking a private static viewport here would leave MyGUI stuck at startup size and would conflict with
        // the dynamic viewport commands emitted by the shared main render graph.
        vsg::GraphicsPipelineStates states{
            vsg::VertexInputState::create(vertexBindings, vertexAttributes),
            vsg::InputAssemblyState::create(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
            rasterization,
            vsg::MultisampleState::create(),
            colorBlend,
            depth,
            vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR),
        };

        auto sampler = vsg::Sampler::create();
        sampler->minFilter = VK_FILTER_LINEAR;
        sampler->magFilter = VK_FILTER_LINEAR;
        sampler->addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler->addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sampler->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler->minLod = 0.0f;
        sampler->maxLod = 0.0f;

        auto white = vsg::ubvec4Array2D::create(1, 1, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        (*white)(0, 0) = vsg::ubvec4(255, 255, 255, 255);

        UiPipeline result;
        result.pipelineLayout = pipelineLayout;
        result.descriptorSetLayout = descriptorSetLayout;
        result.sampler = sampler;
        result.whiteTexture = white;
        result.bindPipeline = vsg::BindGraphicsPipeline::create(
            vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{ vertexShader, fragmentShader }, states));
        return result;
    }

    vsg::ref_ptr<vsg::Node> buildUiTestOverlay(const UiPipeline& ui)
    {
        if (!ui)
            return {};

        std::vector<GuiVertex> verts;
        const auto quad = [&](float x0, float y0, float x1, float y1, uint32_t c) {
            // Two triangles; z = 0, full-texture uv (a white texture, so the quad shows its vertex colour).
            verts.push_back({ x0, y0, 0.f, c, 0.f, 0.f });
            verts.push_back({ x1, y0, 0.f, c, 1.f, 0.f });
            verts.push_back({ x1, y1, 0.f, c, 1.f, 1.f });
            verts.push_back({ x0, y0, 0.f, c, 0.f, 0.f });
            verts.push_back({ x1, y1, 0.f, c, 1.f, 1.f });
            verts.push_back({ x0, y1, 0.f, c, 0.f, 1.f });
        };
        // Clip space in MyGUI's GL convention (y=+1 top, y=-1 bottom); the shader flips Y for Vulkan.
        quad(-1.00f, -1.00f, 1.00f, -0.72f, packABGR(12, 12, 26, 150)); // translucent bottom bar
        quad(-0.96f, -0.95f, -0.62f, -0.78f, packABGR(255, 140, 38, 235)); // opaque orange "button" on the bar
        quad(-0.98f, 0.98f, -0.60f, 0.78f, packABGR(20, 120, 200, 180)); // translucent top-left panel

        const uint32_t count = static_cast<uint32_t>(verts.size());
        auto bytes = vsg::ubyteArray::create(count * sizeof(GuiVertex));
        std::memcpy(bytes->dataPointer(), verts.data(), count * sizeof(GuiVertex));

        auto info = vsg::ImageInfo::create(ui.sampler, ui.whiteTexture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto image = vsg::DescriptorImage::create(info, 0, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        auto descriptorSet = vsg::DescriptorSet::create(ui.descriptorSetLayout, vsg::Descriptors{ image });
        auto bindDescriptorSet
            = vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, ui.pipelineLayout, 0, descriptorSet);

        auto stateGroup = vsg::StateGroup::create();
        stateGroup->add(ui.bindPipeline);
        stateGroup->add(bindDescriptorSet);
        stateGroup->addChild(vsg::BindVertexBuffers::create(0, vsg::DataList{ bytes }));

        auto draw = vsg::Draw::create(count, 1, 0, 0);
        stateGroup->addChild(draw);
        return stateGroup;
    }
}
