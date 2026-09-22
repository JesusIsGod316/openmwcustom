#ifndef OPENMW_RESOURCE_CACHEDIAGNOSTICS_H
#define OPENMW_RESOURCE_CACHEDIAGNOSTICS_H
#include <components/debug/runtimediagnostics.hpp>
#include <osg/Image>
#include <array>
#include <string_view>
#include <unordered_map>

namespace Resource
{
    // Called only while the owning cache lock is held. No scenegraph traversal,
    // ref_ptr acquisition, file reads, or cache-policy changes. Payload totals
    // exclude object/allocator overhead and unsupported wrapper payloads.
    class CacheDiagnosticCensus
    {
    public:
        static constexpr std::size_t Limit = 32768;
        struct Top { std::array<char, 256> name{}; std::uint64_t bytes = 0, external = 0; };
        void add(std::string_view name, const osg::Object* object, double lastUse, double referenceTime)
        {
            if (entries >= Limit) { limited = true; return; }
            ++entries;
            bool external = object && object->referenceCount() > 1;
            Debug::RuntimeDiagnostics::PayloadInfo payload;
            if (const auto* image = dynamic_cast<const osg::Image*>(object))
                payload = { image->data(), image->getTotalSizeInBytesIncludingMipmaps(), 1 };
            else if (const auto* source = dynamic_cast<const Debug::RuntimeDiagnostics::PayloadSource*>(object))
                payload = source->diagnosticPayload();
            external = external || payload.externallyReferenced;
            if (external) ++externallyReferenced; else ++cacheOnly;
            if (lastUse > 0 && referenceTime >= lastUse)
            {
                const auto age = static_cast<std::uint64_t>((referenceTime - lastUse) * 1000000.0);
                oldestInactiveUs = (std::max)(oldestInactiveUs, external ? 0 : age);
            }
            if (!object) { ++nullEntries; return; }
            if (!payload.identity) { ++unmeasuredEntries; return; }
            auto [it, inserted] = mPayloads.emplace(payload.identity, std::pair(payload.bytes, external));
            if (inserted)
            {
                knownPayloadBytes += payload.bytes;
                sourceElements += payload.elements;
                if (!external) cacheOnlyPayloadBytes += payload.bytes;
                if (payload.bytes > top.back().bytes)
                {
                    top.back().bytes = payload.bytes;
                    top.back().external = external;
                    Debug::RuntimeDiagnostics::copyText(top.back().name, name);
                    std::sort(top.begin(), top.end(), [](const Top& a, const Top& b) { return a.bytes > b.bytes; });
                }
            }
            else
            {
                ++sharedPayloadReferences;
                if (external && !it->second.second)
                {
                    cacheOnlyPayloadBytes -= it->second.first;
                    it->second.second = true;
                }
            }
        }
        std::array<Top, 8> top{};
        std::uint64_t entries = 0, externallyReferenced = 0, cacheOnly = 0, nullEntries = 0;
        std::uint64_t knownPayloadBytes = 0, cacheOnlyPayloadBytes = 0, unmeasuredEntries = 0;
        std::uint64_t sharedPayloadReferences = 0, sourceElements = 0, oldestInactiveUs = 0;
        bool limited = false;
    private:
        std::unordered_map<const void*, std::pair<std::uint64_t, bool>> mPayloads;
    };
}
#endif
