#ifndef OPENMW_COMPONENTS_RESOURCE_PRELOADESTIMATE_H
#define OPENMW_COMPONENTS_RESOURCE_PRELOADESTIMATE_H

#include "speculativebudget.hpp"
#include <array>
#include <cstdint>
#include <istream>
#include <span>

namespace Resource
{
    struct PreloadEstimate { std::uint64_t peak = 0, retained = 0; bool knownDimensions = false; };
    inline std::uint64_t saturatingProduct(std::uint64_t a, std::uint64_t b) noexcept
    { return b && a > UINT64_MAX / b ? UINT64_MAX : a * b; }
    inline std::uint64_t saturatingSum(std::uint64_t a, std::uint64_t b) noexcept
    { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }

    // Estimates, NOT a bound on a decoder's arbitrary internal allocations.
    // Read only a small header on an optional worker and restore the stream.
    inline PreloadEstimate estimateImage(std::span<const unsigned char> header, std::uint64_t encodedBytes)
    {
        const auto le = [&header](std::size_t at) -> std::uint64_t {
            return at + 4 <= header.size() ? std::uint64_t(header[at]) | (std::uint64_t(header[at+1]) << 8)
                | (std::uint64_t(header[at+2]) << 16) | (std::uint64_t(header[at+3]) << 24) : 0;
        };
        const auto be = [&header](std::size_t at) -> std::uint64_t {
            return at + 4 <= header.size() ? (std::uint64_t(header[at]) << 24) | (std::uint64_t(header[at+1]) << 16)
                | (std::uint64_t(header[at+2]) << 8) | std::uint64_t(header[at+3]) : 0;
        };
        std::uint64_t width = 0, height = 0, layers = 1;
        if (header.size() >= 128 && le(0) == 0x20534444 && le(4) == 124) // DDS
        {
            height = le(12); width = le(16);
            layers = (std::max)(std::uint64_t(1), le(24));
            if (le(112) & 0x200) layers = saturatingProduct(layers, 6); // cube
            if (le(84) == 0x30315844) // DX10 array header
            {
                if (header.size() < 148) return {256 * SpeculativeBudget::MiB, 0, false};
                layers = saturatingProduct(layers, (std::max)(std::uint64_t(1), le(140)));
                // DX10 cube flag can be present without old caps2.
                if ((le(136) & 4) && !(le(112) & 0x200)) layers = saturatingProduct(layers, 6);
            }
        }
        else if (header.size() >= 24 && be(0) == 0x89504e47 && be(4) == 0x0d0a1a0a)
        { width = be(16); height = be(20); }
        else if (header.size() >= 18 && (header[2] == 1 || header[2] == 2 || header[2] == 3
                || header[2] == 9 || header[2] == 10 || header[2] == 11)
            && (header[16] == 8 || header[16] == 16 || header[16] == 24 || header[16] == 32))
        { width = header[12] | (std::uint64_t(header[13]) << 8); height = header[14] | (std::uint64_t(header[15]) << 8); }
        const bool known = width && height;
        // Eight bytes/pixel and a 2x mip margin also cover skinny mip chains.
        const auto output = known ? saturatingProduct(saturatingProduct(saturatingProduct(width, height), layers), 16)
                                  : (std::max)(128 * SpeculativeBudget::MiB, saturatingProduct(encodedBytes, 16));
        return {saturatingSum(saturatingSum(saturatingProduct(output, 2), encodedBytes), 8 * SpeculativeBudget::MiB),
            output, known};
    }
    inline PreloadEstimate estimateStream(std::istream& stream, bool image)
    {
        const auto flags = stream.rdstate();
        const auto start = stream.tellg();
        if (start < 0) { stream.clear(flags); throw SpeculativeDeferred{}; }
        stream.seekg(0, std::ios::end);
        const auto end = stream.tellg();
        stream.clear(); stream.seekg(start);
        if (!stream) throw SpeculativeDeferred{};
        const auto encoded = end >= start ? static_cast<std::uint64_t>(end - start) : 0;
        PreloadEstimate result;
        if (image)
        {
            std::array<unsigned char, 148> header{};
            stream.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
            const auto read = static_cast<std::size_t>(stream.gcount());
            stream.clear(); stream.seekg(start);
            if (!stream) throw SpeculativeDeferred{};
            result = estimateImage(std::span(header.data(), read), encoded);
        }
        else
        {
            const auto retained = (std::max)(4 * SpeculativeBudget::MiB, saturatingProduct(encoded, 8));
            result = {saturatingProduct(retained, 2), retained, false};
        }
        stream.clear(flags);
        return result;
    }
}
#endif
