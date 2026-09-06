#ifndef OPENMW_COMPONENTS_NIFRENDER_VFSIDENTITY_H
#define OPENMW_COMPONENTS_NIFRENDER_VFSIDENTITY_H

#include "translationidentity.hpp"

#include <components/files/hash.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>

namespace NifRender
{
    struct ResolvedVfsIdentity
    {
        VFS::Path::Normalized canonicalPath;
        std::string archiveIdentity;
        std::string contentIdentity;

        [[nodiscard]] bool valid() const noexcept
        {
            return !canonicalPath.empty() && !contentIdentity.empty();
        }

        [[nodiscard]] TranslationSourceIdentity sourceIdentity() const
        {
            return { std::string(canonicalPath.value()), contentIdentity };
        }
    };

    [[nodiscard]] inline std::string encodeOpenMwContentHash(const std::array<std::uint64_t, 2>& hash)
    {
        std::ostringstream stream;
        stream << "openmw128:" << std::hex << std::setfill('0') << std::setw(16) << hash[0] << std::setw(16)
               << hash[1];
        return stream.str();
    }

    // Resolve the same texture path compatibility rules used by the current
    // renderer, then hash the selected VFS payload. Archive/path provenance is
    // retained for diagnostics and invalidation, while dedup uses content bytes.
    // This means loose-file and archive aliases with identical bytes converge on
    // one realization identity without confusing which source won VFS priority.
    [[nodiscard]] inline ResolvedVfsIdentity resolveTextureVfsIdentity(
        VFS::Path::NormalizedView authoredPath, const VFS::Manager& vfs)
    {
        ResolvedVfsIdentity result;
        result.canonicalPath = Misc::ResourceHelpers::correctTexturePath(authoredPath, vfs);
        auto stream = vfs.find(result.canonicalPath);
        if (!stream)
            return result;

        result.archiveIdentity = vfs.getArchive(result.canonicalPath);
        result.contentIdentity = encodeOpenMwContentHash(Files::getHash(result.canonicalPath.value(), *stream));
        return result;
    }
}

#endif
