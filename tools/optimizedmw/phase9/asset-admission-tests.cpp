#include <components/resource/speculativebudget.hpp>
#include <components/sceneutil/assetpaging.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <optional>
#include <vector>
#include <osg/Geometry>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <cstdlib>

namespace
{
    namespace R = Resource;
    namespace A = SceneUtil::AssetPaging;
    constexpr auto MiB = R::SpeculativeBudget::MiB;
    void require(bool value,const char* message)
    { if (!value) throw std::runtime_error(message); }
    R::OpenGlPressureSample healthy()
    {
        R::OpenGlPressureSample value;
        value.generation=4; value.sampledAtMs=20000; value.growthWatermarkValid=true;
        value.memory.physicalValid=true; value.memory.physicalTotal=32*1024*MiB;
        value.memory.physicalAvailable=8*1024*MiB;
        value.memory.commitValid=true; value.memory.commitAvailable=16*1024*MiB;
        value.memory.systemCommitValid=true; value.memory.systemCommitAvailable=16*1024*MiB;
        value.decision.state=R::OpenGlPressure::Normal; value.decision.admissionsPerSample=4;
        value.decision.limits={4*1024*MiB,2*1024*MiB,5*1024*MiB,1024*MiB,256*MiB,1536*MiB};
        return value;
    }
    template<class F> R::SpeculativeReservationReceipt refused(F&& operation)
    {
        try { operation(); }
        catch (const R::SpeculativeDeferred& error)
        {
            require(error.reservation.has_value(),"budget refusal must contain its exact reservation decision");
            return *error.reservation;
        }
        throw std::runtime_error("expected an optional reservation refusal");
    }
    R::OpenGlPressureSample sample(void* value)
    { return *static_cast<R::OpenGlPressureSample*>(value); }
    void requiredFreshEpochRetryTests()
    {
        using Epoch = std::pair<std::uint64_t, std::uint64_t>;
        std::atomic<bool> abort{false};
        std::optional<Epoch> epoch{Epoch{7, 11}};
        const auto readEpoch = [&] { return epoch; };
        unsigned attempts = 0, retries = 0;
        std::vector<unsigned> privateViews;
        const bool ready = SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
            SceneUtil::PagingWorkScope required(&abort, SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
            require(SceneUtil::PagingWorkScope::requiredReadiness(), "retry lost the demanded preparation phase");
            ++attempts;
            privateViews.clear();
            privateViews.push_back(0);
            if (attempts == 1)
            {
                ++epoch->second; // source recipe changed while loading listener rendered
                throw SceneUtil::PagingWorkCancelled{};
            }
            privateViews.push_back(1);
            privateViews.push_back(2);
        }, [&](const Epoch& before, const Epoch& after, unsigned retry) {
            require(before.first == after.first && before.second + 1 == after.second && retry == 1,
                "required retry lost the exact shader-epoch transition receipt");
            ++retries;
        });
        require(ready && attempts == 2 && retries == 1 && privateViews == std::vector<unsigned>{0, 1, 2}
            && !SceneUtil::PagingWorkScope::active(),
            "fresh required retry must rebuild the complete private view rather than publish its partial predecessor");
        attempts = retries = 0;
        const auto retried = [&](const Epoch&, const Epoch&, unsigned) { ++retries; };
        require(!SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
            ++attempts; throw SceneUtil::PagingWorkCancelled{};
        }, retried) && attempts == 1 && retries == 0,
            "unclassified cancellation without a source change cannot authorize a retry");
        attempts = retries = 0;
        require(!SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
            ++attempts; ++epoch->first; abort.store(true); throw SceneUtil::PagingWorkCancelled{};
        }, retried) && attempts == 1 && retries == 0,
            "actual abort must dominate a simultaneous source invalidation");
        abort.store(false); attempts = retries = 0;
        require(!SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
            ++attempts; ++epoch->second; throw SceneUtil::PagingWorkCancelled{};
        }, retried) && attempts == SceneUtil::RequiredPagingMaximumAttempts
            && retries + 1 == SceneUtil::RequiredPagingMaximumAttempts,
            "repeated source mutations must stop at the strict required retry bound");
        attempts = retries = 0;
        bool originalError = false;
        try
        {
            SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
                ++attempts; ++epoch->second; throw std::runtime_error("original loader failure");
            }, retried);
        }
        catch (const std::runtime_error& error)
        { originalError = std::string_view(error.what()) == "original loader failure"; }
        require(originalError && attempts == 1 && retries == 0,
            "an ordinary loader failure must retain its exact exception without retry or suppression");
        attempts = retries = 0;
        require(!SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] {
            ++attempts; epoch.reset(); throw SceneUtil::PagingWorkCancelled{};
        }, retried) && attempts == 1 && retries == 0,
            "world invalidation cannot reuse a cancelled private view in a different world");
        epoch = Epoch{7, 12}; abort.store(true); attempts = 0;
        require(!SceneUtil::prepareFreshRequiredPaging(abort, readEpoch, [&] { ++attempts; }, retried)
            && attempts == 0, "an already aborted job cannot start required construction");
        std::cout << "REQUIRED_FRESH_EPOCH_RETRY shader_changed=1 complete_private_rebuild=1"
            << " no_partial_publication=1 true_abort_preserved=1 unchanged_epoch_refused=1"
            << " maximum_attempts=3 original_exception_preserved=1 world_invalidated_refused=1\n";
    }
}

int main() try
{
    requiredFreshEpochRetryTests();
#ifdef _WIN32
    require(_putenv_s("OPENMW_P9_ASSET_MASK","1")==0,"isolated L1 fixture environment");
#else
    require(setenv("OPENMW_P9_ASSET_MASK","1",1)==0,"isolated L1 fixture environment");
#endif
    const auto original=healthy();
    {
        // Exact runtime refusal cohort. The old full128MiB allowance cannot
        // fit; bounded metadata + a census of this actual control population
        // can, without changing either the floor or the256MiB hard cap.
        auto captured=original;
        captured.decision.state=R::OpenGlPressure::Caution;
        captured.memory.physicalTotal=0x7d3c5d000ull;
        captured.memory.physicalAvailable=0x82e57000ull;
        captured.decision.limits.physicalCritical=0x7d3c5d00ull;
        captured.memory.commitAvailable=0x77d11000ull;
        captured.memory.systemCommitAvailable=0x77d11000ull;
        captured.decision.limits.commitCritical=256*MiB;
        R::SpeculativeBudget budget({256*MiB,4,1});
        const auto old=refused([&]{auto eager=budget.reserve(captured,128*MiB,R::SpeculativePriority::NearFuture);});
        require(old.reasons==R::SpeculativeBudget::PhysicalHeadroom && old.requested==128*MiB
            && old.physicalFloor==0x7d3c5d00ull,"replay the recorded physical-headroom refusal exactly");
        osg::ref_ptr<osg::Geometry> geometry=new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array(256);
        geometry->setVertexArray(vertices);
        osg::ref_ptr<osg::DrawElementsUInt> indices=new osg::DrawElementsUInt(GL_TRIANGLES);
        for(unsigned i=0;i<1536;++i) indices->push_back(i%256);
        geometry->addPrimitiveSet(indices);
        osg::ref_ptr<osg::Group> graph=new osg::Group;graph->addChild(geometry);
        const auto census=A::controlEstimate(*graph);
        require(census.valid && census.peakBytes>census.retainedBytes && census.peakBytes<MiB,
            "actual source streams and pinned optimizer scratch produce a bounded small work estimate");
        std::atomic<bool> abort{false};
        R::SpeculativeScope scope(&budget,&sample,&captured,R::SpeculativePriority::NearFuture);
        A::OptionalUpgradeScope optional(&abort);
        require(A::OptionalUpgradeScope::incrementalControl() && budget.stats().futureBytes==8*MiB,
            "L1 uses the explicit bounded proof metadata rather than128MiB speculative work");
        A::OptionalUpgradeScope::reserveReference();
        require(budget.stats().futureBytes==8*MiB+512,"reference tables are charged before insertion");
        A::Outcome outcome;
        auto control=A::reserveControlSources(A::Lod,outcome);
        require(control && A::admitControlSource(census,A::Lod,outcome,*control),
            "recorded low headroom admits the actual small control construction");
        const auto future=budget.stats().futureBytes;
        require(future==8*MiB+512+census.peakBytes,"complete metadata/control overlap remains additive");
        auto larger=census;larger.peakBytes=128*MiB;larger.retainedBytes=MiB;
        require(!A::admitControlSource(larger,A::Lod,outcome,*control) && budget.stats().futureBytes==future
            && (outcome.transient&A::Lod),"oversized construction is refused before allocation without releasing live work");
        A::retainHostOwner(*graph,census.retainedBytes);
        require(budget.stats().estimatedOwnerBytes==census.retainedBytes
            && budget.stats().futureBytes==future-census.retainedBytes,
            "publication transfers retained control ownership from its own actual work stage");
        require(geometry->getVertexArray()==vertices && geometry->getPrimitiveSet(0)==indices,
            "preflight reads existing source streams without mutating them");
        osg::ref_ptr<osg::DrawElementsUInt> strip=new osg::DrawElementsUInt(GL_TRIANGLE_STRIP);
        for(unsigned i=0;i<1536;++i) strip->push_back(i%3);
        geometry->setPrimitiveSet(0,strip);
        const auto expanded=A::controlEstimate(*graph);
        require(expanded.valid && expanded.peakBytes>=census.peakBytes+2*1536*80
            && expanded.retainedBytes>census.retainedBytes,
            "ordinary strip/fan expansion is included even with repeated indices and no larger vertex array");
        geometry->setPrimitiveSet(0,indices);
        osg::ref_ptr<SceneUtil::MorphGeometry> morph=new SceneUtil::MorphGeometry;
        morph->setSourceGeometry(geometry);
        osg::ref_ptr<SceneUtil::RigGeometry> rig=new SceneUtil::RigGeometry;
        rig->setSourceGeometry(geometry);
        const auto visibleSource=A::controlEstimate(*geometry);
        const auto hiddenMorph=A::controlEstimate(*morph),hiddenRig=A::controlEstimate(*rig);
        require(hiddenMorph.valid && hiddenRig.valid && visibleSource.valid
            && hiddenMorph.peakBytes>=visibleSource.peakBytes+4096
            && hiddenRig.peakBytes>=visibleSource.peakBytes+4096
            && hiddenMorph.retainedBytes>=visibleSource.retainedBytes+2048
            && hiddenRig.retainedBytes>=visibleSource.retainedBytes+2048,
            "ordinary CopyOp hidden rig/morph source arrays are included before allocation");
        osg::ref_ptr<SceneUtil::MorphGeometry> missing=new SceneUtil::MorphGeometry;
        require(!A::controlEstimate(*missing).valid,"missing hidden source cannot become an undercharged control");
        osg::ref_ptr<osg::Group> cyclic=new osg::Group;cyclic->addChild(cyclic);
        require(!A::controlEstimate(*cyclic).valid,"bounded source census refuses cycles without unbounded recursion");
        cyclic->removeChildren(0,1);
        std::cout<<"ASSET_CONTROL_PRESSURE_REPLAY fixed_cap=1 exact_captured_floor=1 old128_refused=1"
            <<" bounded_metadata=1 actual_control_progress=1 additive_overlap=1 before_allocation=1"
            <<" owner_transfer=1 source_immutable=1 expanded_primitives=1 hidden_deformation_source=1 cycle_refused=1\n";
    }
    {
        R::SpeculativeBudget budget({256*MiB,4,1});
        auto outer=budget.reserve(original,128*MiB,R::SpeculativePriority::NearFuture);
        auto competing=budget.reserve(original,112*MiB);
        const auto eager=refused([&] { auto stage=budget.reserve(original,32*MiB); });
        require(eager.reasons==R::SpeculativeBudget::AggregateTransientLimit
            && eager.requested==32*MiB && eager.currentFuture==240*MiB
            && eager.proposedFuture==272*MiB && eager.transientLimit==256*MiB
            && eager.growth==0 && eager.extra==272*MiB && eager.sample.generation==4
            && eager.sample.sampledAtMs==20000,"aggregate denial must report the exact locked decision, not RAM exhaustion");
        auto precise=budget.reserve(original,0);
        budget.extend(precise,original,MiB);
        require(precise.future()==MiB && budget.stats().futureBytes==241*MiB,
            "an exact small private population must progress under the same cap where eager32MiB was denied");
        const auto over=refused([&] { budget.extend(precise,original,16*MiB); });
        require((over.reasons&R::SpeculativeBudget::AggregateTransientLimit)
            && precise.future()==MiB && budget.stats().futureBytes==241*MiB,
            "failed incremental admission must leave existing future ownership unchanged");
        precise.releaseUnallocated(MiB);
        require(budget.stats().futureBytes==240*MiB,"only unallocated private future may be returned");
    }
    {
        R::SpeculativeBudget budget({256*MiB,4,1});
        auto owner=budget.reserve(original,256*MiB);
        auto zero=budget.reserve(original,0);
        const auto full=refused([&] { budget.extend(zero,original,1); });
        require(full.currentFuture==256*MiB && full.requested==1
            && (full.reasons&R::SpeculativeBudget::AggregateTransientLimit),"the exact cap cannot admit one additional byte");
        owner.releaseUnallocated(256*MiB);
        budget.extend(zero,original,MiB);
        require(zero.future()==MiB && budget.stats().futureBytes==MiB,
            "released competing reservation must allow a subsequent exact private admission");
    }
    {
        R::SpeculativeBudget budget({256*MiB,4,1});
        auto low=original;
        low.memory.physicalAvailable=low.decision.limits.physicalReserve+MiB;
        low.memory.commitAvailable=low.decision.limits.commitReserve+MiB;
        low.memory.systemCommitAvailable=low.decision.limits.commitReserve+MiB;
        const auto denied=refused([&] { auto stage=budget.reserve(low,2*MiB); });
        require(denied.reasons==(R::SpeculativeBudget::PhysicalHeadroom
            |R::SpeculativeBudget::ProcessCommitHeadroom|R::SpeculativeBudget::SystemCommitHeadroom),
            "all independent valid headroom predicates must remain visible in one coherent receipt");
        auto invalid=original; invalid.generation=0; invalid.growthWatermarkValid=false;
        const auto unknown=refused([&] { auto stage=budget.reserve(invalid,MiB); });
        require((unknown.reasons&R::SpeculativeBudget::PressurePolicy)
            && (unknown.reasons&R::SpeculativeBudget::MissingGrowthWatermark)
            && (unknown.pressureReasons&R::SpeculativeBudget::MissingGeneration),
            "unknown sampler state must refuse optional ownership without inventing memory pressure");
        auto critical=original; critical.decision.state=R::OpenGlPressure::Critical;
        const auto nearCritical=refused([&] { auto stage=budget.reserve(critical,MiB,R::SpeculativePriority::NearFuture); });
        require(nearCritical.pressureReasons&R::SpeculativeBudget::NearFutureState,"critical pressure still blocks the NearFuture lane");
        auto caution=original; caution.decision.state=R::OpenGlPressure::Caution;
        caution.memory.physicalAvailable=3*1024*MiB;
        auto near=budget.reserve(caution,MiB,R::SpeculativePriority::NearFuture);
        const auto background=refused([&] { auto stage=budget.reserve(caution,MiB); });
        require(background.pressureReasons&R::SpeculativeBudget::BackgroundState,
            "existing caution exception belongs only to bounded NearFuture ownership");
    }
    {
        R::SpeculativeBudget budget({256*MiB,4,1});
        auto observed=original;
        auto stage=budget.reserve(observed,2*MiB);
        int identity=0;
        auto charge=budget.track({&identity,2},MiB,false,&stage);
        require(stage.future()==MiB && budget.stats().futureBytes==MiB
            && budget.stats().estimatedOwnerBytes==MiB && budget.stats().observedGrowth==MiB,
            "root publication transfers actual private ownership instead of double charging future bytes");
        charge.reset();
        require(budget.stats().estimatedOwnerBytes==0 && budget.stats().observedGrowth==MiB,
            "tracked owner release must not fabricate immediate OS-free credit");
        auto tooTight=observed; tooTight.memory.physicalAvailable=tooTight.decision.limits.physicalReserve+2*MiB;
        const auto beforeSample=refused([&] { auto next=budget.reserve(tooTight,MiB); });
        require(beforeSample.unobservedGrowth==MiB && (beforeSample.reasons&R::SpeculativeBudget::PhysicalHeadroom),
            "growth after the sample watermark remains charged despite owner destruction");
        tooTight.accountedGrowth=budget.watermark()->load(); ++tooTight.generation;
        auto afterSample=budget.reserve(tooTight,MiB);
        require(budget.stats().futureBytes==2*MiB,"fresh OS sample may account for earlier growth without changing caps");
        auto regressed=original; regressed.accountedGrowth=2*MiB;
        const auto bad=refused([&] { auto next=budget.reserve(regressed,1); });
        require(bad.reasons&R::SpeculativeBudget::RegressedGrowthWatermark,"a future growth watermark must fail closed");
    }
    {
        R::SpeculativeBudget budget({UINT64_MAX,4,0});
        auto huge=original; huge.memory.physicalTotal=UINT64_MAX; huge.memory.physicalAvailable=UINT64_MAX;
        huge.memory.commitAvailable=UINT64_MAX; huge.memory.systemCommitAvailable=UINT64_MAX;
        huge.decision.limits={};
        auto stage=budget.reserve(huge,UINT64_MAX-8);
        const auto overflow=refused([&] { auto next=budget.reserve(huge,16); });
        require(overflow.reasons&R::SpeculativeBudget::ArithmeticOverflow,
            "saturated addition must not create an undercharged live reservation");
        require(budget.stats().futureBytes==UINT64_MAX-8,"overflow leaves earlier ownership intact");
        const auto max=refused([&] { auto next=budget.reserve(huge,UINT64_MAX); });
        require(max.reasons&R::SpeculativeBudget::InvalidRequestedBytes,"unbounded requested allocation must be refused");
    }
    {
        auto value=original;
        R::SpeculativeBudget budget({256*MiB,4,1});
        const auto untouched=budget.stats().futureBytes;
        { R::SpeculativeScope::Stage demand(128*MiB,0); }
        require(budget.stats().futureBytes==untouched,"demand outside the optional worker scope never reserves or waits");
        R::SpeculativeScope scope(&budget,&sample,&value,R::SpeculativePriority::NearFuture);
        R::SpeculativeScope::Stage privateCopies(0,0);
        privateCopies.reserveAdditional(MiB,32*MiB);
        require(privateCopies.reserved()==MiB && budget.stats().futureBytes==MiB,"Scope Stage reserves each clone before allocation");
        bool denied=false;
        try { privateCopies.reserveAdditional(32*MiB,32*MiB); }
        catch (const R::SpeculativeDeferred&) { denied=true; }
        require(denied && privateCopies.reserved()==MiB,"per-population private32MiB bound remains independent of global capacity");
        privateCopies.releaseUnallocated(MiB);
        require(privateCopies.reserved()==0 && budget.stats().futureBytes==0,"failed GPU admission can return only its unallocated clone future");
    }
    {
        R::SpeculativeReservationReceipt permit;
        permit.jobPermit=true; permit.reasons=R::SpeculativeBudget::PressurePolicy;
        require(A::admissionWaiting(R::SpeculativeDeferred(permit)),
            "denied Background permit must wait before any finite construction attempt");
        permit.jobPermit=false; permit.reasons=R::SpeculativeBudget::AggregateTransientLimit;
        permit.requested=MiB; permit.transientLimit=2*MiB;
        require(A::admissionWaiting(R::SpeculativeDeferred(permit)),
            "temporary common future occupancy is an admission wait before allocation");
        permit.requested=3*MiB;
        require(!A::admissionWaiting(R::SpeculativeDeferred(permit)),
            "an individually oversized construction preserves finite failure semantics");
        permit.reasons=R::SpeculativeBudget::ArithmeticOverflow;
        require(!A::admissionWaiting(R::SpeculativeDeferred(permit))
            && !A::admissionWaiting(R::SpeculativeDeferred{}),
            "accounting errors and unclassified construction failures cannot wait indefinitely");
    }
    {
        int world=0,otherWorld=0;
        A::OptionalTailRetry retry; retry.generation={&world,7,12,A::Lod};
        const auto generation=retry.generation;
        retry.finishAttempt(true,100);
        require(retry.attempts==1 && retry.pending && !retry.due(generation,5099)
            && retry.due(generation,5100),"same complete weak cohort becomes eligible after exact5sec cooldown");
        auto changed=generation; ++changed.vfs;
        require(!retry.due(changed,10000),"changed VFS source cannot spend an earlier cohort retry");
        changed=generation; ++changed.shader;
        require(!retry.due(changed,10000),"changed approved shader generation cannot spend an earlier cohort retry");
        changed=generation; changed.world=&otherWorld;
        require(!retry.due(changed,10000),"changed world cannot spend an earlier cohort retry");
        changed=generation; changed.mask=A::Hlod;
        require(!retry.due(changed,10000),"changed optional mechanism cohort starts independently");
        retry.finishAttempt(true,5200);
        require(retry.attempts==2 && retry.due(generation,10200),"second bounded worker failure permits the final deferred attempt");
        retry.finishAttempt(true,10300);
        require(retry.attempts==3 && retry.exhausted && !retry.pending && !retry.due(generation,999999),
            "same cohort exhausts at exactly3 real tail attempts rather than cache-hit observations");
        A::OptionalTailRetry fresh; fresh.generation=changed;
        require(fresh.attempts==0 && !fresh.exhausted,"a new exact source/world/cohort gets a fresh finite ledger");
        fresh.finishAttempt(true,1); fresh.cancel();
        require(!fresh.due(changed,999999),"inflight cancellation cannot reschedule obsolete optional work");
        A::OptionalTailRetry terminal; terminal.generation=generation; terminal.finishAttempt(false,1);
        require(!terminal.pending && !terminal.due(generation,999999),"successful or terminal fallback does not spawn retries");
        A::OptionalTailRetry waiting; waiting.generation=generation;
        A::Quality quality;
        for (unsigned i=0;i<10;++i)
        {
            const auto now=100+std::uint64_t(i)*10000;
            waiting.finishAttempt(true,now,false);
            A::Outcome outcome; outcome.defer(A::Lod,true); outcome.finish(A::Lod);
            quality=quality.after(outcome,now).withoutOwner();
            require(!waiting.attempts && !waiting.exhausted && waiting.pending
                && !waiting.due(generation,now+A::RetryDelayMilliseconds-1)
                && waiting.due(generation,now+A::RetryDelayMilliseconds),
                "unadmitted tail remains bounded by cooldown without spending construction attempts");
            require(!quality.attempts[0] && !quality.exhausted && (quality.admissionWait&A::Lod)
                && quality.satisfies(A::Lod,now+1) && !quality.satisfies(A::Lod,now+A::RetryDelayMilliseconds),
                "evicted waiting chunk keeps renewed eligibility without inventing a completed owner");
        }
        A::Outcome started; started.constructionStarted=A::Lod; started.defer(A::Lod,true); started.finish(A::Lod);
        quality=quality.after(started,200000);
        require(quality.attempts[0]==1 && !quality.admissionWait,
            "a later failure after actual control allocation still spends the finite chunk ledger");
        waiting.finishAttempt(false,200000,true);
        require(waiting.attempts==1 && !waiting.pending && !waiting.exhausted,
            "renewed admission can finish the same waiting cohort normally");
        A::OptionalTailRetry overflow; overflow.generation=generation; overflow.finishAttempt(true,UINT64_MAX-10);
        require(overflow.retryAfter==UINT64_MAX && !overflow.due(generation,UINT64_MAX-1),"cooldown cannot wrap to a busy loop");
    }
    std::cout << "ASSET_ADMISSION_PASS fixed_cap=1 exact_private_progress=1 aggregate_denial=1 coherent_receipt=1"
        << " headroom=1 unknown_pressure=1 owner_release=1 growth_conserved=1 overflow=1 demand_unblocked=1"
        << " retry_cooldown=1 finite_tail=1 generation_reset=1 cancellation=1 terminal_fallback=1"
        << " preallocation_wait=1 eviction_wait_conserved=1 postallocation_failure_bounded=1 same_cohort_resume=1\n";
    return 0;
}
catch (const std::exception& error)
{ std::cerr << "ASSET_ADMISSION_FAIL " << error.what() << '\n'; return 1; }
