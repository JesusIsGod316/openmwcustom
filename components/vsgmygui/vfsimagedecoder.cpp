#include "vfsimagedecoder.hpp"

#include <components/vfs/manager.hpp>

#include <vsg/io/Options.h>
#include <vsg/utils/CoordinateSpace.h>
#include <vsgXchange/images.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>

namespace VsgMyGui
{
    namespace
    {
        [[nodiscard]] VkFormat srgbFormat(VkFormat format) noexcept
        {
            switch (format)
            {
                case VK_FORMAT_R8G8B8_UNORM: return VK_FORMAT_R8G8B8_SRGB;
                case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
                case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
                case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
                case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
                case VK_FORMAT_BC2_UNORM_BLOCK: return VK_FORMAT_BC2_SRGB_BLOCK;
                case VK_FORMAT_BC3_UNORM_BLOCK: return VK_FORMAT_BC3_SRGB_BLOCK;
                case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_BC7_SRGB_BLOCK;
                default: return format;
            }
        }
    }

    ImageDecoder makeVfsImageDecoder(const VFS::Manager& vfs)
    {
        auto images = vsgXchange::images::create();
        return [&vfs, images = std::move(images)](const std::string& name) -> vsg::ref_ptr<vsg::Data> {
            const VFS::Path::Normalized path = VFS::Path::toNormalized(name);
            Files::IStreamPtr stream = vfs.find(path);
            if (!stream || !images)
                return {};

            std::string extension = std::filesystem::path(name).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            if (extension.empty())
                return {};

            auto options = vsg::Options::create();
            options->extensionHint = extension;
            // MyGUI's file-backed images are authored color assets. Ask the
            // decoder to preserve them as sRGB so Vulkan sampling performs the
            // required transfer to linear before UI modulation and the sRGB
            // swapchain does not re-encode already-gamma-space values.
            // Manual MyGUI textures (glyph/coverage/dynamic uploads) retain their
            // explicit UNORM allocation path and are intentionally unaffected.
            options->setValue("image_format", vsg::CoordinateSpace::sRGB);
            vsg::ref_ptr<vsg::Object> decoded = images->read(*stream, options);
            auto* data = decoded ? dynamic_cast<vsg::Data*>(decoded.get()) : nullptr;
            if (!data || !data->dataAvailable() || data->width() == 0 || data->height() == 0)
                return {};

            // Some decoder paths, notably DDS block-compressed assets, preserve
            // the source VkFormat even when image_format requests sRGB. MyGUI
            // has no separate neutral texture-view color-space contract, so make
            // the sampled image format authoritative here. This prevents an
            // authored sRGB menu texture from being sampled as linear UNORM and
            // then gamma-encoded a second time by the sRGB swapchain.
            data->properties.format = srgbFormat(data->properties.format);
            data->properties.origin = vsg::TOP_LEFT;
            return vsg::ref_ptr<vsg::Data>(data);
        };
    }
}
