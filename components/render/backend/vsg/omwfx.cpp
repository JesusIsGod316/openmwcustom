#include "omwfx.hpp"
#include "fximagestate.hpp"
#include "viewpipelinebinding.hpp"
#include "vsgsubmission.hpp"
#include <components/fx/stateupdater.hpp>

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

        VkSamplerAddressMode wrap(Fx::NativeWrap mode)
        {
            switch (mode)
            {
                case Fx::NativeWrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
                case Fx::NativeWrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                case Fx::NativeWrap::ClampBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
                case Fx::NativeWrap::ClampEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            }
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }

        vsg::ref_ptr<vsg::Sampler> sampler(Fx::NativeSampler settings = {}, unsigned levels = 1)
        {
            auto result = vsg::Sampler::create();
            result->minFilter = settings.min == Fx::NativeFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            result->magFilter = settings.mag == Fx::NativeFilter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
            result->mipmapMode = settings.mipmap == Fx::NativeMipmapMode::Linear
                ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            result->maxLod = settings.mipmap == Fx::NativeMipmapMode::None ? 0.0f : static_cast<float>(levels - 1);
            result->addressModeU = wrap(settings.u);
            result->addressModeV = wrap(settings.v);
            result->addressModeW = wrap(settings.w);
            return result;
        }

        VkFormat targetFormat(Fx::NativeImageFormat value)
        {
            switch (value)
            {
                case Fx::NativeImageFormat::R8: return VK_FORMAT_R8_UNORM;
                case Fx::NativeImageFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
                case Fx::NativeImageFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
                case Fx::NativeImageFormat::Rg8: return VK_FORMAT_R8G8_UNORM;
                case Fx::NativeImageFormat::Rg16Float: return VK_FORMAT_R16G16_SFLOAT;
                case Fx::NativeImageFormat::Rg32Float: return VK_FORMAT_R32G32_SFLOAT;
                case Fx::NativeImageFormat::Rgb8:
                case Fx::NativeImageFormat::Rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
                case Fx::NativeImageFormat::Rgb16Float:
                case Fx::NativeImageFormat::Rgba16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
                case Fx::NativeImageFormat::Rgb32Float:
                case Fx::NativeImageFormat::Rgba32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
            }
            throw std::runtime_error("OMWFX target format is invalid");
        }

        VkBlendFactor blendFactor(Fx::NativeBlendFactor value)
        {
            switch (value)
            {
                case Fx::NativeBlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
                case Fx::NativeBlendFactor::One: return VK_BLEND_FACTOR_ONE;
                case Fx::NativeBlendFactor::SourceColor: return VK_BLEND_FACTOR_SRC_COLOR;
                case Fx::NativeBlendFactor::OneMinusSourceColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case Fx::NativeBlendFactor::DestinationColor: return VK_BLEND_FACTOR_DST_COLOR;
                case Fx::NativeBlendFactor::OneMinusDestinationColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case Fx::NativeBlendFactor::SourceAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
                case Fx::NativeBlendFactor::OneMinusSourceAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case Fx::NativeBlendFactor::DestinationAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
                case Fx::NativeBlendFactor::OneMinusDestinationAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case Fx::NativeBlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
                case Fx::NativeBlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
                case Fx::NativeBlendFactor::ConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                case Fx::NativeBlendFactor::OneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
                case Fx::NativeBlendFactor::SourceAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            }
            throw std::runtime_error("OMWFX blend factor is invalid");
        }

        VkBlendOp blendOperation(Fx::NativeBlendOperation value)
        {
            switch (value)
            {
                case Fx::NativeBlendOperation::Add: return VK_BLEND_OP_ADD;
                case Fx::NativeBlendOperation::Subtract: return VK_BLEND_OP_SUBTRACT;
                case Fx::NativeBlendOperation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
                case Fx::NativeBlendOperation::Minimum: return VK_BLEND_OP_MIN;
                case Fx::NativeBlendOperation::Maximum: return VK_BLEND_OP_MAX;
            }
            throw std::runtime_error("OMWFX blend equation is invalid");
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
            std::map<std::tuple<std::uintptr_t, unsigned, bool>, vsg::ref_ptr<vsg::Image>> loadedImages;

            void reserve(std::size_t size)
            {
                if (size > ImageBudget || bytes > ImageBudget - size)
                    throw std::runtime_error("OMWFX image plan exceeds the 384 MiB safety budget");
                bytes += size;
            }

            Target target(unsigned width, unsigned height, Fx::NativeImageFormat format, bool mipmaps,
                VkClearColorValue clear, const Fx::NativeSampler* settings = nullptr)
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
                if (format == Fx::NativeImageFormat::Rgb8 || format == Fx::NativeImageFormat::Rgb16Float
                    || format == Fx::NativeImageFormat::Rgb32Float)
                {
                    allocated = vsg::ImageView::create(result.image, VK_IMAGE_ASPECT_COLOR_BIT);
                    allocated->subresourceRange.levelCount = image.mipLevels;
                    allocated->components.a = VK_COMPONENT_SWIZZLE_ONE;
                    allocated->compile(device);
                }
                result.sampled = vsg::ImageInfo::create(sampler(settings ? *settings : Fx::NativeSampler{}, image.mipLevels),
                    allocated, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
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

            ImageInfo texture(const Fx::NativeTexture& texture)
            {
                if (!texture.payload || !texture.payload->width || !texture.payload->height || !texture.payload->depth)
                    throw std::runtime_error("OMWFX native texture payload is invalid: " + texture.name);
                const auto& payload = *texture.payload;
                const bool one = texture.dimension == Fx::NativeTextureDimension::One;
                const bool three = texture.dimension == Fx::NativeTextureDimension::Three;
                const bool bytes8 = payload.storage == Fx::NativePixelStorage::Rgba8;
                const bool mipmaps = texture.sampler.mipmap != Fx::NativeMipmapMode::None;
                const auto levels = mipmaps ? 1u + static_cast<unsigned>(std::floor(std::log2(
                    std::max({payload.width, payload.height, payload.depth})))) : 1u;
                auto sampled = sampler(texture.sampler, levels);
                auto& gpuImage = loadedImages[{texture.imageIdentity, static_cast<unsigned>(texture.dimension), mipmaps}];
                if (!gpuImage)
                {
                    reserve(static_cast<std::size_t>(payload.width) * payload.height * payload.depth
                        * (bytes8 ? 4 : 16) * (mipmaps ? 2 : 1));
                    vsg::ref_ptr<vsg::Data> data;
                    vsg::Data::Properties properties;
                    properties.format = bytes8 ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_R32G32B32A32_SFLOAT;
                    properties.origin = payload.bottomLeft ? vsg::BOTTOM_LEFT : vsg::TOP_LEFT;
                    if (bytes8)
                    {
                        if (one) data = vsg::ubvec4Array::create(payload.width, properties);
                        else if (three) data = vsg::ubvec4Array3D::create(payload.width, payload.height, payload.depth, properties);
                        else data = vsg::ubvec4Array2D::create(payload.width, payload.height, properties);
                        if (payload.bytes.size() != static_cast<std::size_t>(payload.width) * payload.height * payload.depth * 4)
                            throw std::runtime_error("OMWFX byte texture payload size mismatch: " + texture.name);
                        std::memcpy(data->dataPointer(), payload.bytes.data(), payload.bytes.size());
                    }
                    else
                    {
                        if (one) data = vsg::vec4Array::create(payload.width, properties);
                        else if (three) data = vsg::vec4Array3D::create(payload.width, payload.height, payload.depth, properties);
                        else data = vsg::vec4Array2D::create(payload.width, payload.height, properties);
                        if (payload.floats.size() != static_cast<std::size_t>(payload.width) * payload.height * payload.depth * 4)
                            throw std::runtime_error("OMWFX float texture payload size mismatch: " + texture.name);
                        std::memcpy(data->dataPointer(), payload.floats.data(), payload.floats.size() * sizeof(float));
                    }
                    gpuImage = vsg::ImageInfo::create(sampled, data)->imageView->image;
                }
                auto result = vsg::ImageInfo::create(sampled, vsg::ImageView::create(gpuImage),
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (texture.sourceComponents == 1)
                    result->imageView->components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_ZERO,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
                else if (texture.sourceComponents == 2)
                    result->imageView->components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                        VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
                else if (texture.sourceComponents == 3)
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
                    throw std::runtime_error("OMWFX authored blending requires both factors: " + pass.name);
                if (pass.blendSource && pass.blendDestination)
                {
                    if (blend->attachments.empty()) blend->attachments.emplace_back();
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
            auto luminance = build.target(256, 256, Fx::NativeImageFormat::R16Float, true, {{0,0,0,0}});
            luminance.sampled->sampler->maxLod = 8;
            auto history = build.target(1, 1, Fx::NativeImageFormat::R32Float, false, {{1,0,0,0}});
            auto adapted = build.target(1, 1, Fx::NativeImageFormat::R32Float, false, {{1,0,0,0}});
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
                const auto [width, height] = target.size.get(static_cast<int>(extent.width), static_cast<int>(extent.height));
                auto resource = build.target(width, height, target.format, target.mipMap,
                    {{target.clearColor[0], target.clearColor[1], target.clearColor[2], target.clearColor[3]}}, &target.sampler);
                histories.emplace(name, resource);
                current.emplace(name, std::move(resource));
            }
            std::map<std::string, ImageInfo> textures;
            for (const auto& texture : technique.textures) textures.emplace(texture.name, build.texture(texture));
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
                        spec.format, spec.mipMap, {{0,0,0,0}}, &spec.sampler);
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
                        pingPong.push_back(build.target(extent.width, extent.height, Fx::NativeImageFormat::Rgba16Float, false, {{0,0,0,0}}));
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
        Fx::StateUpdater::updateNativeCameraSnapshot(state, &view.current.projection.matrix[0][0],
            &currentView[0][0], &previousView[0][0], static_cast<float>(view.current.projection.nearPlane),
            static_cast<float>(view.current.projection.farPlane), static_cast<float>(extent.width),
            static_cast<float>(extent.height));
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
