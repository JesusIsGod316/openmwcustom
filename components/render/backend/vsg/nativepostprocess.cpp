#include "nativepostprocess.hpp"
#include "viewpipelinebinding.hpp"
#include <vsg/all.h>
#include <string>

namespace RenderVsg
{
    vsg::ref_ptr<vsg::Node> createNativePostProcess(vsg::ref_ptr<vsg::ImageView> color,
        vsg::ref_ptr<vsg::ImageView> sceneDepth, NativePostProcessMode mode)
    {
        if (!color || !sceneDepth) return {};
        const std::string vertex = R"(#version 450
layout(location=0) out vec2 uv;
void main() {
    vec2 p=vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv=p;
    gl_Position=vec4(p*2.0-1.0,0.0,1.0);
})";
        const std::string fragment = std::string("#version 450\n#define MODE ")
            + std::to_string(static_cast<int>(mode)) + R"(
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
layout(set=0,binding=0) uniform sampler2D sceneColor;
layout(set=0,binding=1) uniform sampler2D sceneDepth;
float luma(vec3 c) { return dot(c,vec3(0.2126,0.7152,0.0722)); }
void main() {
    vec4 center=texture(sceneColor,uv);
#if MODE == 2
    color=vec4(vec3(texture(sceneDepth,uv).r),1.0);
#elif MODE == 1
    // Small, explicitly named edge-AA filter. No exposure, bloom, color
    // grading, temporal history or claim of SMAA/.omwfx equivalence.
    vec2 pixel=1.0/vec2(textureSize(sceneColor,0));
    vec3 left=texture(sceneColor,uv-vec2(pixel.x,0)).rgb;
    vec3 right=texture(sceneColor,uv+vec2(pixel.x,0)).rgb;
    vec3 up=texture(sceneColor,uv-vec2(0,pixel.y)).rgb;
    vec3 down=texture(sceneColor,uv+vec2(0,pixel.y)).rgb;
    float mid=luma(center.rgb), l=luma(left), r=luma(right), u=luma(up), d=luma(down);
    float low=min(mid,min(min(l,r),min(u,d))), high=max(mid,max(max(l,r),max(u,d)));
    float contrast=high-low;
    if(contrast < max(0.03125,high*0.125)) { color=center; return; }
    vec2 tangent=vec2(-(d-u),r-l);
    float span=max(abs(tangent.x),abs(tangent.y));
    if(span < 1e-5) { color=center; return; }
    tangent=tangent/span*pixel*0.5;
    vec3 filtered=0.5*(texture(sceneColor,uv+tangent).rgb+texture(sceneColor,uv-tangent).rgb);
    color=vec4(clamp(filtered,min(center.rgb,min(min(left,right),min(up,down))),
        max(center.rgb,max(max(left,right),max(up,down)))),center.a);
#else
    color=center;
#endif
})";
        auto layout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}});
        auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{layout},
            vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT,0,128}});
        auto raster = vsg::RasterizationState::create();
        raster->cullMode = VK_CULL_MODE_NONE;
        auto depth = vsg::DepthStencilState::create();
        depth->depthTestEnable = depth->depthWriteEnable = VK_FALSE;
        auto pipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{
            vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT,"main",vertex),
            vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT,"main",fragment)},
            vsg::GraphicsPipelineStates{vsg::VertexInputState::create(),
                vsg::InputAssemblyState::create(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),
                vsg::ViewportState::create(0,0,1,1),raster,vsg::MultisampleState::create(),
                vsg::ColorBlendState::create(),depth,
                vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR)});
        pipeline->setValue("openmw.pipeline.family","native-postprocess");
        auto linear = vsg::Sampler::create();
        linear->minFilter = linear->magFilter = VK_FILTER_LINEAR;
        linear->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        linear->addressModeU = linear->addressModeV = linear->addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        auto nearest = vsg::Sampler::create();
        nearest->minFilter = nearest->magFilter = VK_FILTER_NEAREST;
        nearest->addressModeU = nearest->addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        auto descriptors = vsg::DescriptorSet::create(layout,vsg::Descriptors{
            vsg::DescriptorImage::create(vsg::ImageInfoList{vsg::ImageInfo::create(
                linear,color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)},0,0),
            vsg::DescriptorImage::create(vsg::ImageInfoList{vsg::ImageInfo::create(
                nearest,sceneDepth,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL)},1,0)});
        auto group = vsg::StateGroup::create();
        group->add(ViewPipelineBinding::create(pipeline));
        group->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,descriptors));
        group->addChild(vsg::Draw::create(3,1,0,0));
        return group;
    }
}
