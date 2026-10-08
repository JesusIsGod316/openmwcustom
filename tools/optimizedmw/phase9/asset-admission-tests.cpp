#include <components/resource/speculativebudget.hpp>
#include <components/sceneutil/assetpaging.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>

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
}

int main() try
{
    const auto original=healthy();
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
        A::OptionalTailRetry overflow; overflow.generation=generation; overflow.finishAttempt(true,UINT64_MAX-10);
        require(overflow.retryAfter==UINT64_MAX && !overflow.due(generation,UINT64_MAX-1),"cooldown cannot wrap to a busy loop");
    }
    std::cout << "ASSET_ADMISSION_PASS fixed_cap=1 exact_private_progress=1 aggregate_denial=1 coherent_receipt=1"
        << " headroom=1 unknown_pressure=1 owner_release=1 growth_conserved=1 overflow=1 demand_unblocked=1"
        << " retry_cooldown=1 finite_tail=1 generation_reset=1 cancellation=1 terminal_fallback=1\n";
    return 0;
}
catch (const std::exception& error)
{ std::cerr << "ASSET_ADMISSION_FAIL " << error.what() << '\n'; return 1; }
