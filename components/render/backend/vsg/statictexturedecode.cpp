#include "statictexturedecode.hpp"

#include <vsg/core/Array2D.h>
#include <vsg/io/Options.h>
#include <vsg/utils/CoordinateSpace.h>
#include <vsg/utils/SharedObjects.h>
#include <vsgXchange/images.h>

#include <algorithm>
#include <cctype>
#include <exception>
#include <istream>
#include <memory>
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

            auto options = vsg::Options::create();
            options->sharedObjects = mSharedObjects;
            options->extensionHint = extension;
            options->setValue("image_format", coordinateSpace(key.view.colorSpace));

            vsg::ref_ptr<vsg::Object> decoded = mImages->read(*stream, options);
            auto* data = decoded ? dynamic_cast<vsg::Data*>(decoded.get()) : nullptr;
            if (!data || !data->dataAvailable() || data->width() == 0u || data->height() == 0u)
                return {};

            data->properties.origin = vsg::TOP_LEFT;
            return vsg::ref_ptr<vsg::Data>(data);
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
