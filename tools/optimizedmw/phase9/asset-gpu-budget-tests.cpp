#include <components/resource/assetgpubudget.hpp>
#include <iostream>
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
    sample(false,P::Comfortable,0,4096*MiB); require(!reserve(1),"unavailable sample admitted");
    sample(true,P::Comfortable,3000,4096*MiB); require(!reserve(1),"stale sample admitted");
    sample(true,P::Soft,0,4096*MiB); require(!reserve(1),"soft pressure admitted");
    sample(true,P::Comfortable,0,HeadroomReserve); require(!reserve(1),"headroom reserve invaded");
    sample(true,P::Comfortable,0,4096*MiB);
    require(!reserve(129*MiB),"single texture-domain limit ignored");
    auto first=reserve(128*MiB), second=reserve(128*MiB);
    require(first && second && liveBytes==GlobalLimit && !reserve(1),"aggregate overlap limit ignored");
    first.reset(); second.reset(); require(liveBytes==0,"targeted release retained debt");
    auto pooled=reserve(4*MiB); require(retainUntilShutdown(pooled),"pooled retention failed");
    pooled.reset(); require(liveBytes==4*MiB,"graph release falsely repaid pooled allocation");
    discardAfterContextsClosed(); require(liveBytes==0,"context discard retained pooled debt");
    std::vector<std::thread> workers;
    for(unsigned index=0;index<8;++index) workers.emplace_back([] {
        for(unsigned attempt=0;attempt<100;++attempt) { auto token=reserve(64*1024); std::this_thread::yield(); }
    });
    for(auto& worker:workers) worker.join();
    require(liveBytes==0 && peakBytes<=GlobalLimit,"concurrent accounting debt or overflow");
    std::cout<<"PASS asset GPU admission: stale/pressure/headroom fallback, local and aggregate overlap bounds, targeted and pooled lifetime, concurrent accounting\n";
}
catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
