#ifndef OPENMW_RENDERCORE_DIAGNOSTICFRAMEIDENTITY_H
#define OPENMW_RENDERCORE_DIAGNOSTICFRAMEIDENTITY_H

#include <osg/Texture>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace RenderCore::Temporal
{
    // Identifies an actual owner/publication event. The caller must issue only
    // at that event; a number alone proves neither native currentness nor GL
    // allocation. Separate issuers keep different provenance roles distinct.
    class DiagnosticIdentitySource final
    {
    public:
        std::uint64_t issue() noexcept
        {
            auto serial = mSerial.load(std::memory_order_relaxed);
            do
            {
                if (serial == std::numeric_limits<std::uint64_t>::max()) return 0;
            } while (!mSerial.compare_exchange_weak(serial, serial + 1, std::memory_order_relaxed));
            return serial + 1;
        }
    private:
        friend struct DiagnosticFrameIdentityTestAccess;
        std::atomic<std::uint64_t> mSerial{0};
    };
    inline std::uint64_t issueDiagnosticPublisherIdentity() noexcept
    {
        static DiagnosticIdentitySource source;
        return source.issue();
    }
    inline std::uint64_t issueDiagnosticAcquiredCapture() noexcept
    {
        static DiagnosticIdentitySource source;
        return source.issue();
    }
    inline std::uint64_t issueDiagnosticHistoryOwner() noexcept
    {
        static DiagnosticIdentitySource source;
        return source.issue();
    }

    // Issued by the actual PostProcessor target replacement/republication,
    // not by a consumer observing a familiar texture pointer or GL name.
    // Retains the exact CPU resource owners across delayed acquired draw.
    // This is a source-group publication generation, NOT a driver allocation
    // generation or the coarse TemporalMotion history-reset resourceEpoch.
    class DiagnosticTargetGroup final
    {
    public:
        enum class Event { TargetReplacement, OwnerRepublishedAfterRelease };
        static std::shared_ptr<DiagnosticTargetGroup> issue(osg::Texture* scene, osg::Texture* depth,
            Event event = Event::TargetReplacement) noexcept
        {
            if (!scene || !depth || scene == depth) return {};
            static DiagnosticIdentitySource source;
            const auto serial = source.issue();
            if (!serial) return {};
            try { return std::shared_ptr<DiagnosticTargetGroup>(new DiagnosticTargetGroup(serial, scene, depth, event)); }
            catch (...) { return {}; } // Optional identity refusal cannot break ordinary rendering.
        }
        std::uint64_t generation() const noexcept { return mGeneration; }
        Event event() const noexcept { return mEvent; }
        bool valid() const noexcept { return !mInvalidated.load(std::memory_order_acquire); }
        bool matches(const osg::Texture* scene, const osg::Texture* depth) const noexcept
        { return valid() && scene == mScene.get() && depth == mDepth.get(); }
        const osg::Texture* scene() const noexcept { return mScene.get(); }
        const osg::Texture* depth() const noexcept { return mDepth.get(); }
        void invalidate() noexcept { mInvalidated.store(true, std::memory_order_release); }
        DiagnosticTargetGroup(const DiagnosticTargetGroup&) = delete;
        DiagnosticTargetGroup& operator=(const DiagnosticTargetGroup&) = delete;
    private:
        DiagnosticTargetGroup(std::uint64_t generation, osg::Texture* scene, osg::Texture* depth, Event event)
            : mGeneration(generation), mEvent(event), mScene(scene), mDepth(depth) {}
        const std::uint64_t mGeneration;
        const Event mEvent;
        const osg::ref_ptr<osg::Texture> mScene, mDepth;
        std::atomic<bool> mInvalidated{false};
    };
    struct DiagnosticAcquiredIdentity
    {
        std::uint64_t publisher = 0, capture = 0;
        std::shared_ptr<DiagnosticTargetGroup> targets;
        bool matches(const osg::Texture* scene, const osg::Texture* depth) const noexcept
        { return publisher && capture && targets && targets->matches(scene, depth); }
    };
}
#endif
