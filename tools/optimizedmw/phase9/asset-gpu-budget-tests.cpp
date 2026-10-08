#include <components/resource/assetgpubudget.hpp>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

int main() try
{
    using namespace Resource::AssetGpuBudget;
    const auto require=[](bool value,const char* message){if(!value) throw std::runtime_error(message);};
    const auto sample=[](bool valid,Debug::V3GpuMemory::PressureState pressure,std::uint64_t age,std::uint64_t bytes) {
        std::lock_guard lock(Debug::V3GpuMemory::sAdmissionMutex);
        Debug::V3GpuMemory::sAdmissionSample={bytes,milliseconds()-age,pressure,valid};
    };
    using P=Debug::V3GpuMemory::PressureState;
    constexpr auto MiB=Resource::AssetPolicy::MiB;
    constexpr std::uint64_t ReplayNow = 10000;
    constexpr std::uint64_t CapturedDebt = 935960;
    constexpr std::uint64_t TinyRequest = 4096;
    const Debug::V3GpuMemory::AdmissionSample comfortable{4096*MiB,ReplayNow,P::Comfortable,true};
    const auto replay=[&](std::uint64_t bytes,std::uint64_t used,
        const Debug::V3GpuMemory::AdmissionSample& value) {
        return evaluate(bytes,Resource::AssetPolicy::TextureAllocationLimit,value,ReplayNow,used);
    };
    auto receipt=replay(TinyRequest,CapturedDebt,comfortable);
    require(!receipt.reasons && receipt.requestedBytes==TinyRequest && receipt.currentDebt==CapturedDebt
        && receipt.proposedDebt==CapturedDebt+TinyRequest && receipt.localLimit==128*MiB
        && receipt.globalLimit==256*MiB && receipt.headroomReserve==512*MiB
        && receipt.sample.headroomBytes==comfortable.headroomBytes && receipt.sampleAgeMs==0
        && !receipt.admitted,"pure admission receipt lost exact inputs or claimed allocation");
    auto replaySample=comfortable;
    replaySample.sampledAtMs=ReplayNow-MaximumSampleAgeMs;
    require(!replay(1,0,replaySample).reasons,"exact 2-second sample boundary denied");
    --replaySample.sampledAtMs;
    require(replay(1,0,replaySample).reasons==StaleSample,"stale replay did not isolate sample age");
    replaySample=comfortable; replaySample.sampledAtMs=ReplayNow+1;
    receipt=replay(1,0,replaySample);
    require(receipt.reasons==RegressedSampleClock
        && receipt.sampleAgeMs==std::numeric_limits<std::uint64_t>::max(),"future sample clock underflowed");
    replaySample=comfortable; replaySample.valid=false;
    require(replay(1,0,replaySample).reasons==InvalidSample,"invalid replay did not isolate validity");
    replaySample=comfortable; replaySample.pressure=P::Hard;
    require(replay(1,0,replaySample).reasons==PressurePolicy,"pressure replay did not isolate policy");
    replaySample=comfortable; replaySample.headroomBytes=HeadroomReserve-1;
    require(replay(1,0,replaySample).reasons==HeadroomReserveLimit,"headroom reserve replay lost boundary");
    replaySample.headroomBytes=HeadroomReserve;
    require(replay(1,0,replaySample).reasons==(RequestHeadroom|DebtHeadroom),
        "overlapping request/debt headroom predicates were treated as disjoint");
    replaySample.headroomBytes=HeadroomReserve+CapturedDebt+TinyRequest;
    require(!replay(TinyRequest,CapturedDebt,replaySample).reasons,"exact affordable debt boundary denied");
    --replaySample.headroomBytes;
    require(replay(TinyRequest,CapturedDebt,replaySample).reasons==DebtHeadroom,
        "current debt did not contribute to exact headroom refusal");
    require(replay(129*MiB,0,comfortable).reasons==LocalRequestLimit,"local limit replay changed");
    receipt=evaluate(257*MiB,512*MiB,comfortable,ReplayNow,0);
    require(receipt.reasons==(GlobalRequestLimit|GlobalDebtLimit),"global request and debt gates lost overlap");
    require(replay(2,GlobalLimit-1,comfortable).reasons==GlobalDebtLimit,"aggregate debt replay changed");
    receipt=replay(1,std::numeric_limits<std::uint64_t>::max(),comfortable);
    require(receipt.reasons==(GlobalDebtLimit|DebtHeadroom|ArithmeticOverflow)
        && receipt.proposedDebt==std::numeric_limits<std::uint64_t>::max(),"overflow replay wrapped proposed debt");
    require(replay(0,0,comfortable).reasons==InvalidRequest,"zero request replay changed");
    replaySample={HeadroomReserve,ReplayNow-MaximumSampleAgeMs-1,P::Soft,false};
    require(replay(1,0,replaySample).reasons==(InvalidSample|PressurePolicy|StaleSample|RequestHeadroom|DebtHeadroom),
        "independent overlapping refusal predicates were discarded");

    sample(false,P::Comfortable,0,4096*MiB);
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==InvalidSample
        && !receipt.sample.valid && receipt.currentDebt==0,"unavailable sample admitted or receipt incoherent");
    const auto firstInvalid=stats().firstRefusal[3];
    sample(true,P::Comfortable,3000,4096*MiB);
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==StaleSample
        && receipt.sampleAgeMs>MaximumSampleAgeMs,"stale sample admitted or exact age missing");
    sample(true,P::Soft,0,4096*MiB);
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==PressurePolicy
        && receipt.sample.pressure==P::Soft,"soft pressure admitted or wrong receipt");
    sample(true,P::Comfortable,0,HeadroomReserve);
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==(RequestHeadroom|DebtHeadroom),
        "headroom reserve invaded or overlapping predicates lost");
    sample(false,P::Comfortable,0,4096*MiB); require(!reserve(2),"second invalid sample admitted");
    require(stats().firstRefusal[3].sequence==firstInvalid.sequence
        && stats().firstRefusal[3].requestedBytes==1,"bounded first refusal was overwritten");
    sample(true,P::Comfortable,0,4096*MiB);
    require(!reserve(0,128*MiB,&receipt) && receipt.reasons==InvalidRequest,
        "invalid zero request failed production receipt");
    {
        std::lock_guard lock(Debug::V3GpuMemory::sAdmissionMutex);
        Debug::V3GpuMemory::sAdmissionSample={4096*MiB,milliseconds()+1000,P::Comfortable,true};
    }
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==RegressedSampleClock,
        "regressed clock failed production receipt");
    sample(true,P::Comfortable,0,HeadroomReserve-1);
    require(!reserve(1,128*MiB,&receipt) && receipt.reasons==HeadroomReserveLimit,
        "headroom floor failed production receipt");
    sample(true,P::Comfortable,0,4096*MiB);
    require(!reserve(129*MiB),"single texture-domain limit ignored");
    require(!reserve(257*MiB,512*MiB,&receipt) && receipt.reasons==(GlobalRequestLimit|GlobalDebtLimit),
        "global request failed production overlapping receipt");
    auto overflowOwner=reserve(TinyRequest);
    require(overflowOwner && !reserve(std::numeric_limits<std::uint64_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),&receipt)
        && receipt.reasons==(GlobalRequestLimit|RequestHeadroom|GlobalDebtLimit|DebtHeadroom|ArithmeticOverflow)
        && receipt.currentDebt==TinyRequest
        && receipt.proposedDebt==std::numeric_limits<std::uint64_t>::max(),
        "production arithmetic refusal did not retain exact owner debt");
    overflowOwner.reset(); require(liveBytes==0,"overflow refusal changed prior live debt");
    auto first=reserve(128*MiB), second=reserve(128*MiB);
    require(first && second && liveBytes==GlobalLimit && !reserve(1,128*MiB,&receipt)
        && receipt.reasons==GlobalDebtLimit && receipt.currentDebt==GlobalLimit
        && receipt.proposedDebt==GlobalLimit+1,"aggregate overlap limit ignored or final CAS debt lost");
    first.reset(); second.reset(); require(liveBytes==0,"targeted release retained debt");

    // Replay the captured 935,960-byte debt through real reservation tokens,
    // using a fresh explicit fixture sample. This is correctness under that
    // debt, not a claim that the capture's unavailable GPU refusal sample was
    // comfortable or that reducing GPU protection would be safe.
    sample(true,P::Comfortable,0,HeadroomReserve+CapturedDebt+TinyRequest);
    auto captured=reserve(CapturedDebt);
    auto tiny=reserve(TinyRequest,128*MiB,&receipt);
    require(captured && tiny && receipt.admitted && !receipt.reasons
        && receipt.currentDebt==CapturedDebt && receipt.proposedDebt==CapturedDebt+TinyRequest
        && liveBytes==CapturedDebt+TinyRequest,"actual tiny request failed safe captured-debt boundary");
    tiny.reset(); captured.reset(); require(liveBytes==0,"captured-debt fixture leaked reservations");

    sample(true,P::Comfortable,0,4096*MiB);
    const auto beforeAllocation=stats();
    bool allocationSeamOwned=false;
    require(!Detail::reserveChecked(TinyRequest,128*MiB,&receipt,[&] {
        allocationSeamOwned=liveBytes==TinyRequest;
        throw std::bad_alloc();
    }),"reservation allocation failure admitted");
    const auto afterAllocation=stats();
    require(allocationSeamOwned && liveBytes==0 && receipt.reasons==ReservationAllocationFailed && !receipt.admitted
        && receipt.currentDebt==0 && receipt.proposedDebt==TinyRequest
        && afterAllocation.reservationDenied==beforeAllocation.reservationDenied+1
        && afterAllocation.reservationAdmitted==beforeAllocation.reservationAdmitted
        && afterAllocation.refusalCounts[12]==beforeAllocation.refusalCounts[12]+1,
        "reservation allocation failure lost exact receipt or rollback");
    auto failedRetention=reserve(TinyRequest);
    const auto beforeRetention=stats();
    const auto beforeRetainedOwners=retainedUntilShutdown.size();
    require(!Detail::retainChecked(failedRetention,[] { throw std::bad_alloc(); }),
        "retention allocation failure reported success");
    const auto afterRetention=stats();
    require(liveBytes==TinyRequest && afterRetention.retentionFailed==beforeRetention.retentionFailed+1
        && afterRetention.reservationDenied==beforeRetention.reservationDenied
        && afterRetention.lastRetention.operation==Operation::Retention
        && afterRetention.lastRetention.reasons==RetainAllocationFailed
        && afterRetention.lastRetention.requestedBytes==TinyRequest
        && !afterRetention.lastRetention.retained
        && retainedUntilShutdown.size()==beforeRetainedOwners
        && afterRetention.refusalCounts[13]==beforeRetention.refusalCounts[13]+1,
        "retention allocation failure misclassified reservation debt or falsely repaid live owner");
    failedRetention.reset(); require(liveBytes==0,"failed retention owner retained debt after actual release");
    auto pooled=reserve(4*MiB); require(retainUntilShutdown(pooled),"pooled retention failed");
    pooled.reset(); require(liveBytes==4*MiB,"graph release falsely repaid pooled allocation");
    discardAfterContextsClosed(); require(liveBytes==0,"context discard retained pooled debt");

    // Hold all contenders until their decisions are published: exactly one
    // 1MiB reservation may fill a 255MiB existing debt. Failed CAS contenders
    // must report the final 256MiB debt rather than their initial observation.
    sample(true,P::Comfortable,0,4096*MiB);
    auto capOwner0=reserve(128*MiB), capOwner1=reserve(127*MiB);
    require(capOwner0 && capOwner1 && liveBytes==255*MiB,"concurrent cap fixture owners unavailable");
    std::array<ReservationReceipt,8> capReceipts{};
    std::array<bool,8> capAccepted{};
    std::atomic_uint capDecisions{0};
    std::atomic_bool capStart{false}, capRelease{false};
    std::vector<std::thread> capWorkers;
    for(unsigned index=0;index<8;++index) capWorkers.emplace_back([&,index] {
        while(!capStart.load()) std::this_thread::yield();
        auto token=reserve(MiB,128*MiB,&capReceipts[index]);
        capAccepted[index]=bool(token);
        ++capDecisions;
        while(!capRelease.load()) std::this_thread::yield();
    });
    capStart=true;
    while(capDecisions.load()!=8) std::this_thread::yield();
    capRelease=true;
    for(auto& worker:capWorkers) worker.join();
    unsigned capAdmitted=0;
    for(unsigned index=0;index<8;++index)
    {
        const auto& proof=capReceipts[index];
        if(capAccepted[index])
        {
            ++capAdmitted;
            require(proof.admitted && !proof.reasons && proof.currentDebt==255*MiB
                && proof.proposedDebt==GlobalLimit,"CAS admission receipt retained stale debt");
        }
        else require(!proof.admitted && proof.reasons==GlobalDebtLimit
            && proof.currentDebt==GlobalLimit && proof.proposedDebt==257*MiB,
            "CAS refusal receipt retained pre-contention debt");
    }
    require(capAdmitted==1 && liveBytes==255*MiB,"concurrent overlap exceeded cap or lost debt");
    capOwner0.reset(); capOwner1.reset(); require(liveBytes==0,"concurrent cap fixture leaked owners");

    sample(true,P::Comfortable,0,4096*MiB);
    const auto beforeWorkers=stats();
    std::vector<std::thread> workers;
    for(unsigned index=0;index<8;++index) workers.emplace_back([] {
        for(unsigned attempt=0;attempt<100;++attempt) { auto token=reserve(64*1024); std::this_thread::yield(); }
    });
    for(auto& worker:workers) worker.join();
    require(liveBytes==0 && peakBytes<=GlobalLimit,"concurrent accounting debt or overflow");
    const auto finalStats=stats();
    require(finalStats.reservationAttempts==beforeWorkers.reservationAttempts+800
        && finalStats.reservationAttempts==finalStats.reservationAdmitted+finalStats.reservationDenied
        && finalStats.reservationAdmitted==admitted && finalStats.reservationDenied==denied
        && finalStats.retentionAttempts==finalStats.retentionSucceeded+finalStats.retentionFailed,
        "bounded counters lost concurrent attempts or combined retention with reservation decisions");
    for(unsigned index=0;index<RefusalCount;++index)
    {
        const auto& proof=finalStats.firstRefusal[index];
        require(finalStats.refusalCounts[index] && finalStats.hasFirstRefusal[index]
            && (proof.reasons&(1u<<index)) && proof.sequence && proof.sequence<=finalStats.sequence,
            "per-reason first refusal missing or not bound to its exact predicate");
    }
    std::cout<<"PASS asset GPU admission: exact replay/overlapping predicates, stale/pressure/headroom fallback, "
        <<"local and aggregate overlap bounds, actual tiny admission at captured 935960-byte debt, "
        <<"reservation/retention allocation failure receipts, targeted and pooled lifetime, concurrent bounded publication\n";
    // Terminal, exact-build receipt: reached only after every assertion above.
    // Retention rollback means no failed vector owner was published; the
    // already-live caller owner remains charged until its actual release.
    std::cout<<"ASSET_GPU_ADMISSION_PASS schema=1";
    for(unsigned index=0;index<RefusalCount;++index)
        std::cout<<" reason_"<<RefusalNames[index]<<"=1";
    std::cout<<" overlapping_predicates=1 freshness_boundary=1 clock_regression=1"
        <<" captured_debt_tiny_admission=1 reservation_allocation_rollback=1 retention_allocation_rollback=1"
        <<" pooled_lifetime=1 cas_final_debt=1 concurrent_counters=1\n";
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
