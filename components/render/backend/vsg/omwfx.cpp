#include "omwfx.hpp"
#include "fximagestate.hpp"
#include "viewpipelinebinding.hpp"
#include "vsgsubmission.hpp"
#include <components/fx/stateupdater.hpp>

#include <osg/Texture1D>
#include <osg/Texture3D>
#include <SDL3/SDL_opengl_glext.h>
#include <cmath>
#include <cstring>
#include <limits>

namespace RenderVsg
{
    namespace
    {
        using ImageInfo = vsg::ref_ptr<vsg::ImageInfo>;
        constexpr std::size_t ImageBudget = 384u * 1024u * 1024u;

        VkSamplerAddressMode wrap(osg::Texture::WrapMode mode)
        {
            switch (mode)
            {
                case osg::Texture::REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case osg::Texture::MIRROR: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case osg::Texture::CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
        }

        vsg::ref_ptr<vsg::Sampler> sampler(const osg::Texture* texture = nullptr, unsigned levels = 1)
        {
            auto result = vsg::Sampler::create();
            const auto min = texture ? texture->getFilter(osg::Texture::MIN_FILTER) : osg::Texture::LINEAR;
            const auto mag = texture ? texture->getFilter(osg::Texture::MAG_FILTER) : osg::Texture::LINEAR;
            result->minFilter = min == osg::Texture::NEAREST || min == osg::Texture::NEAREST_MIPMAP_NEAREST
                || min == osg::Texture::NEAREST_MIPMAP_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            result->magFilter = mag == osg::Texture::NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            result->mipmapMode = min == osg::Texture::LINEAR_MIPMAP_LINEAR || min == osg::Texture::NEAREST_MIPMAP_LINEAR
                ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            result->maxLod = min == osg::Texture::LINEAR || min == osg::Texture::NEAREST ? 0.0f : static_cast<float>(levels - 1);
            result->addressModeU = texture ? wrap(texture->getWrap(osg::Texture::WRAP_S)) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            result->addressModeV = texture ? wrap(texture->getWrap(osg::Texture::WRAP_T)) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            result->addressModeW = texture ? wrap(texture->getWrap(osg::Texture::WRAP_R)) : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            return result;
        }

        VkFormat targetFormat(int value)
        {
            switch (value)
            {
                case GL_RED: return VK_FORMAT_R8_UNORM;
                case GL_R16F: return VK_FORMAT_R16_SFLOAT;
                case GL_R32F: return VK_FORMAT_R32_SFLOAT;
                case GL_RG: return VK_FORMAT_R8G8_UNORM;
                case GL_RG16F: return VK_FORMAT_R16G16_SFLOAT;
                case GL_RG32F: return VK_FORMAT_R32G32_SFLOAT;
                case GL_RGB: case GL_RGBA: return VK_FORMAT_R8G8B8A8_UNORM;
                case GL_RGB16F: case GL_RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
                case GL_RGB32F: case GL_RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
                default: throw std::runtime_error("OMWFX target format is not a supported color attachment: " + std::to_string(value));
            }
        }

        VkBlendFactor blendFactor(osg::BlendFunc::BlendFuncMode mode)
        {
            switch (mode)
            {
                case osg::BlendFunc::ZERO: return VK_BLEND_FACTOR_ZERO;
                case osg::BlendFunc::ONE: return VK_BLEND_FACTOR_ONE;
                case osg::BlendFunc::SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR;
                case osg::BlendFunc::ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case osg::BlendFunc::DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR;
                case osg::BlendFunc::ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case osg::BlendFunc::SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
                case osg::BlendFunc::ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case osg::BlendFunc::DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA;
                case osg::BlendFunc::ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case osg::BlendFunc::CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
                case osg::BlendFunc::ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
                case osg::BlendFunc::CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                case osg::BlendFunc::ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
                case osg::BlendFunc::SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
                default:
                    throw std::runtime_error("OMWFX blend factor is not supported by the native Vulkan executor: "
                        + std::to_string(static_cast<unsigned int>(mode)));
            }
        }

        VkBlendOp blendOperation(osg::BlendEquation::Equation equation)
        {
            switch (equation)
            {
                case osg::BlendEquation::FUNC_ADD: return VK_BLEND_OP_ADD;
                case osg::BlendEquation::FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
                case osg::BlendEquation::FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
                case osg::BlendEquation::RGBA_MIN:
                case osg::BlendEquation::ALPHA_MIN:
                    return VK_BLEND_OP_MIN;
                case osg::BlendEquation::RGBA_MAX:
                case osg::BlendEquation::ALPHA_MAX:
                    return VK_BLEND_OP_MAX;
                default:
                    throw std::runtime_error("OMWFX blend equation is not supported by the native Vulkan executor: "
                        + std::to_string(static_cast<unsigned int>(equation)));
            }
        }

        struct Target
        {
            vsg::ref_ptr<vsg::Image> image;
            vsg::ref_ptr<vsg::ImageView> attachment;
            ImageInfo sampled;
            vsg::ref_ptr<vsg::Framebuffer> framebuffer;
        };

        struct Builder
        {
            vsg::Viewer& viewer;
            vsg::Device* device;
            vsg::ref_ptr<vsg::Group> commands;
            std::vector<std::unique_ptr<ViewCompileManager::Registration>>& registrations;
            std::size_t bytes = 0;
            // Share the GPU image, not just its CPU pixels. Different nearest/
            // linear samplers of Rafael's large sky image must not duplicate VRAM.
            std::map<std::tuple<const osg::Image*, unsigned, bool>, vsg::ref_ptr<vsg::Image>> loadedImages;

            void reserve(std::size_t size)
            {
                if (size > ImageBudget || bytes > ImageBudget - size)
                    throw std::runtime_error("OMWFX image plan exceeds the 384 MiB safety budget");
                bytes += size;
            }

            Target target(unsigned width, unsigned height, int format, bool mipmaps,
                VkClearColorValue clear, const osg::Texture* settings = nullptr)
            {
                if (!width || !height || width > 16384 || height > 16384)
                    throw std::runtime_error("Invalid OMWFX target extent");
                Target result;
                result.image = vsg::Image::create();
                auto& image = *result.image;
                image.imageType = VK_IMAGE_TYPE_2D;
                image.format = targetFormat(format);
                image.extent = {width, height, 1};
                image.mipLevels = mipmaps ? 1u + static_cast<unsigned>(std::floor(std::log2(std::max(width, height)))) : 1u;
                image.arrayLayers = 1;
                image.samples = VK_SAMPLE_COUNT_1_BIT;
                image.tiling = VK_IMAGE_TILING_OPTIMAL;
                image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
                // Conservative upper bound; memory is retained only by this
                // active graph and is released after a GPU-safe replacement.
                const unsigned texelBytes = image.format == VK_FORMAT_R8_UNORM ? 1
                    : image.format == VK_FORMAT_R16_SFLOAT || image.format == VK_FORMAT_R8G8_UNORM ? 2
                    : image.format == VK_FORMAT_R32_SFLOAT || image.format == VK_FORMAT_R16G16_SFLOAT
                        || image.format == VK_FORMAT_R8G8B8A8_UNORM ? 4
                    : image.format == VK_FORMAT_R32G32_SFLOAT || image.format == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 16;
                reserve(static_cast<std::size_t>(width) * height * texelBytes * (mipmaps ? 2 : 1));
                VkFormatProperties properties{};
                vkGetPhysicalDeviceFormatProperties(device->getPhysicalDevice()->vk(), image.format, &properties);
                const auto required = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                    | (mipmaps ? VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT
                        | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT : 0);
                if ((properties.optimalTilingFeatures & required) != required)
                    throw std::runtime_error("OMWFX target format lacks required device features");
                auto allocated = vsg::createImageView(device, result.image, VK_IMAGE_ASPECT_COLOR_BIT);
                result.attachment = vsg::ImageView::create(result.image, VK_IMAGE_ASPECT_COLOR_BIT);
                result.attachment->subresourceRange.levelCount = 1;
                result.attachment->compile(device);
                if (format == GL_RGB || format == GL_RGB16F || format == GL_RGB32F)
                {
                    allocated = vsg::ImageView::create(result.image, VK_IMAGE_ASPECT_COLOR_BIT);
                    allocated->subresourceRange.levelCount = image.mipLevels;
                    allocated->components.a = VK_COMPONENT_SWIZZLE_ONE;
                    allocated->compile(device);
                }
                result.sampled = vsg::ImageInfo::create(sampler(settings, image.mipLevels), allocated, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                auto color = vsg::defaultColorAttachment(image.format);
                color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
                color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                color.initialLayout = color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vsg::SubpassDescription pass;
                pass.colorAttachments = {{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT}};
                vsg::RenderPass::Dependencies dependencies{
                    {VK_SUBPASS_EXTERNAL, 0, FxSampleStages | VK_PIPELINE_STAGE_TRANSFER_BIT,
                        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
                    {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        FxSampleStages | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0}};
                auto renderPass = vsg::RenderPass::create(device, vsg::RenderPass::Attachments{color},
                    vsg::RenderPass::Subpasses{pass}, dependencies);
                result.framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{result.attachment}, width, height, 1);
                commands->addChild(FxInitializeImage::create(result.image, clear));
                return result;
            }

            ImageInfo texture(const osg::Texture& texture)
            {
                const auto* image = texture.getImage(0);
                if (!image || !image->data()) throw std::runtime_error("OMWFX texture has no image: " + texture.getName());
                // Reuse the same OSG 3.6 S3TC decoder used by ImageManager's
                // software fallback, once at asset publication, never per frame.
                if (image->isCompressed() && image->getPixelFormat() != GL_COMPRESSED_RGB_S3TC_DXT1_EXT
                    && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
                    && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
                    && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
                    throw std::runtime_error("OMWFX unsupported compressed texture format: " + texture.getName());
                const unsigned width = image->s(), height = image->t(), depth = image->r();
                const bool one = dynamic_cast<const osg::Texture1D*>(&texture) != nullptr;
                const bool three = dynamic_cast<const osg::Texture3D*>(&texture) != nullptr;
                if (!width || !height || !depth) throw std::runtime_error("OMWFX image has an empty extent");
                const bool bytes8 = image->isCompressed() || image->getDataType() == GL_UNSIGNED_BYTE;
                const auto minFilter = texture.getFilter(osg::Texture::MIN_FILTER);
                const bool mipmaps = minFilter != osg::Texture::NEAREST && minFilter != osg::Texture::LINEAR;
                const auto levels = mipmaps ? 1u + static_cast<unsigned>(std::floor(std::log2(std::max({width, height, depth})))) : 1u;
                auto sampled = sampler(&texture, levels);
                auto& gpuImage = loadedImages[{image, one ? 1u : three ? 3u : 2u, mipmaps}];
                if (!gpuImage)
                {
                reserve(static_cast<std::size_t>(width) * height * depth * (bytes8 ? 4 : 16) * (mipmaps ? 2 : 1));
                vsg::ref_ptr<vsg::Data> data;
                vsg::Data::Properties properties;
                properties.format = bytes8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32G32B32A32_SFLOAT;
                properties.origin = image->getOrigin() == osg::Image::BOTTOM_LEFT ? vsg::BOTTOM_LEFT : vsg::TOP_LEFT;
                if (bytes8)
                {
                    if (one) data = vsg::ubvec4Array::create(width, properties);
                    else if (three) data = vsg::ubvec4Array3D::create(width, height, depth, properties);
                    else data = vsg::ubvec4Array2D::create(width, height, properties);
                }
                else
                {
                    if (one) data = vsg::vec4Array::create(width, properties);
                    else if (three) data = vsg::vec4Array3D::create(width, height, depth, properties);
                    else data = vsg::vec4Array2D::create(width, height, properties);
                }
                std::size_t index = 0;
                for (unsigned z = 0; z < depth; ++z)
                    for (unsigned y = 0; y < height; ++y)
                        for (unsigned x = 0; x < width; ++x)
                        {
                            const auto color = image->getColor(x, y, z);
                            if (bytes8)
                            {
                                auto& output = static_cast<vsg::ubvec4*>(data->dataPointer())[index++];
                                for (unsigned c = 0; c < 4; ++c)
                                    output[c] = static_cast<unsigned char>(std::clamp(std::round(color[c] * 255.0f), 0.0f, 255.0f));
                            }
                            else static_cast<vsg::vec4*>(data->dataPointer())[index++] = vsg::vec4(color.r(), color.g(), color.b(), color.a());
                        }
                gpuImage = vsg::ImageInfo::create(sampled, data)->imageView->image;
                }
                auto result = vsg::ImageInfo::create(sampled, vsg::ImageView::create(gpuImage), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                const int internal = texture.getInternalFormat();
                if (internal == GL_RED || internal == GL_R16F || internal == GL_R32F)
                    result->imageView->components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
                else if (internal == GL_RG || internal == GL_RG16F || internal == GL_RG32F)
                    result->imageView->components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
                else if (internal == GL_RGB || internal == GL_RGB16F || internal == GL_RGB32F)
                    result->imageView->components.a = VK_COMPONENT_SWIZZLE_ONE;
                return result;
            }

            void draw(const Fx::NativePass& pass, const Target& target, const std::vector<ImageInfo>& images,
                vsg::ref_ptr<vsg::Data> state, vsg::ref_ptr<vsg::Data> lights, vsg::ref_ptr<vsg::Data> parameters)
            {
                const auto stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                vsg::DescriptorSetLayoutBindings bindings{
                    {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
                    {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr},
                    {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr}};
                vsg::Descriptors descriptors{vsg::DescriptorBuffer::create(state, 0),
                    vsg::DescriptorBuffer::create(lights, 1), vsg::DescriptorBuffer::create(parameters, 2)};
                for (unsigned i = 0; i < images.size(); ++i)
                {
                    bindings.push_back({i + 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr});
                    descriptors.push_back(vsg::DescriptorImage::create(vsg::ImageInfoList{images[i]}, i + 3));
                }
                auto layout = vsg::DescriptorSetLayout::create(bindings);
                auto pipelineLayout = vsg::PipelineLayout::create(vsg::DescriptorSetLayouts{layout}, vsg::PushConstantRanges{});
                auto raster = vsg::RasterizationState::create();
                raster->cullMode = VK_CULL_MODE_NONE;
                auto depth = vsg::DepthStencilState::create();
                depth->depthTestEnable = depth->depthWriteEnable = VK_FALSE;
                auto blend = vsg::ColorBlendState::create();
                if (pass.blendSource.has_value() != pass.blendDestination.has_value())
                    throw std::runtime_error("OMWFX authored blending requires both source and destination factors: "
                        + pass.name);
                if (pass.blendSource && pass.blendDestination)
                {
                    if (blend->attachments.empty())
                        blend->attachments.emplace_back();
                    auto& attachment = blend->attachments.front();
                    attachment.blendEnable = VK_TRUE;
                    attachment.srcColorBlendFactor = blendFactor(*pass.blendSource);
                    attachment.dstColorBlendFactor = blendFactor(*pass.blendDestination);
                    attachment.colorBlendOp = pass.blendEquation ? blendOperation(*pass.blendEquation) : VK_BLEND_OP_ADD;
                    attachment.srcAlphaBlendFactor = attachment.srcColorBlendFactor;
                    attachment.dstAlphaBlendFactor = attachment.dstColorBlendFactor;
                    attachment.alphaBlendOp = attachment.colorBlendOp;
                    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
                }
                auto pipeline = vsg::GraphicsPipeline::create(pipelineLayout, vsg::ShaderStages{
                    vsg::ShaderStage::create(VK_SHADER_STAGE_VERTEX_BIT, "main", pass.shaders.vertex),
                    vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", pass.shaders.fragment)},
                    vsg::GraphicsPipelineStates{vsg::VertexInputState::create(), vsg::InputAssemblyState::create(),
                        vsg::ViewportState::create(0, 0, target.image->extent.width, target.image->extent.height),
                        raster, vsg::MultisampleState::create(), blend, depth});
                auto group = vsg::StateGroup::create();
                group->add(ViewPipelineBinding::create(pipeline));
                group->add(vsg::BindDescriptorSet::create(VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0,
                    vsg::DescriptorSet::create(layout, descriptors)));
                group->addChild(vsg::Draw::create(3, 1, 0, 0));
                // VSG 1.1.15's compile traversal dereferences View::camera even
                // for a fullscreen pass without camera-dependent shaders.
                auto camera = vsg::Camera::create();
                camera->viewportState = vsg::ViewportState::create(0, 0, target.image->extent.width, target.image->extent.height);
                auto view = vsg::View::create(camera, group, static_cast<vsg::ViewFeatures>(0));
                view->viewDependentState = {};
                auto graph = vsg::RenderGraph::create();
                graph->framebuffer = target.framebuffer;
                graph->renderArea = {{0, 0}, {target.image->extent.width, target.image->extent.height}};
                graph->addChild(view);
                auto& manager = static_cast<ViewCompileManager&>(*viewer.compileManager);
                auto registration = manager.registerFramebufferView(*target.framebuffer, view);
                const auto compiled = compileForViewerView(viewer, *view, graph);
                if (!compiled) throw std::runtime_error("OMWFX pass compilation failed: " + pass.name + ": " + compiled.message);
                registrations.push_back(std::move(registration));
                commands->addChild(graph);
                if (target.image->mipLevels > 1) commands->addChild(FxGenerateMipmaps::create(target.image));
            }
        };

        vsg::ref_ptr<vsg::ubyteArray> buffer(std::size_t bytes)
        {
            auto result = vsg::ubyteArray::create(std::max<std::size_t>(16, (bytes + 15) / 16 * 16), std::uint8_t(0));
            result->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
            return result;
        }

        constexpr const char* FullscreenVertex = R"(#version 450
layout(location=0) out vec2 uv;
void main() { uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2); gl_Position=vec4(uv*2.0-1.0,0,1); }
)";
    }

    OmwFxRuntime::OmwFxRuntime(vsg::Viewer& viewer, vsg::Device* device, vsg::ref_ptr<vsg::ImageView> scene,
        vsg::ref_ptr<vsg::ImageView> depth, RenderCore::Extent2D dimensions, const Fx::NativeFrame& frame,
        vsg::ref_ptr<vsg::ubyteArray> lights)
        : commands(vsg::Group::create()), output(scene), chain(frame.chain), interior(frame.interior),
          underwater(frame.underwater), extent(dimensions), mState(buffer(frame.state.size())), mLights(lights)
    {
        if (!chain || !frame.enabled) return;
        Builder build{viewer, device, commands, mRegistrations};
        auto lastShader = vsg::ImageInfo::create(sampler(), scene, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        auto nearest = sampler();
        nearest->minFilter = nearest->magFilter = VK_FILTER_NEAREST;
        auto depthInfo = vsg::ImageInfo::create(nearest, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        auto black = vsg::ImageInfo::create(sampler(), vsg::vec4Array2D::create(1, 1, vsg::vec4(0, 0, 0, 0),
            vsg::Data::Properties{VK_FORMAT_R32G32B32A32_SFLOAT}));
        auto white = vsg::ImageInfo::create(sampler(), vsg::vec4Array2D::create(1, 1, vsg::vec4(1, 1, 1, 1),
            vsg::Data::Properties{VK_FORMAT_R32G32B32A32_SFLOAT}));
        ImageInfo eyeAdaptation = white;
        const auto active = [&](const Fx::NativeTechnique& technique)
        {
            return !(technique.flags & (interior ? Fx::Technique::Flag_Disable_Interiors : Fx::Technique::Flag_Disable_Exteriors))
                && !(technique.flags & (underwater ? Fx::Technique::Flag_Disable_Underwater : Fx::Technique::Flag_Disable_Abovewater));
        };
        const bool hdr = std::any_of(chain->techniques.begin(), chain->techniques.end(),
            [&](const auto& technique) { return active(technique) && technique.hdr; });
        if (hdr)
        {
            // Same log-luminance range and exponential adaptation law as the
            // existing OpenMW postprocessor. Entire reduction/history is GPU-side.
            auto luminance = build.target(256, 256, GL_R16F, true, {{0,0,0,0}});
            luminance.sampled->sampler->maxLod = 8;
            auto history = build.target(1, 1, GL_R32F, false, {{1,0,0,0}});
            auto adapted = build.target(1, 1, GL_R32F, false, {{1,0,0,0}});
            mExposure = vsg::vec4Value::create(vsg::vec4(0, frame.exposureSpeed, 0, 0));
            mExposure->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
            Fx::NativePass pass;
            pass.name = "native-luminance";
            pass.shaders.vertex = FullscreenVertex;
            pass.shaders.fragment = R"(#version 450
layout(location=0) in vec2 uv; layout(location=0) out vec4 color;
layout(set=0,binding=3) uniform sampler2D scene;
void main() { float lum=max(dot(texture(scene,uv).rgb,vec3(.2126,.7152,.0722)),.004);
color=vec4(clamp((log2(lum)+9.0)/13.0,0.0,1.0)); }
)";
            build.draw(pass, luminance, {lastShader}, mState, mLights, mExposure);
            pass.name = "native-eye-adaptation";
            pass.shaders.fragment = R"(#version 450
layout(location=0) in vec2 uv; layout(location=0) out vec4 color;
layout(set=0,binding=2,std140) uniform Params { vec4 exposure; };
layout(set=0,binding=3) uniform sampler2D luminance;
layout(set=0,binding=4) uniform sampler2D previous;
void main() { float current=exp2(textureLod(luminance,vec2(.5),8).r*13.0-9.0);
float prev=texture(previous,vec2(.5)).r;
color=vec4(prev+(current-prev)*(1.0-exp(-exposure.x*exposure.y))); }
)";
            build.draw(pass, adapted, {luminance.sampled, history.sampled}, mState, mLights, mExposure);
            commands->addChild(FxCopyImage::create(adapted.image, history.image));
            eyeAdaptation = adapted.sampled;
        }

        std::vector<Target> pingPong;
        for (std::size_t index = 0; index < chain->techniques.size(); ++index)
        {
            const auto& technique = chain->techniques[index];
            mParameters.push_back(buffer(technique.parameterBytes));
            if (!active(technique)) continue;
            std::map<std::string, Target> histories, current;
            for (const auto& [name, target] : technique.targets)
            {
                const auto [width, height] = target.mSize.get(static_cast<int>(extent.width), static_cast<int>(extent.height));
                auto resource = build.target(width, height, target.mTarget->getInternalFormat(), target.mMipMap,
                    {{target.mClearColor.r(), target.mClearColor.g(), target.mClearColor.b(), target.mClearColor.a()}}, target.mTarget);
                histories.emplace(name, resource);
                current.emplace(name, std::move(resource));
            }
            std::map<std::string, ImageInfo> textures;
            for (const auto& texture : technique.textures) textures.emplace(texture->getName(), build.texture(*texture));
            auto lastPass = lastShader;
            for (const auto& pass : technique.passes)
            {
                std::vector<ImageInfo> inputs;
                for (const auto& name : pass.shaders.samplers)
                {
                    if (name == "omw_SamplerLastShader") inputs.push_back(lastShader);
                    else if (name == "omw_SamplerLastPass") inputs.push_back(lastPass);
                    else if (name == "omw_SamplerDepth") inputs.push_back(depthInfo);
                    else if (name == "omw_EyeAdaptation") inputs.push_back(eyeAdaptation);
                    else if (name == "omw_SamplerNormals" || name == "omw_SamplerDistortion") inputs.push_back(black);
                    else if (auto it = current.find(name); it != current.end()) inputs.push_back(it->second.sampled);
                    else if (auto textureIt = textures.find(name); textureIt != textures.end()) inputs.push_back(textureIt->second);
                    else throw std::runtime_error("Unresolved OMWFX sampler: " + technique.name + '/' + name);
                }
                Target destination;
                if (!pass.target.empty())
                {
                    const auto& previous = current.at(pass.target);
                    const auto& spec = technique.targets.at(pass.target);
                    destination = build.target(previous.image->extent.width, previous.image->extent.height,
                        spec.mTarget->getInternalFormat(), spec.mMipMap, {{0,0,0,0}}, spec.mTarget);
                    // Preserve authored clear values, partial writes and blending
                    // while avoiding attachment/sampler feedback aliasing.
                    commands->addChild(FxCopyImage::create(previous.image, destination.image));
                }
                else
                {
                    auto candidate = std::find_if(pingPong.begin(), pingPong.end(), [&](const Target& target)
                    {
                        return target.sampled->imageView->image != lastShader->imageView->image
                            && target.sampled->imageView->image != lastPass->imageView->image;
                    });
                    if (candidate == pingPong.end())
                    {
                        pingPong.push_back(build.target(extent.width, extent.height, GL_RGBA16F, false, {{0,0,0,0}}));
                        destination = pingPong.back();
                    }
                    else destination = *candidate;
                }
                build.draw(pass, destination, inputs, mState, mLights, mParameters[index]);
                if (pass.target.empty()) lastPass = destination.sampled;
                else current.at(pass.target) = destination;
            }
            lastShader = lastPass;
            for (const auto& [name, history] : histories)
                if (current.at(name).image != history.image)
                    commands->addChild(FxCopyImage::create(current.at(name).image, history.image));
        }
        output = lastShader->imageView;
    }

    void OmwFxRuntime::update(const Fx::NativeFrame& frame, const RenderCore::FrameView& view,
        double /*simulationTime*/, double delta)
    {
        if (frame.state.size() > mState->size() || frame.state.size() < 5 * sizeof(glm::mat4))
            throw std::runtime_error("OMWFX frame-state layout mismatch");
        // The native camera already contains Vulkan's current reverse-Z/Y
        // convention. Publish ALL camera fields formerly filled by PingPongCull,
        // not only matrices: a zero eyePos makes weather/fog evaluate the wrong
        // ray even though inverse projection itself is correct.
        auto state = frame.state;
        const glm::mat4 currentView(view.current.view), previousView(view.previous.view);
        Fx::StateUpdater::updateNativeCameraSnapshot(state,
            osg::Matrixf(&view.current.projection.matrix[0][0]), osg::Matrixf(&currentView[0][0]),
            osg::Matrixf(&previousView[0][0]), static_cast<float>(view.current.projection.nearPlane),
            static_cast<float>(view.current.projection.farPlane), osg::Vec2f(extent.width, extent.height));
        std::memcpy(mState->dataPointer(), state.data(), state.size());
        mState->dirty();
        if (frame.parameters.size() != mParameters.size()) throw std::runtime_error("OMWFX parameter chain mismatch");
        for (std::size_t i = 0; i < mParameters.size(); ++i)
        {
            if (frame.parameters[i].size() > mParameters[i]->size()) throw std::runtime_error("OMWFX parameter buffer mismatch");
            std::memcpy(mParameters[i]->dataPointer(), frame.parameters[i].data(), frame.parameters[i].size());
            mParameters[i]->dirty();
        }
        if (mExposure)
        {
            mExposure->value().set(static_cast<float>(std::max(0.0, delta)), frame.exposureSpeed, 0, 0);
            mExposure->dirty();
        }
    }
}
