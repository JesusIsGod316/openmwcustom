#ifndef OPENMW_SCENEUTIL_ASSETPAGING_HPP
#define OPENMW_SCENEUTIL_ASSETPAGING_HPP

#include "generatedstaticlod.hpp"
#include "selectivehlod.hpp"
#include "pagingwork.hpp"
#include <components/resource/speculativebudget.hpp>
#include <components/resource/assetpolicy.hpp>
#include <components/vfs/pathutil.hpp>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace VFS { class Manager; }

namespace SceneUtil::AssetPaging
{
    inline constexpr unsigned Lod = Resource::AssetPolicy::VisualLod;
    inline constexpr unsigned Hlod = Resource::AssetPolicy::Hlod;
    inline constexpr unsigned Mask = Lod | Hlod;
    inline constexpr unsigned MaximumAttempts = 3;
    inline constexpr std::uint64_t RetryDelayMilliseconds = 5000;
    enum class Gate { Graph, Material, Transform, Layout, Capacity, NoReduction, Source, Occluder, Population, Count };
    struct LaneCounters
    {
        std::atomic_uint64_t sourceRequested{0}, sourceCertified{0}, sourceRejected{0}, snapshotDenied{0};
        std::atomic_uint64_t outerCpuDenied{0}, privateCpuDenied{0}, privateGpuDenied{0}, candidateAdmitted{0}, materialConflictInstances{0};
        std::atomic_uint64_t buildAttempts{0}, buildRefused{0}, ready{0};
        std::atomic_uint64_t refusals[static_cast<unsigned>(Gate::Count)]{};
        std::atomic_uint64_t details[static_cast<unsigned>(GeneratedStaticLod::Detail::Count)]{};
    };
    struct Counters
    {
        inline static LaneCounters lod, hlod;
        // OR of observed per-chunk outcomes; these are lane bitfields, not counts
        // of unique chunks. Workers may still progress at the stopped-draw receipt.
        inline static std::atomic_uint32_t attemptedMask{0}, completedMask{0}, terminalMask{0}, transientMask{0};
        inline static std::atomic_uint64_t retryAttempts{0}, retryDeferred{0}, retryExhausted{0};
    };
    inline LaneCounters& lane(unsigned bit) { return bit == Lod ? Counters::lod : Counters::hlod; }
    template<class F> void each(unsigned mask, F&& function)
    { for (unsigned bit : {Lod, Hlod}) if (mask & bit) function(lane(bit)); }
    class HostOwner final : public osg::Object
    {
    public:
        HostOwner() = default;
        explicit HostOwner(Resource::SpeculativeBudget::ChargePtr charge) : mCharge(std::move(charge)) {}
        HostOwner(const HostOwner& copy, const osg::CopyOp& op) : osg::Object(copy,op), mCharge(copy.mCharge) {}
        META_Object(SceneUtil, HostOwner)
    private:
        Resource::SpeculativeBudget::ChargePtr mCharge;
    };
    // Every allocation was admitted by private/cook stages. Transfer its
    // conservative retained estimate before the stages unwind or ICO sees it.
    // ObjectCache later shares the same root identity; an unpublished CompileSet
    // remains charged for as long as it owns the root, without a global retain.
    inline void retainHostOwner(osg::Node& node, std::uint64_t estimate)
    {
        const auto context = Resource::SpeculativeScope::capture();
        if (!estimate || !context.budget) return;
        auto charge = Resource::SpeculativeScope::track(context.budget,{&node,2u},estimate,false);
        node.getOrCreateUserDataContainer()->addUserObject(new HostOwner(std::move(charge)));
    }
    struct Outcome
    {
        unsigned allowed = Mask;
        unsigned attempted = 0, completed = 0, terminal = 0, transient = 0;
        std::shared_ptr<GeneratedStaticLod::Readiness> readiness;
        void defer(unsigned mask) { transient |= mask & Mask; }
        void finish(unsigned requested)
        {
            attempted |= requested & Mask;
            terminal = attempted & ~(completed | transient);
            Counters::attemptedMask.fetch_or(attempted); Counters::completedMask.fetch_or(completed);
            Counters::terminalMask.fetch_or(terminal); Counters::transientMask.fetch_or(transient);
        }
    };
    struct Quality
    {
        unsigned attempted = 0, completed = 0, terminal = 0, transient = 0, exhausted = 0;
        std::array<unsigned char, 2> attempts{};
        std::array<std::uint64_t, 2> retryAfter{};
        // Retain the small compile receipt, never its graph. It records whether
        // the exact candidate actually completed GL preparation before eviction.
        std::shared_ptr<GeneratedStaticLod::Readiness> readiness;
        unsigned settled() const
        {
            const unsigned prepared = readiness && readiness->ready.load(std::memory_order_acquire) ? completed & ~transient : 0;
            return terminal | exhausted | prepared;
        }
        bool satisfies(unsigned requested, std::uint64_t now) const;
        Quality after(const Outcome& outcome, std::uint64_t now) const;
        Quality withoutOwner() const;
    };
    std::uint64_t milliseconds();

    // Used by both actual terrain-preloader owners and the chain fixture. The
    // caller establishes the existing NearFuture SpeculativeScope first.
    class OptionalUpgradeScope
    {
    public:
        explicit OptionalUpgradeScope(std::atomic<bool>* abort)
            : mOverlap(overlap())
            , mWork(abort, PagingWorkScope::Phase::OptionalOptimization) {}
    private:
        static std::unique_ptr<Resource::SpeculativeScope::Stage> overlap()
        {
            try { return std::make_unique<Resource::SpeculativeScope::Stage>(128 * Resource::SpeculativeBudget::MiB,0); }
            catch (const Resource::SpeculativeDeferred&)
            {
                each(Resource::AssetPolicy::mask() & Mask,[](LaneCounters& value) {
                    ++value.outerCpuDenied; ++value.refusals[static_cast<unsigned>(Gate::Capacity)];
                });
                throw;
            }
        }
        std::unique_ptr<Resource::SpeculativeScope::Stage> mOverlap;
        PagingWorkScope mWork;
    };
    struct Source
    {
        std::string identity;
        std::size_t bytes = 0;
        std::size_t hostBytes = 0;
        unsigned eligible = 0;
    };
    std::optional<Source> prepareSource(const osg::Node& original, VFS::Path::NormalizedView model,
        const VFS::Manager& vfs, unsigned requested, Outcome& outcome);
    std::unique_ptr<Resource::SpeculativeScope::Stage> reservePrivateSources(unsigned eligible, Outcome& outcome);
    bool admitPrivateSource(const Source& source, Outcome& outcome);
    void refuse(unsigned lanes, Gate gate);
    void refusalDetail(unsigned lanes, GeneratedStaticLod::Detail detail,
        std::string_view model = {}, const GeneratedStaticLod::RefusalInfo* info = nullptr);
    struct Tiers
    {
        GeneratedStaticLod::Result lod;
        HlodBuildResult hlod;
    };
    Tiers buildTiers(osg::Node& originalSource, osg::Group& optimizedControl, std::string_view identity,
        unsigned eligible, osg::Camera* mainCamera, Outcome& outcome);
}
#endif
