#include <components/resource/speculativebudget.hpp>

#include <atomic>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    using B=Resource::SpeculativeBudget;
    using P=Resource::SpeculativePriority;
    constexpr auto M=B::MiB;
    void require(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
    B::Config config()
    {
        B::Config result;result.transientLimit=256*M;result.experimentLimit=256*M;
        result.experimentScratchLimit=128*M;result.experimentMaximumJobs=1;return result;
    }
    Resource::OpenGlPressureSample healthy(B& budget)
    {
        Resource::OpenGlPressureSample s;
        s.generation=1;s.sampledAtMs=1000;s.growthWatermarkValid=true;s.accountedGrowth=*budget.watermark();
        s.memory.physicalValid=true;s.memory.physicalTotal=32ull*1024*M;s.memory.physicalAvailable=16ull*1024*M;
        s.memory.commitValid=true;s.memory.commitAvailable=16ull*1024*M;
        s.memory.systemCommitValid=true;s.memory.systemCommitAvailable=16ull*1024*M;
        s.memory.lowMemoryValid=true;s.memory.lowMemory=false;
        s.decision.state=Resource::OpenGlPressure::Normal;s.decision.admissionsPerSample=4;
        s.decision.limits=Resource::OpenGlPressurePolicy{}.limits(s.memory.physicalTotal);
        return s;
    }
    Resource::OpenGlPressureSample sample(void* owner) {return healthy(*static_cast<B*>(owner));}
    struct ControlledSample
    {
        B& budget;
        Resource::OpenGlPressureSample value;
        static Resource::OpenGlPressureSample read(void* owner)
        {
            auto& control = *static_cast<ControlledSample*>(owner);
            auto result = control.value;
            result.accountedGrowth = *control.budget.watermark();
            return result;
        }
    };
    template<class Function>
    std::uint32_t refusal(Function&& function)
    {
        try {function();}
        catch(const Resource::SpeculativeDeferred& e) {return e.reservation ? e.reservation->reasons : 0;}
        throw std::runtime_error("operation incorrectly admitted");
    }
    template<class Function>
    Resource::SpeculativeReservationReceipt allocationRefusal(Function&& function)
    {
        try { function(); }
        catch (const Resource::SpeculativeDeferred& error)
        {
            require(error.reservation.has_value() && !error.reservation->jobPermit,
                "allocation refusal lost its exact byte decision");
            return *error.reservation;
        }
        throw std::runtime_error("allocation incorrectly admitted");
    }
    template<class Function>
    Resource::SpeculativeReservationReceipt permitRefusal(Function&& function)
    {
        try { function(); }
        catch (const Resource::SpeculativeDeferred& error)
        {
            require(error.reservation.has_value(), "cook permit lost its exact refusal receipt");
            require(error.reservation->jobPermit, "cook permit refusal was mislabeled as allocated work");
            return *error.reservation;
        }
        throw std::runtime_error("cook permit incorrectly admitted");
    }
    B::ChargePtr retain(B& budget,const void* key,std::uint64_t bytes)
    {
        auto stage=budget.reserve(healthy(budget),bytes,P::Background,true);
        return budget.track({key,7},bytes,true,&stage,true);
    }
    void intersections()
    {
        B budget(config());int first=0,second=0;
        auto a=retain(budget,&first,100*M);auto b=retain(budget,&second,100*M);
        require(budget.stats().experimentRetainedBytes==200*M,"retained output did not remain in the child");
        const auto before=budget.stats();
        require(refusal([&]{auto s=budget.reserve(healthy(budget),57*M,P::Background,true);})&B::ExperimentTotalLimit,
            "retained plus construction exceeded total child quota");
        require(budget.stats().futureBytes==before.futureBytes,"child refusal changed parent reservation");
        {auto stage=budget.reserve(healthy(budget),56*M,P::Background,true);
         require(budget.stats().experimentRetainedBytes+budget.stats().experimentFutureBytes==256*M,
             "scratch was treated as additional memory beyond total quota");}
        a.reset();b.reset();
        {auto stage=budget.reserve(healthy(budget),128*M,P::Background,true);
         require(refusal([&]{budget.extend(stage,healthy(budget),1);})&B::ExperimentScratchLimit,
             "scratch sublimit was not enforced");}
        {auto foundation=budget.reserve(healthy(budget),200*M);
         require(refusal([&]{auto s=budget.reserve(healthy(budget),64*M,P::Background,true);})&B::AggregateTransientLimit,
             "child created an independent parent spending allowance");
         require(budget.stats().experimentFutureBytes==0,"parent refusal leaked child future");}
        require(budget.stats().experimentPeakTotalBytes<=256*M && budget.stats().experimentPeakFutureBytes<=128*M,
            "peak exceeded configured intersection");
    }
    void canonicalAndPending()
    {
        B budget(config());int key=0;
        auto stage=budget.reserve(healthy(budget),100*M,P::Background,true);
        auto owner=budget.track({&key,7},100*M,true,&stage,true);
        auto shared=budget.track({&key,7},100*M,true,nullptr,true);
        require(owner==shared && budget.stats().experimentRetainedBytes==100*M,
            "canonical shared owner was charged more than once");
        require(budget.stats().knownOwnerBytes==100*M && budget.stats().futureBytes==0,
            "output transfer duplicated common ownership");
        owner->pending();owner->pending();
        require(budget.stats().experimentPendingBytes==100*M,"pending debt is not idempotent");
        const auto growth=budget.watermark()->load(std::memory_order_acquire);owner.reset();
        require(budget.stats().experimentRetainedBytes==100*M,"detaching one holder freed shared ownership");
        shared.reset();
        require(budget.stats().experimentRetainedBytes==0 && budget.stats().experimentPendingBytes==0,
            "final owner release leaked child debt");
        require(*budget.watermark()==growth,"owner release invented immediate OS free credit");
        int invalid=0;
        require(refusal([&]{budget.track({&invalid,7},M,true,nullptr,true);})&B::ExperimentReservationMismatch,
            "experimental output escaped without construction reservation");
        auto wrong=budget.reserve(healthy(budget),M);
        require(refusal([&]{budget.track({&invalid,7},M,true,&wrong,true);})&B::ExperimentReservationMismatch,
            "foundation reservation was adopted as experimental ownership");
    }
    void unknownAndPins()
    {
        B budget(config());int key=0,foundationKey=0,source=0;
        auto stage=budget.reserve(healthy(budget),16*M,P::Background,true);
        require(refusal([&]{budget.track({&key,7},0,false,&stage,true);})&B::ExperimentReservationMismatch,
            "zero-byte experimental owner escaped its construction reservation");
        require(budget.stats().unknownOwners==0 && budget.stats().experimentRetainedBytes==0
            && budget.stats().experimentFutureBytes==16*M,"rejected unknown owner changed budget totals");
        auto estimated=budget.track({&key,7},8*M,false,&stage,true);
        require(budget.stats().experimentRetainedBytes==8*M && budget.stats().experimentFutureBytes==8*M,
            "estimated output escaped transfer accounting");
        auto ordinary=budget.track({&foundationKey,7},0,false);
        require(!budget.promoteUnknown(ordinary,M,stage),"child promotion reclassified an existing foundation owner");
        const auto growth=budget.watermark()->load(std::memory_order_acquire);
        auto p=budget.pinSource({&source,9},64*M);auto same=budget.pinSource({&source,9},128*M);
        require(p==same && budget.stats().experimentSourcePins==1 && budget.stats().experimentSourcePinEstimateBytes==64*M,
            "source receipt was neither canonical nor bounded to its original estimate");
        require(budget.stats().experimentRetainedBytes==8*M && *budget.watermark()==growth,
            "borrowed source was recharged as a new allocation");
        p.reset();require(budget.stats().experimentSourcePins==1,"one pin holder release lost another holder");
        same.reset();require(budget.stats().experimentSourcePins==0,"pin lifetime escaped its actual holders");
    }
    void scopesAndPressure()
    {
        B budget(config());
        Resource::SpeculativeScope foundation(&budget,&sample,&budget,P::NearFuture);
        Resource::SpeculativeScope::Stage ordinary(8*M,0);
        {
            Resource::ExperimentalSpeculativeScope experiment;
            Resource::ExperimentalSpeculativeScope nested;
            require(budget.stats().experimentJobs==1,"nested cooking consumed a second permit");
            Resource::SpeculativeScope::Stage cook(24*M,0);int key=0;
            auto owner=Resource::SpeculativeScope::track(&budget,{&key,7},8*M,true);
            require(budget.stats().futureBytes==24*M && budget.stats().experimentFutureBytes==16*M,
                "nested child transfer disturbed the foundation stage");
            require(Resource::SpeculativeScope::capture().experimental,"child scope lost its domain");
        }
        require(!Resource::SpeculativeScope::capture().experimental && budget.stats().experimentJobs==0,
            "construction did not restore legacy caller context");
        require(budget.stats().futureBytes==8*M && budget.stats().experimentFutureBytes==0,
            "scope destruction leaked child reservation or altered parent");
        auto permit=budget.tryExperimentJob(healthy(budget));
        require(permitRefusal([&]{auto second=budget.tryExperimentJob(healthy(budget));}).reasons==B::ExperimentJobLimit,
            "occupied cook permit was confused with byte or pressure refusal");
        auto caution=healthy(budget);caution.decision.state=Resource::OpenGlPressure::Caution;
        permit.finish();
        require(permitRefusal([&]{auto denied=budget.tryExperimentJob(caution);}).reasons&B::PressurePolicy,
            "cook permit pressure denial did not preserve policy refusal");
        require(budget.stats().experimentJobs==0,"pressure refusal leaked a cooking permit");
        auto degraded=healthy(budget);degraded.decision.state=Resource::OpenGlPressure::Degraded;
        require(refusal([&]{auto denied=budget.reserve(degraded,M,P::Background,true);})&B::PressurePolicy,
            "invalid/degraded pressure granted child capacity");
    }
    void permitRecoveryAndRecheck()
    {
        B budget(config());
        Resource::OpenGlPressurePolicy policy;
        auto current = healthy(budget);
        current.memory.physicalAvailable = 3ull * 1024 * M;
        current.decision = policy.update(current.memory, 1000);
        require(current.decision.state == Resource::OpenGlPressure::Caution,
            "fixture did not enter actual caution pressure");
        const auto before = budget.stats();
        const auto waiting = budget.experimentJobReceipt(current);
        require(waiting.jobPermit && waiting.experimental && waiting.requested == 0
            && waiting.priority == P::Background && waiting.experimentJobs == 0 && waiting.experimentJobLimit == 1,
            "advisory permit lost its domain, priority or exact job bound");
        require((waiting.reasons & (B::PressurePolicy | B::PhysicalHeadroom))
                == (B::PressurePolicy | B::PhysicalHeadroom)
            && (waiting.pressureReasons & (B::BackgroundState | B::BackgroundAdmissionsZero))
                == (B::BackgroundState | B::BackgroundAdmissionsZero),
            "caution refusal did not retain independent pressure and headroom predicates");
        require(waiting.physicalFloor == current.decision.limits.physicalReserve
            && waiting.commitFloor == current.decision.limits.commitReserve,
            "optional cook borrowed the near-future caution floors");
        require(budget.stats().experimentJobs == before.experimentJobs
            && budget.stats().experimentDenied == before.experimentDenied
            && budget.stats().futureBytes == before.futureBytes,
            "advisory permit observation spent an attempt or allocation");
        // The foundation's protected NearFuture lane remains independently
        // usable; optional cooking cannot change that caller's admission.
        { auto protectedStage = budget.reserve(current, M, P::NearFuture); }
        const auto deferred = permitRefusal([&]{ auto job = budget.tryExperimentJob(current); });
        require(deferred.sample.generation == current.generation
            && deferred.sample.memory.physicalAvailable == current.memory.physicalAvailable,
            "cook permit refusal lost the deciding sample");
        current.memory.physicalAvailable = 6ull * 1024 * M;
        current.accountedGrowth = *budget.watermark();
        current.generation = 2;
        current.sampledAtMs = 2000;
        current.decision = policy.update(current.memory, 2000);
        require(current.decision.state == Resource::OpenGlPressure::Recovering,
            "fixture skipped recovery hysteresis");
        require(permitRefusal([&]{ auto job = budget.tryExperimentJob(current); }).reasons == B::PressurePolicy,
            "valid recovery headroom bypassed the Background policy");
        for (std::uint64_t now : {3000, 4000, 5000, 6000})
        {
            ++current.generation;
            current.sampledAtMs = now;
            current.decision = policy.update(current.memory, now);
        }
        ++current.generation;
        current.sampledAtMs = 6999;
        current.decision = policy.update(current.memory, 6999);
        require(budget.experimentJobReceipt(current).reasons & B::PressurePolicy,
            "cook admitted before the real recovery interval elapsed");
        ++current.generation;
        current.sampledAtMs = 7000;
        current.decision = policy.update(current.memory, 7000);
        require(current.decision.state == Resource::OpenGlPressure::Normal,
            "fixture did not restore actual Normal admission");
        require(budget.experimentJobReceipt(current).reasons == 0,
            "renewed valid headroom was permanently refused after transient pressure");
        auto job = budget.tryExperimentJob(current);
        require(budget.stats().experimentJobs == 1,
            "renewed eligible cook did not acquire its bounded permit");
        // A previously eligible advisory receipt cannot promise later admission.
        const auto busy = permitRefusal([&]{ auto raced = budget.tryExperimentJob(current); });
        require(busy.reasons == B::ExperimentJobLimit && busy.experimentJobs == 1 && busy.experimentJobLimit == 1,
            "racing permit acquisition exceeded or concealed the concurrency cap");
        require(budget.stats().experimentJobs == 1 && budget.stats().futureBytes == 0,
            "racing refusal leaked a job or invented allocation debt");
        job.finish();
        require(budget.experimentJobReceipt(current).reasons == 0 && budget.stats().experimentJobs == 0,
            "released permit could not be retried without a source reset");
    }
    void permitValidityAndAllocationFloors()
    {
        B budget(config());
        auto current = healthy(budget);
        current.growthWatermarkValid = false;
        const auto noWatermark = permitRefusal([&]{ auto job = budget.tryExperimentJob(current); });
        require(noWatermark.reasons == B::MissingGrowthWatermark && noWatermark.pressureReasons == 0,
            "missing growth watermark was confused with valid pressure state");
        current = healthy(budget);
        current.memory.physicalValid = false;
        const auto invalidPhysical = permitRefusal([&]{ auto job = budget.tryExperimentJob(current); });
        require((invalidPhysical.reasons & B::PressurePolicy)
            && (invalidPhysical.pressureReasons & B::PhysicalInvalid),
            "invalid physical sample was not independently retained");
        const auto missingSample = permitRefusal([&]{ auto job = budget.tryExperimentJob({}); });
        require((missingSample.reasons & (B::PressurePolicy | B::MissingGrowthWatermark))
                == (B::PressurePolicy | B::MissingGrowthWatermark)
            && (missingSample.pressureReasons & B::MissingGeneration),
            "empty/stale sampler observation became a valid permit");
        current = healthy(budget);
        current.memory.physicalAvailable = current.decision.limits.physicalReserve;
        require(budget.experimentJobReceipt(current).reasons == 0,
            "zero-byte advisory permit consumed hypothetical future bytes");
        auto job = budget.tryExperimentJob(current);
        require(refusal([&]{ auto work = budget.reserve(current, M, P::Background, true); }) & B::PhysicalHeadroom,
            "advisory permit bypassed the subsequent actual allocation floor");
        job.finish();
        --current.memory.physicalAvailable;
        require(permitRefusal([&]{ auto next = budget.tryExperimentJob(current); }).reasons == B::PhysicalHeadroom,
            "zero-byte preflight ignored already insufficient physical headroom");
        current = healthy(budget);
        current.memory.commitAvailable = current.decision.limits.commitReserve - 1;
        require(permitRefusal([&]{ auto next = budget.tryExperimentJob(current); }).reasons == B::ProcessCommitHeadroom,
            "cook preflight ignored the process commit floor");
        current = healthy(budget);
        current.memory.systemCommitAvailable = current.decision.limits.commitReserve - 1;
        require(permitRefusal([&]{ auto next = budget.tryExperimentJob(current); }).reasons == B::SystemCommitHeadroom,
            "cook preflight ignored the independent system commit floor");
        require(budget.stats().experimentJobs == 0 && budget.stats().futureBytes == 0
            && budget.stats().experimentFutureBytes == 0,
            "refused permit or allocation leaked the common/child ledger");
        B legacy;
        require(legacy.experimentJobReceipt({}).reasons == 0,
            "disabled experimental policy acquired a new sampler requirement");
        auto inactive = legacy.tryExperimentJob({});
        require(legacy.stats().experimentJobs == 0 && legacy.stats().experimentDenied == 0,
            "legacy no-op permit changed historical accounting");
    }
    void failedScopeKeepsProtectedCaller()
    {
        B budget(config());
        auto recovering = healthy(budget);
        recovering.decision.state = Resource::OpenGlPressure::Recovering;
        recovering.decision.admissionsPerSample = 0;
        ControlledSample samples{budget, recovering};
        Resource::SpeculativeScope foundation(&budget, &ControlledSample::read, &samples, P::NearFuture);
        Resource::SpeculativeScope::Stage ordinary(8*M, 0);
        const auto failed = permitRefusal([]{ Resource::ExperimentalSpeculativeScope optional; });
        require(failed.reasons == B::PressurePolicy && failed.priority == P::Background,
            "failed optional scope took the protected caller's priority");
        const auto caller = Resource::SpeculativeScope::capture();
        require(caller.budget == &budget && caller.owner == &samples && caller.priority == P::NearFuture
            && !caller.experimental && budget.stats().experimentJobs == 0,
            "failed optional scope did not restore the exact protected caller");
        int key = 0;
        auto protectedOwner = Resource::SpeculativeScope::track(&budget, {&key, 7}, M, true);
        require(protectedOwner && budget.stats().knownOwnerBytes == M && budget.stats().futureBytes == 7*M
            && budget.stats().experimentFutureBytes == 0 && budget.stats().experimentRetainedBytes == 0,
            "failed cook reclassified or detached the protected stage's owner");
        {
            Resource::SpeculativeScope missingSampler(&budget, nullptr, nullptr, P::NearFuture);
            const auto missing = permitRefusal([]{ Resource::ExperimentalSpeculativeScope optional; });
            require(missing.jobPermit && (missing.pressureReasons & B::MissingGeneration),
                "missing scope sampler returned an unclassified cook denial");
            require(!Resource::SpeculativeScope::capture().experimental && budget.stats().experimentJobs == 0,
                "missing sampler denial leaked an experimental scope or permit");
        }
        require(Resource::SpeculativeScope::capture().owner == &samples,
            "failed nested caller did not restore the outer sampler");
    }
    void concurrencyAndLegacy()
    {
        B budget(config());std::latch ready(8),start(1),attempted(8),release(1);
        std::atomic_uint admitted=0;std::vector<std::thread> workers;
        for(unsigned i=0;i<8;++i) workers.emplace_back([&]{
            ready.count_down();start.wait();
            try {auto stage=budget.reserve(healthy(budget),64*M,P::Background,true);
                ++admitted;attempted.count_down();release.wait();}
            catch(const Resource::SpeculativeDeferred&) {attempted.count_down();}
        });
        ready.wait();start.count_down();attempted.wait();
        const auto stats=budget.stats();release.count_down();for(auto& worker:workers) worker.join();
        require(admitted==2 && stats.experimentFutureBytes==128*M && stats.futureBytes==128*M,
            "concurrent reservations were not atomically intersected");
        require(budget.stats().futureBytes==0 && budget.stats().experimentFutureBytes==0,
            "concurrent destruction leaked reservations");
        B legacy;int key=0;
        auto stage=legacy.reserve(healthy(legacy),200*M,P::Background,true);
        auto owner=legacy.track({&key,7},150*M,true,&stage,true);
        require(!stage.experimental() && legacy.stats().experimentRetainedBytes==0
            && legacy.stats().knownOwnerBytes==150*M && legacy.stats().futureBytes==50*M,
            "disabled child quota changed historical accounting");
    }
    void explicitActiveOwnerPriority()
    {
        B budget(config());
        auto recovering=healthy(budget);
        recovering.decision.state=Resource::OpenGlPressure::Recovering;
        recovering.decision.admissionsPerSample=0;
        ControlledSample samples{budget,recovering};
        Resource::SpeculativeScope foundation(&budget,&ControlledSample::read,&samples,P::NearFuture);
        Resource::SpeculativeScope::Stage ordinary(8*M,0);
        const auto current=ControlledSample::read(&samples);
        const auto unqualified=allocationRefusal([]{Resource::SpeculativeScope::Stage copy(M,0,true);});
        require(unqualified.reasons==B::PressurePolicy && unqualified.priority==P::Background
            && budget.stats().futureBytes==8*M && budget.stats().experimentFutureBytes==0,
            "explicit experimental stage inherited unqualified ordinary NearFuture priority");
        require(budget.experimentJobReceipt(current).reasons==B::PressurePolicy,
            "default experiment borrowed active-owner recovery priority");
        const auto permitted=budget.experimentJobReceipt(current,P::NearFuture);
        require(permitted.reasons==0 && permitted.priority==P::NearFuture && permitted.jobPermit
            && permitted.physicalFloor==current.decision.limits.physicalReserve
            && permitted.commitFloor==current.decision.limits.commitReserve,
            "explicit active recovery permit changed its physical/commit floors");
        {
            Resource::ExperimentalSpeculativeScope active(P::NearFuture);
            Resource::ExperimentalSpeculativeScope nested;
            require(Resource::SpeculativeScope::capture().experimental
                && Resource::SpeculativeScope::capture().priority==P::NearFuture
                && budget.stats().experimentJobs==1 && budget.stats().jobs==0,
                "nested default scope demoted active priority or acquired another worker/job");
            const auto occupied=permitRefusal([&]{
                auto second=budget.tryExperimentJob(ControlledSample::read(&samples),P::NearFuture);
            });
            require(occupied.reasons==B::ExperimentJobLimit && occupied.priority==P::NearFuture,
                "active owner escaped the same child concurrency limit");
            Resource::SpeculativeScope::Stage privateCopy(0,0,true);
            privateCopy.reserveAdditional(24*M,200*M);
            require(budget.stats().futureBytes==32*M && budget.stats().experimentFutureBytes==24*M,
                "explicit experimental extension lost priority or parent/child intersection");
            {
                auto ownerEstimate=Resource::SpeculativeScope::reserveOwnerEstimate(&budget,M);
                require(ownerEstimate.experimental() && budget.stats().futureBytes==33*M
                    && budget.stats().experimentFutureBytes==25*M,
                    "active owner estimate escaped its experimental domain");
            }
            require(allocationRefusal([&]{privateCopy.reserveAdditional(105*M,200*M);}).reasons
                == B::ExperimentScratchLimit,"active owner extension bypassed the child scratch cap");
        }
        require(!Resource::SpeculativeScope::capture().experimental
            && Resource::SpeculativeScope::capture().priority==P::NearFuture
            && budget.stats().experimentJobs==0 && budget.stats().futureBytes==8*M
            && budget.stats().experimentFutureBytes==0,
            "active scope did not restore the ordinary caller and all child debt");

        samples.value.memory.physicalAvailable=samples.value.decision.limits.physicalReserve+8*M;
        {
            Resource::ExperimentalSpeculativeScope active(P::NearFuture);
            const auto floor=allocationRefusal([]{Resource::SpeculativeScope::Stage copy(M,0,true);});
            require(floor.reasons==B::PhysicalHeadroom && floor.priority==P::NearFuture
                && floor.physicalFloor==samples.value.decision.limits.physicalReserve,
                "active zero-byte permit bypassed the actual recovery allocation floor");
        }
        samples.value=recovering;
        {
            auto held=budget.reserve(ControlledSample::read(&samples),200*M,P::NearFuture);
            Resource::ExperimentalSpeculativeScope active(P::NearFuture);
            const auto cap=allocationRefusal([]{Resource::SpeculativeScope::Stage copy(64*M,0,true);});
            require(cap.reasons==B::AggregateTransientLimit && budget.stats().futureBytes==208*M
                && budget.stats().experimentFutureBytes==0,
                "active owner created an independent common spending pool");
        }
        {
            int a=0,b=0;
            auto first=retain(budget,&a,100*M);
            auto second=retain(budget,&b,100*M);
            Resource::ExperimentalSpeculativeScope active(P::NearFuture);
            const auto cap=allocationRefusal([]{Resource::SpeculativeScope::Stage copy(57*M,0,true);});
            require(cap.reasons==B::ExperimentTotalLimit && budget.stats().experimentRetainedBytes==200*M
                && budget.stats().futureBytes==8*M,
                "active owner escaped retained-output and construction overlap");
        }
        for(auto state:{Resource::OpenGlPressure::Critical,Resource::OpenGlPressure::Degraded})
        {
            samples.value=recovering;
            samples.value.decision.state=state;
            const auto blocked=permitRefusal([]{Resource::ExperimentalSpeculativeScope active(P::NearFuture);});
            require(blocked.reasons==B::PressurePolicy && (blocked.pressureReasons&B::NearFutureState)
                && budget.stats().experimentJobs==0 && !Resource::SpeculativeScope::capture().experimental,
                "explicit active owner bypassed critical/degraded pressure");
        }
        samples.value=recovering;
        samples.value.generation=0;
        require(permitRefusal([]{Resource::ExperimentalSpeculativeScope active(P::NearFuture);})
            .pressureReasons&B::MissingGeneration,"explicit active owner bypassed missing sample generation");
        samples.value=recovering;
        samples.value.growthWatermarkValid=false;
        require(permitRefusal([]{Resource::ExperimentalSpeculativeScope active(P::NearFuture);})
            .reasons==B::MissingGrowthWatermark,"explicit active owner bypassed the growth watermark");
        samples.value=healthy(budget);
        {
            Resource::ExperimentalSpeculativeScope background;
            Resource::ExperimentalSpeculativeScope nested(P::NearFuture);
            require(Resource::SpeculativeScope::capture().priority==P::Background
                && budget.stats().experimentJobs==1,
                "nested explicit scope changed an existing Background authorization");
            samples.value=recovering;
            require(allocationRefusal([]{Resource::SpeculativeScope::Stage copy(M,0,true);}).reasons
                == B::PressurePolicy,"nested explicit scope escaped Background recovery policy");
        }
        samples.value=recovering;
        samples.value.decision.state=Resource::OpenGlPressure::Caution;
        samples.value.memory.physicalAvailable=3ull*1024*M;
        const auto caution=budget.experimentJobReceipt(ControlledSample::read(&samples),P::NearFuture);
        require(caution.reasons==0 && caution.physicalFloor==samples.value.decision.limits.physicalCritical
            && caution.commitFloor==samples.value.decision.limits.commitCritical,
            "active caution priority changed the existing NearFuture floors");
        {Resource::ExperimentalSpeculativeScope active(P::NearFuture);
         Resource::SpeculativeScope::Stage copy(M,0,true);}
        require(budget.stats().experimentJobs==0 && budget.stats().futureBytes==8*M
            && budget.stats().experimentFutureBytes==0 && budget.stats().experimentRetainedBytes==0,
            "priority refusal or nested scope leaked accounting");
    }
}
int main()
{
    try {intersections();canonicalAndPending();unknownAndPins();scopesAndPressure();
        permitRecoveryAndRecheck();permitValidityAndAllocationFloors();failedScopeKeepsProtectedCaller();concurrencyAndLegacy();
        explicitActiveOwnerPriority();
        std::cout<<"Experimental cache budget: parent/child intersections, canonical ownership, pending debt, source pins, scopes, exact permit receipts, recovery/retry, allocation floors, protected caller, explicit active priority, concurrency and legacy PASS\n";return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
