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
    template<class Function>
    std::uint32_t refusal(Function&& function)
    {
        try {function();}
        catch(const Resource::SpeculativeDeferred& e) {return e.reservation ? e.reservation->reasons : 0;}
        throw std::runtime_error("operation incorrectly admitted");
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
        refusal([&]{auto second=budget.tryExperimentJob(healthy(budget));});
        auto caution=healthy(budget);caution.decision.state=Resource::OpenGlPressure::Caution;
        permit.finish();
        refusal([&]{auto denied=budget.tryExperimentJob(caution);});
        require(budget.stats().experimentJobs==0,"pressure refusal leaked a cooking permit");
        auto degraded=healthy(budget);degraded.decision.state=Resource::OpenGlPressure::Degraded;
        require(refusal([&]{auto denied=budget.reserve(degraded,M,P::Background,true);})&B::PressurePolicy,
            "invalid/degraded pressure granted child capacity");
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
}
int main()
{
    try {intersections();canonicalAndPending();unknownAndPins();scopesAndPressure();concurrencyAndLegacy();
        std::cout<<"Experimental cache budget: parent/child intersections, canonical ownership, pending debt, source pins, scopes, pressure, concurrency and legacy PASS\n";return 0;}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
