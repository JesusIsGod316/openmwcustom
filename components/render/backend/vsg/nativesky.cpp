#include "nativesky.hpp"
#include "framecamera.hpp"
#include "livetextureimages.hpp"
#include "viewpipelinebinding.hpp"
#include <vsg/all.h>
#include <algorithm>
#include <stdexcept>

namespace RenderVsg
{
    namespace
    {
        // Port of files/shaders/compatibility/sky.{vert,frag}. Keep the native
        // atmosphere, cloud horizon fade, night opacity, moon two-pass equivalent,
        // and solar texture equations rather than synthesizing a different sky.
        const char* vertexSource = R"(#version 450
layout(push_constant) uniform PushConstants { mat4 projection; mat4 modelView; } pc;
layout(location=0) in vec3 position;
layout(location=1) in vec4 vertexColor;
layout(location=2) in vec2 uv;
layout(location=0) out vec4 passColor;
layout(location=1) out vec2 diffuseUV;
layout(set=0,binding=2,std140) uniform SkyParameters {
    mat4 uvTransform; vec4 diffuseColor; vec4 moonBlend; vec4 atmosphereFade; vec4 passOpacity; vec4 fogColor;
} sky;
void main() {
    vec4 clip = pc.projection * pc.modelView * vec4(position,1.0);
    // Camera-relative background is rendered before the world; keep sky meshes
    // beyond the gameplay far plane visible without writing its depth buffer.
    gl_Position = vec4(clip.xy,0.0,clip.w);
    passColor = vertexColor;
    diffuseUV = (sky.uvTransform * vec4(uv,0.0,1.0)).xy;
})";
        const char* fragmentSource = R"(#version 450
layout(location=0) in vec4 passColor;
layout(location=1) in vec2 diffuseUV;
layout(location=0) out vec4 outColor;
layout(set=0,binding=0) uniform sampler2D diffuseMap;
layout(set=0,binding=1) uniform sampler2D maskMap;
layout(set=0,binding=2,std140) uniform SkyParameters {
    mat4 uvTransform; vec4 diffuseColor; vec4 moonBlend; vec4 atmosphereFade; vec4 passOpacity; vec4 fogColor;
} sky;
void main() {
    int pass = int(sky.passOpacity.x);
    float opacity = sky.passOpacity.y;
    vec4 color = vec4(0.0);
    if (pass == 0) { color=sky.diffuseColor; color.a *= passColor.a; }
    else if (pass == 1) { color=texture(diffuseMap,diffuseUV); color.a *= passColor.a*opacity; }
    else if (pass == 2) {
        color=texture(diffuseMap,diffuseUV); color.a *= passColor.a*opacity;
        color.rgb=clamp(color.rgb*sky.diffuseColor.rgb,0.0,1.0);
        color=mix(vec4(sky.fogColor.rgb,color.a),color,passColor.a);
    } else if (pass == 3) {
        vec4 phase=texture(diffuseMap,diffuseUV), mask=texture(maskMap,diffuseUV);
        float maskAlpha=mask.a*sky.atmosphereFade.a;
        float phaseAlpha=phase.a*sky.atmosphereFade.a;
        color=vec4(mask.rgb*sky.atmosphereFade.rgb*maskAlpha + phase.rgb*sky.moonBlend.rgb*phaseAlpha,maskAlpha);
    } else if (pass == 4) { color=texture(diffuseMap,diffuseUV); color.a *= opacity; }
    outColor=color;
})";
        VkBlendFactor blendFactor(RenderCore::BlendFactor factor)
        {
            using B=RenderCore::BlendFactor;
            switch (factor) {
                case B::One: return VK_BLEND_FACTOR_ONE;
                case B::Zero: return VK_BLEND_FACTOR_ZERO;
                case B::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
                case B::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case B::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
                case B::OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case B::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
                case B::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case B::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
                case B::OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case B::SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            }
            throw std::invalid_argument("unknown native sky blend factor");
        }
        VkSamplerAddressMode address(RenderCore::TextureWrap mode)
        {
            using W=RenderCore::TextureWrap;
            switch (mode) {
                case W::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case W::Clamp: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                case W::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case W::Border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            }
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
        vsg::vec4 vector(const glm::vec4& value) { return {value.r,value.g,value.b,value.a}; }
    }
    NativeSky::NativeSky() : mRoot(vsg::Group::create()) {}

    bool NativeSky::sameResources(const RenderCore::SkyDrawSnapshot& a, const RenderCore::SkyDrawSnapshot& b) noexcept
    {
        if (a.identity!=b.identity || a.mesh!=b.mesh || a.pass!=b.pass || a.cullMode!=b.cullMode
            || a.frontFace!=b.frontFace || a.blendEnabled!=b.blendEnabled || a.sourceBlend!=b.sourceBlend
            || a.destinationBlend!=b.destinationBlend || a.textures.size()!=b.textures.size()) return false;
        for (std::size_t i=0; i<a.textures.size(); ++i)
            if (a.textures[i].texture!=b.textures[i].texture || a.textures[i].binding!=b.textures[i].binding) return false;
        return true;
    }

    NativeSky::Resident NativeSky::realize(const RenderCore::SkyDrawSnapshot& draw,
        const StaticTextureResolver& resolver, vsg::ref_ptr<vsg::SharedObjects> shared, std::string& diagnostic)
    {
        using namespace RenderCore;
        Resident result;
        if (!validSkyDraw(draw)) { diagnostic="invalid native sky resource snapshot"; return result; }
        auto parameters=vsg::vec4Array::create(9);
        parameters->properties.dataVariance=vsg::DYNAMIC_DATA;
        auto layout=vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            {0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {1,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
            {2,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}});
        auto pipelineLayout=vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{layout},
            vsg::PushConstantRanges{{VK_SHADER_STAGE_VERTEX_BIT,0,128}});
        auto white=vsg::ubvec4Array2D::create(1,1,vsg::ubvec4(255,255,255,255),vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        vsg::Descriptors descriptors;
        for (std::uint32_t unit=0; unit<2; ++unit)
        {
            auto sampler=vsg::Sampler::create();
            vsg::ref_ptr<vsg::Data> pixels=white;
            sampler->maxLod=0;
            if (unit<draw.textures.size())
            {
                const auto& texture=draw.textures[unit];
                auto binding=texture.binding;
                binding.texture=TextureHandle::fromParts(unit,1); // local realization only; cache is content-keyed
                pixels=resolver(texture.texture,makeTextureRealizationKey(binding,texture.texture));
                if (!pixels) { diagnostic="native sky texture realization failed: "+texture.texture.sourceIdentity; return {}; }
                sampler->addressModeU=address(binding.sampler.wrapU);
                sampler->addressModeV=address(binding.sampler.wrapV);
                sampler->minFilter=binding.sampler.minFilter==TextureFilter::Nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
                sampler->magFilter=binding.sampler.magFilter==TextureFilter::Nearest?VK_FILTER_NEAREST:VK_FILTER_LINEAR;
                sampler->mipmapMode=binding.sampler.mipmapMode==TextureMipmapMode::Nearest?VK_SAMPLER_MIPMAP_MODE_NEAREST:VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sampler->maxLod=binding.sampler.mipmapMode==TextureMipmapMode::None?0:VK_LOD_CLAMP_NONE;
            }
            if (shared) shared->share(sampler);
            descriptors.push_back(vsg::DescriptorImage::create(vsg::ImageInfoList{liveTextureImage(pixels,sampler)},unit,0));
        }
        descriptors.push_back(vsg::DescriptorBuffer::create(parameters,2,0));
        auto bindTextures=vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,
            vsg::DescriptorSet::create(layout,descriptors));
        auto positions=vsg::vec3Array::create(draw.mesh->positions.size());
        auto colors=vsg::vec4Array::create(draw.mesh->colors.size());
        auto uvs=vsg::vec2Array::create(draw.mesh->positions.size());
        for (std::size_t i=0; i<positions->size(); ++i)
        {
            const auto& p=draw.mesh->positions[i]; (*positions)[i]={p.x,p.y,p.z};
            (*colors)[i]=vector(draw.mesh->colors[i]);
            const auto& uv=draw.mesh->texCoordSets[0][i]; (*uvs)[i]={uv.x,uv.y};
        }
        auto indices=vsg::uintArray::create(draw.mesh->indices.size());
        std::copy(draw.mesh->indices.begin(),draw.mesh->indices.end(),indices->begin());
        if (shared) { shared->share(positions); shared->share(colors); shared->share(uvs); shared->share(indices); }
        auto placement=vsg::MatrixTransform::create();
        for (const auto& surface:draw.mesh->surfaces)
        {
            auto vertex=vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT,"main",std::string(vertexSource));
            auto fragment=vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT,"main",std::string(fragmentSource));
            auto raster=vsg::RasterizationState::create();
            raster->cullMode=draw.cullMode==CullMode::None?VK_CULL_MODE_NONE:draw.cullMode==CullMode::Front?VK_CULL_MODE_FRONT_BIT:VK_CULL_MODE_BACK_BIT;
            raster->frontFace=draw.frontFace==FrontFaceWinding::Clockwise?VK_FRONT_FACE_CLOCKWISE:VK_FRONT_FACE_COUNTER_CLOCKWISE;
            auto depth=vsg::DepthStencilState::create(); depth->depthTestEnable=VK_FALSE; depth->depthWriteEnable=VK_FALSE;
            auto blend=vsg::ColorBlendState::create();
            auto& b=blend->attachments[0]; b.blendEnable=draw.blendEnabled;
            b.srcColorBlendFactor=blendFactor(draw.sourceBlend); b.dstColorBlendFactor=blendFactor(draw.destinationBlend);
            b.srcAlphaBlendFactor=b.srcColorBlendFactor; b.dstAlphaBlendFactor=b.dstColorBlendFactor;
            auto attributes=vsg::VertexInputState::create(vsg::VertexInputState::Bindings{
                {0,sizeof(vsg::vec3),VK_VERTEX_INPUT_RATE_VERTEX},{1,sizeof(vsg::vec4),VK_VERTEX_INPUT_RATE_VERTEX},
                {2,sizeof(vsg::vec2),VK_VERTEX_INPUT_RATE_VERTEX}},vsg::VertexInputState::Attributes{
                {0,0,VK_FORMAT_R32G32B32_SFLOAT,0},{1,1,VK_FORMAT_R32G32B32A32_SFLOAT,0},{2,2,VK_FORMAT_R32G32_SFLOAT,0}});
            auto pipeline=vsg::GraphicsPipeline::create(pipelineLayout,vsg::ShaderStages{vertex,fragment},vsg::GraphicsPipelineStates{
                attributes,vsg::InputAssemblyState::create(surface.topology==PrimitiveTopology::TriangleStrip
                    ?VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST),raster,
                vsg::MultisampleState::create(),depth,blend,vsg::DynamicState::create(VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR)});
            ViewPipelineBinding::prepareCache(*pipeline);
            if (shared) shared->share(pipeline);
            auto group=vsg::StateGroup::create(); group->add(ViewPipelineBinding::create(pipeline)); group->add(bindTextures);
            auto vertices=vsg::VertexIndexDraw::create();
            vertices->assignArrays(vsg::DataList{positions,colors,uvs}); vertices->assignIndices(indices);
            vertices->indexCount=surface.indexCount; vertices->firstIndex=surface.firstIndex; vertices->instanceCount=1;
            group->addChild(vertices); placement->addChild(group);
        }
        result.contract=draw; result.placement=placement; result.parameters=parameters;
        return result;
    }

    bool NativeSky::prepare(const RenderCore::NativeSkySnapshot* snapshot,
        const RenderCore::FrameEnvironmentState& environment, const RenderCore::FrameView& view,
        RenderCore::FrameId frame, std::optional<RenderCore::FrameId> completed,
        const StaticTextureResolver& resolver, vsg::ref_ptr<vsg::SharedObjects> shared,
        const Compile& compile, std::string& diagnostic, bool visible)
    {
        mResidents.beginFrame(frame,completed);
        vsg::Group::Children children;
        if (visible && snapshot)
        {
            if (!RenderCore::validNativeSky(*snapshot)) { diagnostic="invalid native sky frame"; return false; }
            for (const auto& draw:snapshot->draws)
            {
                auto& resident=mResidents.acquire(draw.identity);
                if (!resident.placement || !sameResources(resident.contract,draw))
                {
                    Resident candidate=realize(draw,resolver,shared,diagnostic);
                    if (!candidate.placement || !compile(candidate.placement))
                    { if (diagnostic.empty()) diagnostic="native sky pipeline compilation failed"; return false; }
                    resident=std::move(candidate);
                }
                // No state from the main camera is baked into a reflected or
                // refracted sky. Each colour view gets its own camera-relative root.
                resident.placement->matrix=vsg::translate(vsg::dvec3(view.current.worldPosition.x,
                    view.current.worldPosition.y,view.current.worldPosition.z))*toVsgMatrix(draw.transform);
                auto& parameters=*resident.parameters;
                for (glm::length_t i=0; i<4; ++i) parameters[static_cast<std::size_t>(i)]=vector(draw.uvTransform[i]);
                parameters[4]=vector(draw.diffuseColor); parameters[5]=vector(draw.moonBlend);
                parameters[6]=vector(draw.atmosphereFade);
                parameters[7]=vsg::vec4{static_cast<float>(draw.pass),draw.opacity,0.f,0.f};
                parameters[8]=vsg::vec4{environment.fogColor.r,environment.fogColor.g,environment.fogColor.b,environment.fogColor.a};
                resident.parameters->dirty();
                children.push_back(resident.placement);
            }
        }
        mRoot->children=std::move(children);
        mResidents.collectUnused();
        return true;
    }
}
