#ifndef OPENMW_TOOLS_V4_CP3B3_VFS_MOUNT_PLAN_H
#define OPENMW_TOOLS_V4_CP3B3_VFS_MOUNT_PLAN_H

#include <filesystem>
#include <set>
#include <vector>

namespace Cp3b3
{
    enum class VfsMountKind
    {
        Archive,
        DataRoot,
    };

    struct VfsMountEntry
    {
        VfsMountKind kind = VfsMountKind::Archive;
        std::filesystem::path path;
    };

    // Mirrors components/vfs/registerarchives.cpp precedence without depending
    // on OpenMW VFS implementation types. Archives are registered first and
    // loose data roots second, therefore loose files override archive entries.
    // Within each class, later entries retain the higher-priority position.
    // Duplicate data roots are ignored after their first occurrence exactly as
    // registerArchives() does for Files::Collections paths.
    [[nodiscard]] inline std::vector<VfsMountEntry> buildVfsMountPlan(
        const std::vector<std::filesystem::path>& archives, const std::vector<std::filesystem::path>& dataRoots)
    {
        std::vector<VfsMountEntry> result;
        result.reserve(archives.size() + dataRoots.size());

        for (const std::filesystem::path& archive : archives)
            result.push_back({ VfsMountKind::Archive, archive });

        std::set<std::filesystem::path> seenDataRoots;
        for (const std::filesystem::path& root : dataRoots)
        {
            if (seenDataRoots.insert(root).second)
                result.push_back({ VfsMountKind::DataRoot, root });
        }

        return result;
    }
}

#endif
