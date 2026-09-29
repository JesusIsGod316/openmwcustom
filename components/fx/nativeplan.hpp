#ifndef OPENMW_COMPONENTS_FX_NATIVEPLAN_HPP
#define OPENMW_COMPONENTS_FX_NATIVEPLAN_HPP

#include "technique.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <vector>

#include <osg/Texture1D>
#include <osg/Texture3D>
#include <SDL3/SDL_opengl_glext.h>

namespace Fx
{
    struct NativeParameter
    {
        std::size_t uniformIndex = 0;
        std::size_t offset = 0;
        std::size_t stride = 0;
        std::size_t count = 0;
    };

    enum class NativeBlendFactor : std::uint8_t
    {
        Zero,
        One,
        SourceColor,
        OneMinusSourceColor,
        DestinationColor,
        OneMinusDestinationColor,
        SourceAlpha,
        OneMinusSourceAlpha,
        DestinationAlpha,
        OneMinusDestinationAlpha,
        ConstantColor,
        OneMinusConstantColor,
        ConstantAlpha,
        OneMinusConstantAlpha,
        SourceAlphaSaturate,
    };

    enum class NativeBlendOperation : std::uint8_t
    {
        Add,
        Subtract,
        ReverseSubtract,
        Minimum,
        Maximum,
    };

    enum class NativeFilter : std::uint8_t { Nearest, Linear };
    enum class NativeMipmapMode : std::uint8_t { None, Nearest, Linear };
    enum class NativeWrap : std::uint8_t { Repeat, MirroredRepeat, ClampEdge, ClampBorder };
    enum class NativeTextureDimension : std::uint8_t { One = 1, Two = 2, Three = 3 };
    enum class NativePixelStorage : std::uint8_t { Rgba8, Rgba32Float };
    enum class NativeImageFormat : std::uint8_t
    {
        R8,
        R16Float,
        R32Float,
        Rg8,
        Rg16Float,
        Rg32Float,
        Rgb8,
        Rgb16Float,
        Rgb32Float,
        Rgba8,
        Rgba16Float,
        Rgba32Float,
    };

    struct NativeSampler
    {
        NativeFilter min = NativeFilter::Linear;
        NativeFilter mag = NativeFilter::Linear;
        NativeMipmapMode mipmap = NativeMipmapMode::None;
        NativeWrap u = NativeWrap::ClampEdge;
        NativeWrap v = NativeWrap::ClampEdge;
        NativeWrap w = NativeWrap::ClampEdge;
    };

    struct NativeTexturePayload
    {
        unsigned width = 0;
        unsigned height = 0;
        unsigned depth = 0;
        NativePixelStorage storage = NativePixelStorage::Rgba8;
        bool bottomLeft = false;
        std::vector<std::uint8_t> bytes;
        std::vector<float> floats;
    };

    struct NativeTexture
    {
        std::string name;
        NativeTextureDimension dimension = NativeTextureDimension::Two;
        NativeSampler sampler;
        unsigned sourceComponents = 4;
        std::uintptr_t imageIdentity = 0;
        std::shared_ptr<const NativeTexturePayload> payload;
    };

    struct NativeSize
    {
        std::optional<float> widthRatio;
        std::optional<float> heightRatio;
        std::optional<int> width;
        std::optional<int> height;

        [[nodiscard]] std::tuple<int, int> get(int sourceWidth, int sourceHeight) const
        {
            int scaledWidth = sourceWidth;
            int scaledHeight = sourceHeight;
            if (widthRatio) scaledWidth = static_cast<int>(sourceWidth * *widthRatio);
            else if (width) scaledWidth = *width;
            if (heightRatio && *heightRatio > 0.f) scaledHeight = static_cast<int>(sourceHeight * *heightRatio);
            else if (height) scaledHeight = *height;
            return {scaledWidth, scaledHeight};
        }
    };

    struct NativeRenderTarget
    {
        NativeImageFormat format = NativeImageFormat::Rgba8;
        NativeSize size;
        NativeSampler sampler;
        bool mipMap = false;
        std::array<float, 4> clearColor{0.f, 0.f, 0.f, 1.f};
    };

    struct NativePass
    {
        std::string name;
        std::string target;
        VulkanShaderSources shaders;
        std::optional<NativeBlendFactor> blendSource;
        std::optional<NativeBlendFactor> blendDestination;
        std::optional<NativeBlendOperation> blendEquation;
    };

    // An immutable load/reload-time plan. OSG importer objects are converted at
    // this boundary so native render backends consume plain values and owned
    // pixel payloads rather than scenegraph objects.
    struct NativeTechnique
    {
        std::string name;
        std::size_t flags = 0;
        bool hdr = false;
        bool lights = false;
        std::vector<NativePass> passes;
        std::vector<NativeTexture> textures;
        std::map<std::string, NativeRenderTarget> targets;
        std::vector<NativeParameter> parameters;
        std::size_t parameterBytes = 16;
    };

    struct NativeChain
    {
        std::uint64_t generation = 0;
        std::vector<NativeTechnique> techniques;
    };

    struct NativeFrame
    {
        std::shared_ptr<const NativeChain> chain;
        std::vector<char> state;
        std::vector<std::vector<char>> parameters;
        bool enabled = false;
        bool interior = false;
        bool underwater = false;
        float exposureSpeed = 1.0f;
    };

    inline NativeBlendFactor makeNativeBlendFactor(osg::BlendFunc::BlendFuncMode mode)
    {
        switch (mode)
        {
            case osg::BlendFunc::ZERO: return NativeBlendFactor::Zero;
            case osg::BlendFunc::ONE: return NativeBlendFactor::One;
            case osg::BlendFunc::SRC_COLOR: return NativeBlendFactor::SourceColor;
            case osg::BlendFunc::ONE_MINUS_SRC_COLOR: return NativeBlendFactor::OneMinusSourceColor;
            case osg::BlendFunc::DST_COLOR: return NativeBlendFactor::DestinationColor;
            case osg::BlendFunc::ONE_MINUS_DST_COLOR: return NativeBlendFactor::OneMinusDestinationColor;
            case osg::BlendFunc::SRC_ALPHA: return NativeBlendFactor::SourceAlpha;
            case osg::BlendFunc::ONE_MINUS_SRC_ALPHA: return NativeBlendFactor::OneMinusSourceAlpha;
            case osg::BlendFunc::DST_ALPHA: return NativeBlendFactor::DestinationAlpha;
            case osg::BlendFunc::ONE_MINUS_DST_ALPHA: return NativeBlendFactor::OneMinusDestinationAlpha;
            case osg::BlendFunc::CONSTANT_COLOR: return NativeBlendFactor::ConstantColor;
            case osg::BlendFunc::ONE_MINUS_CONSTANT_COLOR: return NativeBlendFactor::OneMinusConstantColor;
            case osg::BlendFunc::CONSTANT_ALPHA: return NativeBlendFactor::ConstantAlpha;
            case osg::BlendFunc::ONE_MINUS_CONSTANT_ALPHA: return NativeBlendFactor::OneMinusConstantAlpha;
            case osg::BlendFunc::SRC_ALPHA_SATURATE: return NativeBlendFactor::SourceAlphaSaturate;
            default: throw std::runtime_error("Unsupported native post-processing blend factor");
        }
    }

    inline NativeBlendOperation makeNativeBlendOperation(osg::BlendEquation::Equation equation)
    {
        switch (equation)
        {
            case osg::BlendEquation::FUNC_ADD: return NativeBlendOperation::Add;
            case osg::BlendEquation::FUNC_SUBTRACT: return NativeBlendOperation::Subtract;
            case osg::BlendEquation::FUNC_REVERSE_SUBTRACT: return NativeBlendOperation::ReverseSubtract;
            case osg::BlendEquation::RGBA_MIN:
            case osg::BlendEquation::ALPHA_MIN: return NativeBlendOperation::Minimum;
            case osg::BlendEquation::RGBA_MAX:
            case osg::BlendEquation::ALPHA_MAX: return NativeBlendOperation::Maximum;
            default: throw std::runtime_error("Unsupported native post-processing blend equation");
        }
    }

    inline NativeWrap makeNativeWrap(osg::Texture::WrapMode mode)
    {
        switch (mode)
        {
            case osg::Texture::REPEAT: return NativeWrap::Repeat;
            case osg::Texture::MIRROR: return NativeWrap::MirroredRepeat;
            case osg::Texture::CLAMP_TO_BORDER: return NativeWrap::ClampBorder;
            default: return NativeWrap::ClampEdge;
        }
    }

    inline NativeSampler makeNativeSampler(const osg::Texture& texture)
    {
        NativeSampler result;
        const auto min = texture.getFilter(osg::Texture::MIN_FILTER);
        const auto mag = texture.getFilter(osg::Texture::MAG_FILTER);
        result.min = min == osg::Texture::NEAREST || min == osg::Texture::NEAREST_MIPMAP_NEAREST
                || min == osg::Texture::NEAREST_MIPMAP_LINEAR
            ? NativeFilter::Nearest : NativeFilter::Linear;
        result.mag = mag == osg::Texture::NEAREST ? NativeFilter::Nearest : NativeFilter::Linear;
        result.mipmap = min == osg::Texture::LINEAR_MIPMAP_LINEAR || min == osg::Texture::NEAREST_MIPMAP_LINEAR
            ? NativeMipmapMode::Linear
            : min == osg::Texture::LINEAR_MIPMAP_NEAREST || min == osg::Texture::NEAREST_MIPMAP_NEAREST
                ? NativeMipmapMode::Nearest : NativeMipmapMode::None;
        result.u = makeNativeWrap(texture.getWrap(osg::Texture::WRAP_S));
        result.v = makeNativeWrap(texture.getWrap(osg::Texture::WRAP_T));
        result.w = makeNativeWrap(texture.getWrap(osg::Texture::WRAP_R));
        return result;
    }

    inline NativeImageFormat makeNativeImageFormat(int value)
    {
        switch (value)
        {
            case GL_RED: return NativeImageFormat::R8;
            case GL_R16F: return NativeImageFormat::R16Float;
            case GL_R32F: return NativeImageFormat::R32Float;
            case GL_RG: return NativeImageFormat::Rg8;
            case GL_RG16F: return NativeImageFormat::Rg16Float;
            case GL_RG32F: return NativeImageFormat::Rg32Float;
            case GL_RGB: return NativeImageFormat::Rgb8;
            case GL_RGB16F: return NativeImageFormat::Rgb16Float;
            case GL_RGB32F: return NativeImageFormat::Rgb32Float;
            case GL_RGBA: return NativeImageFormat::Rgba8;
            case GL_RGBA16F: return NativeImageFormat::Rgba16Float;
            case GL_RGBA32F: return NativeImageFormat::Rgba32Float;
            default: throw std::runtime_error("Unsupported native post-processing target format");
        }
    }

    inline unsigned nativeSourceComponents(int value)
    {
        switch (value)
        {
            case GL_RED:
            case GL_R16F:
            case GL_R32F: return 1;
            case GL_RG:
            case GL_RG16F:
            case GL_RG32F: return 2;
            case GL_RGB:
            case GL_RGB16F:
            case GL_RGB32F: return 3;
            default: return 4;
        }
    }

    inline NativeTexture makeNativeTexture(const osg::Texture& texture,
        std::map<const osg::Image*, std::shared_ptr<const NativeTexturePayload>>* shared = nullptr)
    {
        const auto* image = texture.getImage(0);
        if (!image || !image->data()) throw std::runtime_error("OMWFX texture has no image: " + texture.getName());
        if (image->isCompressed() && image->getPixelFormat() != GL_COMPRESSED_RGB_S3TC_DXT1_EXT
            && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
            && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
            && image->getPixelFormat() != GL_COMPRESSED_RGBA_S3TC_DXT5_EXT)
            throw std::runtime_error("OMWFX unsupported compressed texture format: " + texture.getName());
        const unsigned width = image->s(), height = image->t(), depth = image->r();
        if (!width || !height || !depth) throw std::runtime_error("OMWFX image has an empty extent");
        NativeTexture result;
        result.name = texture.getName();
        result.dimension = dynamic_cast<const osg::Texture1D*>(&texture) ? NativeTextureDimension::One
            : dynamic_cast<const osg::Texture3D*>(&texture) ? NativeTextureDimension::Three
            : NativeTextureDimension::Two;
        result.sampler = makeNativeSampler(texture);
        result.sourceComponents = nativeSourceComponents(texture.getInternalFormat());
        result.imageIdentity = reinterpret_cast<std::uintptr_t>(image);
        if (shared)
            if (const auto found = shared->find(image); found != shared->end())
            { result.payload = found->second; return result; }
        auto payload = std::make_shared<NativeTexturePayload>();
        payload->width = width; payload->height = height; payload->depth = depth;
        payload->bottomLeft = image->getOrigin() == osg::Image::BOTTOM_LEFT;
        const bool bytes8 = image->isCompressed() || image->getDataType() == GL_UNSIGNED_BYTE;
        payload->storage = bytes8 ? NativePixelStorage::Rgba8 : NativePixelStorage::Rgba32Float;
        const std::size_t pixels = static_cast<std::size_t>(width) * height * depth;
        if (bytes8) payload->bytes.resize(pixels * 4);
        else payload->floats.resize(pixels * 4);
        std::size_t index = 0;
        for (unsigned z = 0; z < depth; ++z)
            for (unsigned y = 0; y < height; ++y)
                for (unsigned x = 0; x < width; ++x)
                {
                    const auto color = image->getColor(x, y, z);
                    for (unsigned component = 0; component < 4; ++component)
                        if (bytes8)
                            payload->bytes[index++] = static_cast<std::uint8_t>(
                                std::clamp(std::round(static_cast<double>(color[component]) * 255.0), 0.0, 255.0));
                        else payload->floats[index++] = color[component];
                }
        result.payload = payload;
        if (shared) shared->emplace(image, payload);
        return result;
    }

    inline NativeRenderTarget makeNativeRenderTarget(const Types::RenderTarget& target)
    {
        NativeRenderTarget result;
        result.format = makeNativeImageFormat(target.mTarget->getInternalFormat());
        result.size = {target.mSize.mWidthRatio, target.mSize.mHeightRatio, target.mSize.mWidth, target.mSize.mHeight};
        result.sampler = makeNativeSampler(*target.mTarget);
        result.mipMap = target.mMipMap;
        result.clearColor = {target.mClearColor.r(), target.mClearColor.g(), target.mClearColor.b(), target.mClearColor.a()};
        return result;
    }

    inline NativeTechnique makeNativeTechnique(Technique& technique)
    {
        NativeTechnique result;
        result.name = technique.getName();
        result.flags = technique.getFlags();
        result.hdr = technique.getHDR();
        result.lights = technique.getLights();
        std::map<const osg::Image*, std::shared_ptr<const NativeTexturePayload>> payloads;
        for (const auto& texture : technique.getTextures())
            result.textures.push_back(makeNativeTexture(*texture, &payloads));
        for (const auto& [name, target] : technique.getRenderTargetsMap())
            result.targets.emplace(name, makeNativeRenderTarget(target));
        for (const auto& pass : technique.getPasses())
        {
            NativePass native{pass->getName(), pass->getTarget(), pass->getVulkanSources(technique)};
            if (pass->getBlendSource()) native.blendSource = makeNativeBlendFactor(*pass->getBlendSource());
            if (pass->getBlendDest()) native.blendDestination = makeNativeBlendFactor(*pass->getBlendDest());
            if (pass->getBlendEquation()) native.blendEquation = makeNativeBlendOperation(*pass->getBlendEquation());
            result.passes.push_back(std::move(native));
        }
        std::size_t cursor = 0;
        const auto& uniforms = technique.getUniformMap();
        for (std::size_t i = 0; i < uniforms.size(); ++i)
        {
            const auto& uniform = *uniforms[i];
            if (uniform.mSamplerType) continue;
            std::visit([&](const auto& value)
            {
                using T = typename std::decay_t<decltype(value)>::value_type;
                if (!value.isArray() && uniform.mStatic
                    && Settings::ShaderManager::get().getMode() != Settings::ShaderManager::Mode::Debug) return;
                constexpr std::size_t bytes = std::is_same_v<T, bool> ? 4 : sizeof(T);
                const std::size_t alignment = value.isArray() || bytes >= 12 ? 16 : bytes;
                cursor = (cursor + alignment - 1) / alignment * alignment;
                const auto stride = value.isArray() ? (bytes + 15) / 16 * 16 : bytes;
                const auto count = uniform.getNumElements();
                result.parameters.push_back({i, cursor, stride, count});
                cursor += stride * count;
            }, uniform.mData);
        }
        result.parameterBytes = std::max<std::size_t>(16, (cursor + 15) / 16 * 16);
        return result;
    }

    inline std::vector<char> packNativeParameters(const NativeTechnique& plan, Technique& technique)
    {
        std::vector<char> result(plan.parameterBytes, 0);
        for (const auto& field : plan.parameters)
        {
            const auto& uniform = technique.getUniformMap().at(field.uniformIndex);
            std::visit([&](const auto& value)
            {
                using T = typename std::decay_t<decltype(value)>::value_type;
                const auto copy = [&](std::size_t index, T item)
                {
                    const auto offset = field.offset + index * field.stride;
                    if constexpr (std::is_same_v<T, bool>)
                    {
                        const std::int32_t integer = item ? 1 : 0;
                        if (offset + sizeof(integer) > result.size()) throw std::logic_error("Stale FX parameter layout");
                        std::memcpy(result.data() + offset, &integer, sizeof(integer));
                    }
                    else
                    {
                        if (offset + sizeof(T) > result.size()) throw std::logic_error("Stale FX parameter layout");
                        std::memcpy(result.data() + offset, &item, sizeof(T));
                    }
                };
                if (value.isArray())
                {
                    if (value.getArray().size() != field.count) throw std::logic_error("FX array size changed without reload");
                    for (std::size_t i = 0; i < field.count; ++i) copy(i, value.getArray()[i]);
                }
                else copy(0, value.getValue());
            }, uniform->mData);
        }
        return result;
    }
}
#endif
