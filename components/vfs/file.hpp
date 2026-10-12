#ifndef OPENMW_COMPONENTS_VFS_FILE_H
#define OPENMW_COMPONENTS_VFS_FILE_H

#include <filesystem>
#include <string>
#include <optional>
#include <cstdint>

#include <components/files/istreamptr.hpp>

namespace VFS
{
    struct SourceOpenEstimate
    {
        std::uint64_t sourceBytes=0;
        // Conservative peak owned by open()/the resulting stream, including
        // optional refill backing. Admission evidence, never source identity.
        std::uint64_t openPeakBytes=0;
        // Positive only for a known reader whose open handle owns the exact
        // immutable source until close. Windows constrained readers already
        // use FILE_SHARE_READ (without write/delete sharing). This is an
        // estimated retained stream/handle allowance, not a kernel RAM census.
        std::uint64_t stableReaderBytes=0;
    };
    class File
    {
    public:
        virtual ~File() = default;

        virtual Files::IStreamPtr open() = 0;
        // Unknown/custom or compressed providers keep optional derived work on
        // the ordinary path. Implementations must not materialize source data.
        virtual std::optional<SourceOpenEstimate> getOpenEstimate() const { return {}; }

        virtual std::filesystem::file_time_type getLastModified() const = 0;

        // Optional exact backing for last_write_time, not the virtual resource
        // name. Unknown/custom providers keep getLastModified() on the caller.
        virtual std::optional<std::filesystem::path> getMetadataPath() const { return {}; }

        virtual std::string getStem() const = 0;
    };
}

#endif
