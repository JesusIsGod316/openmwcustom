#ifndef OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONIDENTITY_H
#define OPENMW_COMPONENTS_NIFRENDER_TRANSLATIONIDENTITY_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace NifRender
{
    // Increment whenever the backend-neutral interpretation of identical source
    // bytes changes. This keeps persistent/in-memory realization caches honest
    // across CP3B semantic contract changes without mixing source provenance into
    // the canonical content key.
    inline constexpr std::uint32_t TranslationSemanticSchemaRevision = 1u;

    enum class TranslationOptionBit : std::uint64_t
    {
        ShowMarkers = 1ull << 0,
    };

    [[nodiscard]] constexpr std::uint64_t translationOptionBit(TranslationOptionBit bit) noexcept
    {
        return static_cast<std::uint64_t>(bit);
    }

    struct TranslationSourceIdentity
    {
        // Normalized VFS/source provenance. Useful for diagnostics and invalidation
        // ownership, but deliberately excluded from content equality and backend
        // realization keys.
        std::string sourceIdentity;
        std::string contentIdentity;

        [[nodiscard]] bool valid() const noexcept
        {
            return !sourceIdentity.empty() && !contentIdentity.empty();
        }
    };

    struct TranslationContentKey
    {
        std::string contentIdentity;
        std::uint32_t semanticSchemaRevision = TranslationSemanticSchemaRevision;
        std::uint64_t optionBits = 0;

        [[nodiscard]] bool valid() const noexcept
        {
            return !contentIdentity.empty() && semanticSchemaRevision != 0u;
        }

        friend bool operator==(const TranslationContentKey&, const TranslationContentKey&) = default;
    };

    [[nodiscard]] inline TranslationContentKey makeTranslationContentKey(
        std::string contentIdentity, bool showMarkers = false)
    {
        TranslationContentKey key;
        key.contentIdentity = std::move(contentIdentity);
        if (showMarkers)
            key.optionBits |= translationOptionBit(TranslationOptionBit::ShowMarkers);
        return key;
    }

    // Deterministic FNV-1a fingerprint for cache bucketing/telemetry. Equality
    // must still compare the complete TranslationContentKey, so collisions can
    // never alias distinct assets. Unlike std::hash this value is stable across
    // processes and standard-library implementations.
    [[nodiscard]] inline std::uint64_t stableTranslationKeyFingerprint(const TranslationContentKey& key) noexcept
    {
        constexpr std::uint64_t offset = 14695981039346656037ull;
        constexpr std::uint64_t prime = 1099511628211ull;
        std::uint64_t hash = offset;
        auto observeByte = [&](std::uint8_t byte) {
            hash ^= byte;
            hash *= prime;
        };

        for (const unsigned char value : key.contentIdentity)
            observeByte(value);
        observeByte(0xffu);
        for (unsigned int shift = 0; shift < 32; shift += 8)
            observeByte(static_cast<std::uint8_t>((key.semanticSchemaRevision >> shift) & 0xffu));
        for (unsigned int shift = 0; shift < 64; shift += 8)
            observeByte(static_cast<std::uint8_t>((key.optionBits >> shift) & 0xffu));
        return hash;
    }

    struct TranslationContentKeyHash
    {
        [[nodiscard]] std::size_t operator()(const TranslationContentKey& key) const noexcept
        {
            return static_cast<std::size_t>(stableTranslationKeyFingerprint(key));
        }
    };
}

#endif
