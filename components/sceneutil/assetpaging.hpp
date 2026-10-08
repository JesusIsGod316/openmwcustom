#ifndef OPENMW_SCENEUTIL_ASSETPAGING_HPP
#define OPENMW_SCENEUTIL_ASSETPAGING_HPP

#include "generatedstaticlod.hpp"
#include "selectivehlod.hpp"
#include "pagingwork.hpp"
#include <components/resource/speculativebudget.hpp>
#include <components/resource/assetpolicy.hpp>
#include <components/vfs/pathutil.hpp>
#include <array>
#include <algorithm>
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
    enum class CpuStage : unsigned { Outer, Snapshot, Private, Count };
    struct LaneCounters
    {
        std::atomic_uint64_t sourceRequested{0}, sourceCertified{0}, sourceRejected{0}, snapshotDenied{0};
        std::atomic_uint64_t outerCpuDenied{0}, privateCpuDenied{0}, privateGpuDenied{0}, candidateAdmitted{0}, materialConflictInstances{0};
        std::atomic_uint64_t buildAttempts{0}, buildRefused{0}, ready{0};
        std::atomic_uint64_t controlSourcesAdmitted{0}, controlReservationBytes{0}, controlRetainedBytes{0}, controlCensusRefused{0};
        std::atomic_uint64_t refusals[static_cast<unsigned>(Gate::Count)]{};
        std::atomic_uint64_t details[static_cast<unsigned>(GeneratedStaticLod::Detail::Count)]{};
        // Refusal observations by stage and independent predicate; a single
        // attempt can contribute multiple reasons. These are not a population
        // funnel or a transactional snapshot of other producer counters.
        std::atomic_uint64_t cpuReasons[static_cast<unsigned>(CpuStage::Count)][Resource::SpeculativeBudget::ReservationRefusalCount]{};
        std::atomic_uint64_t cpuUnclassified[static_cast<unsigned>(CpuStage::Count)]{};
    };
    void cpuRefusal(unsigned mask, CpuStage stage, const Resource::SpeculativeDeferred& error);
    void noteOptionalTransient(unsigned mask) noexcept;
    void noteOptionalAttempt(unsigned mask) noexcept;
    void noteOptionalAdmissionWait(unsigned mask) noexcept;
    // A current allocation/job gate can reopen. An oversized single request or
    // an unclassified construction failure still spends the finite cook ledger.
    bool admissionWaiting(const Resource::SpeculativeDeferred& error) noexcept;
    struct Counters
    {
        inline static LaneCounters lod, hlod;
        // OR of observed per-chunk outcomes; these are lane bitfields, not counts
        // of unique chunks. Workers may still progress at the stopped-draw receipt.
        inline static std::atomic_uint32_t attemptedMask{0}, completedMask{0}, terminalMask{0}, transientMask{0};
        inline static std::atomic_uint64_t retryAttempts{0}, retryDeferred{0}, retryExhausted{0};
        inline static std::atomic_uint64_t tailAttempts{0}, tailQueued{0}, tailDeferred{0}, tailExhausted{0}, tailCancelled{0};
    };
    inline LaneCounters& lane(unsigned bit) { return bit == Lod ? Counters::lod : Counters::hlod; }
    template<class F> void each(unsigned mask, F&& function)
    { for (unsigned bit : {Lod, Hlod}) if (mask & bit) function(lane(bit)); }
    class HostOwner final : public osg::Object
    {
    public:
        HostOwner() = default;
        explicit HostOwner(Resource::SpeculativeBudget::ChargePtr charge,Resource::SpeculativeBudget::SourcePinPtr pin={})
            : mCharge(std::move(charge)),mPin(std::move(pin)) {}
        HostOwner(const HostOwner& copy, const osg::CopyOp& op) : osg::Object(copy,op), mCharge(copy.mCharge),mPin(copy.mPin) {}
        META_Object(SceneUtil, HostOwner)
    private:
        Resource::SpeculativeBudget::ChargePtr mCharge;
        Resource::SpeculativeBudget::SourcePinPtr mPin;
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
    inline bool retainExperimentalOwner(osg::Node& derived,std::uint64_t incrementalBytes,const osg::Node* borrowedSource,
        Resource::SpeculativeScope::Stage* admission=nullptr,unsigned kind=2u,std::uint64_t borrowedEstimate=0)
    {
        const auto context=Resource::SpeculativeScope::capture();
        if(!context.budget || !context.budget->experimentEnabled()) return false;
        if(!admission && !context.experimental) throw Resource::SpeculativeDeferred{};
        auto pin=borrowedSource ? context.budget->pinSource({borrowedSource,2u},
            borrowedEstimate ? borrowedEstimate : GeneratedStaticLod::sourceBytes(*borrowedSource))
            : Resource::SpeculativeBudget::SourcePinPtr{};
        auto charge=admission ? admission->track({&derived,kind},incrementalBytes,false)
            : Resource::SpeculativeScope::track(context.budget,{&derived,kind},incrementalBytes,false);
        if(!charge) throw Resource::SpeculativeDeferred{};
        derived.getOrCreateUserDataContainer()->addUserObject(new HostOwner(std::move(charge),std::move(pin)));
        return true;
    }
    void retainPagingOwners(osg::Node& root,std::uint64_t controlBytes,std::uint64_t privateBytes,
        std::uint64_t derivedBytes,Resource::SpeculativeScope::Stage* privateAdmission);
    struct Outcome
    {
        unsigned allowed = Mask;
        unsigned attempted = 0, completed = 0, terminal = 0, transient = 0;
        unsigned admissionWait = 0;
        unsigned constructionStarted = 0;
        std::shared_ptr<GeneratedStaticLod::Readiness> readiness;
        void defer(unsigned mask, bool waiting = false)
        {
            transient |= mask & Mask;
            if (waiting) admissionWait |= mask & Mask & ~constructionStarted;
        }
        void finish(unsigned requested)
        {
            attempted |= requested & Mask;
            admissionWait &= transient & ~completed;
            terminal = attempted & ~(completed | transient);
            Counters::attemptedMask.fetch_or(attempted); Counters::completedMask.fetch_or(completed);
            Counters::terminalMask.fetch_or(terminal); Counters::transientMask.fetch_or(transient);
            // Submission is not GL readiness. Revisit pending ICO ownership
            // once cooldown expires; a ready cached result then terminates the
            // tail without recooking, while expired readiness keeps its finite
            // per-chunk ledger. This never delays required weak publication.
            const unsigned pending = readiness && readiness->ready.load(std::memory_order_acquire) ? 0 : completed;
            noteOptionalTransient(transient | pending);
            noteOptionalAttempt(attempted & ~admissionWait);
            noteOptionalAdmissionWait(admissionWait);
        }
    };
    struct Quality
    {
        unsigned attempted = 0, completed = 0, terminal = 0, transient = 0, exhausted = 0;
        unsigned admissionWait = 0;
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

    // A serialized optional terrain-tail owner revisits the same complete weak
    // view. Cooldown and finite attempts belong to this exact source cohort;
    // they never make weak readiness wait and never count cache observations as
    // retries. Main-thread scheduling copies the ledger only after worker Done.
    struct OptionalTailRetry
    {
        struct Generation
        {
            const void* world = nullptr;
            std::uint64_t vfs = 0, shader = 0;
            unsigned mask = 0;
            bool operator==(const Generation&) const = default;
        };
        Generation generation;
        unsigned attempts = 0;
        bool pending = false, exhausted = false;
        std::uint64_t retryAfter = 0;
        bool due(const Generation& current, std::uint64_t now) const noexcept
        { return generation == current && pending && !exhausted && now >= retryAfter; }
        void finishAttempt(bool deferred,std::uint64_t now, bool spentAttempt = true) noexcept
        {
            if (spentAttempt) attempts = std::min(MaximumAttempts,attempts+1);
            exhausted = deferred && attempts >= MaximumAttempts;
            pending = deferred && !exhausted;
            retryAfter = pending && now <= UINT64_MAX-RetryDelayMilliseconds
                ? now+RetryDelayMilliseconds : UINT64_MAX;
        }
        void cancel() noexcept { pending = false; }
    };

    // Used by both actual terrain-preloader owners and the chain fixture. The
    // caller establishes the existing NearFuture SpeculativeScope first.
    class OptionalUpgradeScope
    {
    public:
        explicit OptionalUpgradeScope(std::atomic<bool>* abort)
            : mOverlap(overlap())
            , mWork(abort, PagingWorkScope::Phase::OptionalOptimization)
            , mPrevious(sCurrent)
        { sCurrent = this; }
        ~OptionalUpgradeScope() { sCurrent = mPrevious; }
        bool needsRetry() const noexcept { return mNeedsRetry; }
        bool spentAttempt() const noexcept { return mSpentAttempt; }
        bool waitingForAdmission() const noexcept { return mWaitingForAdmission; }
        static void noteTransient(unsigned mask) noexcept
        { if (sCurrent && (mask & Mask)) sCurrent->mNeedsRetry = true; }
        static void noteAttempt(unsigned mask) noexcept
        { if (sCurrent && (mask & Mask)) sCurrent->mSpentAttempt = true; }
        static void noteAdmissionWait(unsigned mask) noexcept
        { if (sCurrent && (mask & Mask)) sCurrent->mWaitingForAdmission = true; }
        // The L1-only lane keeps bounded proof metadata, then admits actual
        // reference/control/cook work before allocation. Other lanes preserve
        // their existing overlap policy and independently switchable control.
        static bool incrementalControl() noexcept
        { return sCurrent && sCurrent->mIncremental; }
        static void reserveReference();
    private:
        static std::unique_ptr<Resource::SpeculativeScope::Stage> overlap()
        {
            // 4 MiB effective-state proof + bounded source identity and owner
            // tables/support. This is a conservative metadata ceiling, not a
            // full clone/optimizer allowance or an OS-free-memory claim.
            const auto bytes = (Resource::AssetPolicy::mask() & Mask) == Lod
                ? 8 * Resource::SpeculativeBudget::MiB : 128 * Resource::SpeculativeBudget::MiB;
            try { return std::make_unique<Resource::SpeculativeScope::Stage>(bytes,0); }
            catch (const Resource::SpeculativeDeferred& error)
            {
                cpuRefusal(Resource::AssetPolicy::mask() & Mask,CpuStage::Outer,error);
                each(Resource::AssetPolicy::mask() & Mask,[](LaneCounters& value) {
                    ++value.outerCpuDenied; ++value.refusals[static_cast<unsigned>(Gate::Capacity)];
                });
                throw;
            }
        }
        std::unique_ptr<Resource::SpeculativeScope::Stage> mOverlap;
        PagingWorkScope mWork;
        inline static thread_local OptionalUpgradeScope* sCurrent = nullptr;
        OptionalUpgradeScope* mPrevious = nullptr;
        bool mNeedsRetry = false;
        bool mSpentAttempt = false;
        bool mWaitingForAdmission = false;
        bool mIncremental = (Resource::AssetPolicy::mask() & Mask) == Lod;
    };
    struct ControlEstimate
    {
        std::uint64_t peakBytes = 0, retainedBytes = 0;
        bool valid = false;
    };
    // Worker-only bounded source census; includes conservative clone/flatten/
    // merge and post-transform scratch. Borrowed source/control ownership is
    // already live and remains additive to these new allocations.
    ControlEstimate controlEstimate(const osg::Node& original);
    std::unique_ptr<Resource::SpeculativeScope::Stage> reserveControlSources(unsigned lanes, Outcome& outcome);
    bool admitControlSource(const ControlEstimate& estimate, unsigned lanes, Outcome& outcome,
        Resource::SpeculativeScope::Stage& admission);
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
    bool admitPrivateSource(const Source& source, Outcome& outcome,
        Resource::SpeculativeScope::Stage& hostAdmission, std::size_t copyBytes);
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
