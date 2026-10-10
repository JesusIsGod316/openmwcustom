#ifndef OPENMW_COMPONENTS_RESOURCE_MATERIALIMAGEDEPENDENCIES_H
#define OPENMW_COMPONENTS_RESOURCE_MATERIALIMAGEDEPENDENCIES_H

#include <components/misc/strings/algorithm.hpp>
#include <components/vfs/manager.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace Resource
{
    struct MaterialImageDependency
    {
        VFS::Path::Normalized path;
        bool normalHeight = false;
    };
    inline std::optional<VFS::Path::Normalized> materialImageCompanion(const VFS::Manager& vfs,
        std::string_view diffuse, std::string_view pattern)
    {
        std::string filename(diffuse);
        Misc::StringUtils::replaceLast(filename, ".", std::string(pattern) + ".");
        VFS::Path::Normalized path(filename);
        if (vfs.exists(path)) return path;
        return {};
    }
    // Shared with ShaderVisitor: exact normalized VFS resolution and _nh
    // precedence. Preparation never substitutes a conversion or decoder.
    inline std::optional<MaterialImageDependency> materialNormalDependency(const VFS::Manager& vfs,
        std::string_view diffuse, std::string_view heightPattern, std::string_view normalPattern)
    {
        if (auto path = materialImageCompanion(vfs, diffuse, heightPattern)) return MaterialImageDependency{*path, true};
        if (auto path = materialImageCompanion(vfs, diffuse, normalPattern)) return MaterialImageDependency{*path, false};
        return {};
    }
}
#endif
