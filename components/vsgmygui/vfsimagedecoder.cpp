#include "vfsimagedecoder.hpp"

#include <components/vfs/manager.hpp>

#include <vsg/core/CoordinateSpace.h>
#include <vsg/io/Options.h>
#include <vsgXchange/images.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>

namespace VsgMyGui
{
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
            data->properties.origin = vsg::TOP_LEFT;
            return vsg::ref_ptr<vsg::Data>(data);
        };
    }
}
