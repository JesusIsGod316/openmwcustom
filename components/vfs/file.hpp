#ifndef OPENMW_COMPONENTS_VFS_FILE_H
#define OPENMW_COMPONENTS_VFS_FILE_H

#include <filesystem>
#include <string>
#include <optional>

#include <components/files/istreamptr.hpp>

namespace VFS
{
    class File
    {
    public:
        virtual ~File() = default;

        virtual Files::IStreamPtr open() = 0;

        virtual std::filesystem::file_time_type getLastModified() const = 0;

        // Optional exact backing for last_write_time, not the virtual resource
        // name. Unknown/custom providers keep getLastModified() on the caller.
        virtual std::optional<std::filesystem::path> getMetadataPath() const { return {}; }

        virtual std::string getStem() const = 0;
    };
}

#endif
