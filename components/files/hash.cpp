#include "hash.hpp"

#include <components/debug/hitchtrace.hpp>

#include <smhasher/MurmurHash3.h>

#include <array>
#include <cstdint>
#include <istream>
#include <string>

namespace Files
{
    std::array<std::uint64_t, 2> getHash(std::string_view fileName, std::istream& stream)
    {
        Debug::HitchTrace::Scope hitchHash("asset.hash", Debug::HitchTrace::CpuTiming::Scoped, {}, fileName);
        hitchHash.retain();
        // Preserve the original block-dependent digest and stream rewind. These
        // are logical reads, not claims about disk service or cache hits.
        struct Counters
        {
            Debug::HitchTrace::Scope& scope;
            std::uint64_t bytes = 0;
            std::uint64_t blocks = 0;
            ~Counters()
            {
                scope.setMetric(0, "logical_bytes", bytes);
                scope.setMetric(1, "hash_blocks", blocks);
            }
        } counters{ hitchHash };
        hitchHash.setMetric(2, "rewound", 0);
        std::array<std::uint64_t, 2> hash{ 0, 0 };
        try
        {
            const auto start = stream.tellg();
            const auto exceptions = stream.exceptions();
            stream.exceptions(std::ios_base::badbit);
            while (stream)
            {
                std::array<char, 4096> value;
                stream.read(value.data(), value.size());
                const std::streamsize read = stream.gcount();
                if (read == 0)
                    break;
                std::array<std::uint64_t, 2> blockHash{ 0, 0 };
                MurmurHash3_x64_128(value.data(), static_cast<int>(read), hash.data(), blockHash.data());
                hash = blockHash;
                if (hitchHash.id())
                {
                    counters.bytes += static_cast<std::uint64_t>(read);
                    ++counters.blocks;
                }
            }
            stream.clear();
            stream.exceptions(exceptions);
            stream.seekg(start);
            hitchHash.setMetric(2, "rewound", !stream.fail());
        }
        catch (const std::exception& e)
        {
            std::string message = "Error while reading \"";
            message += fileName;
            message += "\" to get hash: ";
            message += e.what();
            throw std::runtime_error(message);
        }
        return hash;
    }
}
