#ifndef OPENMW_RESOURCE_ASSETGPUBUDGET_H
#define OPENMW_RESOURCE_ASSETGPUBUDGET_H

#include "assetpolicy.hpp"
#include <components/debug/v3gpumemory.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace Resource::AssetGpuBudget
{
    inline std::atomic_uint64_t liveBytes{0}, peakBytes{0}, admitted{0}, denied{0};
    inline constexpr std::uint64_t GlobalLimit = 256 * AssetPolicy::MiB;
    inline constexpr std::uint64_t HeadroomReserve = 512 * AssetPolicy::MiB;
    inline std::uint64_t milliseconds()
    { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()); }

    class Reservation
    {
        struct Key {};
    public:
        Reservation(std::uint64_t bytes,Key):mBytes(bytes){}
        Reservation(const Reservation&)=delete;
        Reservation& operator=(const Reservation&)=delete;
        ~Reservation() { liveBytes.fetch_sub(mBytes); }
        std::uint64_t bytes() const { return mBytes; }
    private:
        friend std::shared_ptr<Reservation> reserve(std::uint64_t,std::uint64_t);
        std::uint64_t mBytes;
    };
    // Hold through final-use completion AND actual targeted deletion. Objects
    // whose OSG allocation may be pooled must retain debt to context discard.
    inline std::shared_ptr<Reservation> reserve(std::uint64_t bytes,
        std::uint64_t localLimit=AssetPolicy::TextureAllocationLimit)
    {
        const auto sample=Debug::V3GpuMemory::admissionSample();
        const auto now=milliseconds();
        if (!bytes || bytes>localLimit || bytes>GlobalLimit || !sample.valid
            || sample.pressure!=Debug::V3GpuMemory::PressureState::Comfortable
            || now<sample.sampledAtMs || now-sample.sampledAtMs>2000
            || sample.headroomBytes<HeadroomReserve || bytes>sample.headroomBytes-HeadroomReserve)
        { ++denied; return {}; }
        auto used=liveBytes.load();
        do
        {
            if(used>GlobalLimit-bytes || used>sample.headroomBytes-HeadroomReserve-bytes)
            { ++denied; return {}; }
        } while(!liveBytes.compare_exchange_weak(used,used+bytes));
        auto peak=peakBytes.load();
        while(peak<used+bytes && !peakBytes.compare_exchange_weak(peak,used+bytes)) {}
        try { auto result=std::make_shared<Reservation>(bytes,Reservation::Key{}); ++admitted; return result; }
        catch(...) { liveBytes.fetch_sub(bytes); ++denied; return {}; }
    }

    // Conservative accounting for ordinary OSG-owned resources. A C++ graph
    // release alone cannot prove that the driver allocation left its pool.
    inline std::mutex retainedMutex;
    inline std::vector<std::shared_ptr<Reservation>> retainedUntilShutdown;
    inline bool retainUntilShutdown(std::shared_ptr<Reservation> token)
    {
        if(!token) return false;
        try { std::lock_guard lock(retainedMutex); retainedUntilShutdown.push_back(std::move(token)); return true; }
        catch(...) { return false; }
    }
    inline void discardAfterContextsClosed()
    { std::lock_guard lock(retainedMutex); retainedUntilShutdown.clear(); }
}
#endif
