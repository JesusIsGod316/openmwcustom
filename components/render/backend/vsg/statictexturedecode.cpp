#include "statictexturedecode.hpp"

#include "statictexturequirks.hpp"

#include <vsg/core/Array2D.h>
#include <vsg/io/Options.h>
#include <vsg/utils/CoordinateSpace.h>
#include <vsg/utils/SharedObjects.h>
#include <vsgXchange/images.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <utility>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] std::string extensionHint(std::string_view sourceIdentity)
        {
            const std::size_t slash = sourceIdentity.find_last_of("/\\");
            const std::size_t dot = sourceIdentity.find_last_of('.');
            if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
                return {};

            std::string result(sourceIdentity.substr(dot));
            std::ranges::transform(result, result.begin(), [](unsigned char value) {
                return static_cast<char>(std::tolower(value));
            });
            if (result == ".targa")
                result = ".tga";
            return result;
        }

        [[nodiscard]] vsg::CoordinateSpace coordinateSpace(RenderCore::TextureColorSpace source) noexcept
        {
            return source == RenderCore::TextureColorSpace::Srgb ? vsg::CoordinateSpace::sRGB
                                                                  : vsg::CoordinateSpace::LINEAR;
        }

        [[nodiscard]] bool warningTexture(const RenderCore::TextureRecord& record) noexcept
        {
            return record.sourceIdentity == OpenMwWarningTextureSourceIdentity
                && record.contentIdentity == OpenMwWarningTextureContentIdentity;
        }

        [[nodiscard]] vsg::ref_ptr<vsg::Data> createWarningTexture(
            const RenderCore::TextureRealizationKey& key)
        {
            vsg::Data::Properties properties;
            properties.format = key.view.colorSpace == RenderCore::TextureColorSpace::Srgb
                ? VK_FORMAT_R8G8B8_SRGB
                : VK_FORMAT_R8G8B8_UNORM;
            properties.mipLevels = 1u;
            properties.origin = vsg::TOP_LEFT;
            return vsg::ubvec3Array2D::create(8u, 8u, vsg::ubvec3(255u, 0u, 255u), properties);
        }

        [[nodiscard]] bool readTgaKillAlphaHeader(std::istream& stream, bool& killAlpha)
        {
            std::array<std::uint8_t, 18> header{};
            stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
            if (stream.gcount() != static_cast<std::streamsize>(header.size()))
                return false;

            killAlpha = openMwDiscard16BitTgaAlpha(header);
            stream.clear();
            stream.seekg(0, std::ios::beg);
            return static_cast<bool>(stream);
        }

        [[nodiscard]] vsg::ref_ptr<vsg::Data> discardTgaAlpha(
            const vsg::ref_ptr<vsg::Data>& decoded, RenderCore::TextureColorSpace colorSpace)
        {
            auto* rgba = decoded ? dynamic_cast<vsg::ubvec4Array2D*>(decoded.get()) : nullptr;
            if (rgba == nullptr || rgba->properties.mipLevels > 1u)
                return {};

            vsg::Data::Properties properties;
            properties.format = colorSpace == RenderCore::TextureColorSpace::Srgb ? VK_FORMAT_R8G8B8_SRGB
                                                                                   : VK_FORMAT_R8G8B8_UNORM;
            properties.mipLevels = std::max<std::uint8_t>(1u, rgba->properties.mipLevels);
            properties.origin = vsg::TOP_LEFT;
            properties.imageViewType = rgba->properties.imageViewType;
            properties.dataVariance = rgba->properties.dataVariance;

            auto rgb = vsg::ubvec3Array2D::create(rgba->width(), rgba->height(), properties);
            if (!rgb)
                return {};
            for (std::size_t i = 0; i < rgba->size(); ++i)
            {
                const vsg::ubvec4 source = (*rgba)[i];
                rgb->set(i, vsg::ubvec3(source.r, source.g, source.b));
            }
            return rgb;
        }

        void applyOpenMwDxt1Detection(vsg::Data& data)
        {
            const VkFormat format = data.properties.format;
            if (format != VK_FORMAT_BC1_RGBA_UNORM_BLOCK && format != VK_FORMAT_BC1_RGBA_SRGB_BLOCK)
                return;
            if (!data.dataAvailable() || !data.contiguous() || data.valueSize() != 8u)
                return;

            std::size_t firstMipBlocks = static_cast<std::size_t>(data.width()) * data.height();
            if (data.dimensions() == 3u)
                firstMipBlocks *= data.depth();
            if (firstMipBlocks == 0u || firstMipBlocks > data.valueCount())
                return;

            const auto* bytes = static_cast<const std::uint8_t*>(data.dataPointer());
            const std::span<const std::uint8_t> firstMip(bytes, firstMipBlocks * 8u);
            if (openMwBc1UsesOneBitAlpha(firstMip))
                return;

            data.properties.format = format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ? VK_FORMAT_BC1_RGB_SRGB_BLOCK
                                                                             : VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        }
    }

    StaticTextureDecoder::StaticTextureDecoder(vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
        : mSharedObjects(sharedObjects ? std::move(sharedObjects) : vsg::SharedObjects::create())
        , mImages(vsgXchange::images::create())
    {
    }

    StaticTextureDecoder::~StaticTextureDecoder() = default;

    vsg::ref_ptr<vsg::Data> StaticTextureDecoder::decode(const RenderCore::TextureRecord& record,
        const RenderCore::TextureRealizationKey& key, const StaticTextureStreamOpener& opener) const
    {
        if (!key.valid() || record.contentIdentity.empty() || record.revision != key.revision)
            return {};

        if (warningTexture(record))
            return createWarningTexture(key);

        if (!mImages || !opener || record.sourceIdentity.empty())
            return {};

        const std::string extension = extensionHint(record.sourceIdentity);
        if (extension.empty())
            return {};

        try
        {
            Files::IStreamPtr stream = opener(record.sourceIdentity);
            if (!stream)
                return {};

            bool discardLegacyTgaAlpha = false;
            if (extension == ".tga" && !readTgaKillAlphaHeader(*stream, discardLegacyTgaAlpha))
                return {};

            auto options = vsg::Options::create();
            options->sharedObjects = mSharedObjects;
            options->extensionHint = extension;
            options->setValue("image_format", coordinateSpace(key.view.colorSpace));

            vsg::ref_ptr<vsg::Object> decoded = mImages->read(*stream, options);
            auto* rawData = decoded ? dynamic_cast<vsg::Data*>(decoded.get()) : nullptr;
            if (!rawData || !rawData->dataAvailable() || rawData->width() == 0u || rawData->height() == 0u)
                return {};

            vsg::ref_ptr<vsg::Data> data(rawData);
            if (discardLegacyTgaAlpha)
            {
                data = discardTgaAlpha(data, key.view.colorSpace);
                if (!data)
                    return {};
            }

            // VSG's canonical image origin is top-left. vsgXchange/stb_image
            // decodes ordinary raster formats into that row order, while its DDS
            // loader preserves the compressed block order expected by Vulkan.
            data->properties.origin = vsg::TOP_LEFT;
            applyOpenMwDxt1Detection(*data);
            return data;
        }
        catch (const std::exception&)
        {
            return {};
        }
    }

    StaticTextureResolver makeStaticTextureResolver(
        StaticTextureStreamOpener opener, vsg::ref_ptr<vsg::SharedObjects> sharedObjects)
    {
        auto decoder = std::make_shared<StaticTextureDecoder>(std::move(sharedObjects));
        return [decoder = std::move(decoder), opener = std::move(opener)](const RenderCore::TextureRecord& record,
                   const RenderCore::TextureRealizationKey& key) { return decoder->decode(record, key, opener); };
    }
}
