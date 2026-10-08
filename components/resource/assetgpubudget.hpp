#ifndef OPENMW_RESOURCE_ASSETGPUBUDGET_H
#define OPENMW_RESOURCE_ASSETGPUBUDGET_H

#include "assetpolicy.hpp"
#include <components/debug/v3gpumemory.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace Resource::AssetGpuBudget
{
    inline std::atomic_uint64_t liveBytes{0}, peakBytes{0}, admitted{0}, denied{0};
    inline constexpr std::uint64_t GlobalLimit = 256 * AssetPolicy::MiB;
    inline constexpr std::uint64_t HeadroomReserve = 512 * AssetPolicy::MiB;
    inline constexpr std::uint64_t MaximumSampleAgeMs = 2000;
    inline std::uint64_t milliseconds()
    { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()); }

    enum Refusal : std::uint32_t
    {
        InvalidRequest = 1u << 0, LocalRequestLimit = 1u << 1, GlobalRequestLimit = 1u << 2,
        InvalidSample = 1u << 3, PressurePolicy = 1u << 4, RegressedSampleClock = 1u << 5,
        StaleSample = 1u << 6, HeadroomReserveLimit = 1u << 7, RequestHeadroom = 1u << 8,
        GlobalDebtLimit = 1u << 9, DebtHeadroom = 1u << 10, ArithmeticOverflow = 1u << 11,
        ReservationAllocationFailed = 1u << 12, RetainAllocationFailed = 1u << 13,
    };
    inline constexpr unsigned RefusalCount = 14;
    inline constexpr std::array<const char*, RefusalCount> RefusalNames{
        "invalid_request", "local_request_limit", "global_request_limit", "invalid_sample",
        "pressure_policy", "regressed_sample_clock", "stale_sample", "headroom_reserve",
        "request_headroom", "global_debt_limit", "debt_headroom", "arithmetic_overflow",
        "reservation_allocation_failed", "retain_allocation_failed" };

    enum class Operation : unsigned { Reservation = 0, Retention = 1 };
    struct ReservationReceipt
    {
        unsigned schema = 1;
        Operation operation = Operation::Reservation;
        std::uint32_t reasons = 0;
        std::uint64_t sequence = 0, requestedBytes = 0, localLimit = 0, globalLimit = GlobalLimit;
        std::uint64_t currentDebt = 0, proposedDebt = 0, headroomReserve = HeadroomReserve;
        std::uint64_t nowMs = 0, sampleAgeMs = 0;
        Debug::V3GpuMemory::AdmissionSample sample;
        bool admitted = false, retained = false;
    };

    // A pure replay of the existing policy. Predicates overlap; their counters
    // are observations, not distinct allocations. The sampler observation is
    // identified separately from the debt at the reservation decision's CAS.
    inline ReservationReceipt evaluate(std::uint64_t bytes, std::uint64_t localLimit,
        const Debug::V3GpuMemory::AdmissionSample& sample, std::uint64_t now, std::uint64_t used)
    {
        ReservationReceipt receipt;
        receipt.requestedBytes = bytes;
        receipt.localLimit = localLimit;
        receipt.currentDebt = used;
        receipt.proposedDebt = bytes > std::numeric_limits<std::uint64_t>::max() - used
            ? std::numeric_limits<std::uint64_t>::max() : used + bytes;
        receipt.sample = sample;
        receipt.nowMs = now;
        receipt.sampleAgeMs = now < sample.sampledAtMs
            ? std::numeric_limits<std::uint64_t>::max() : now - sample.sampledAtMs;
        if (!bytes) receipt.reasons |= InvalidRequest;
        if (bytes > localLimit) receipt.reasons |= LocalRequestLimit;
        if (bytes > GlobalLimit) receipt.reasons |= GlobalRequestLimit;
        if (!sample.valid) receipt.reasons |= InvalidSample;
        if (sample.pressure != Debug::V3GpuMemory::PressureState::Comfortable)
            receipt.reasons |= PressurePolicy;
        if (now < sample.sampledAtMs) receipt.reasons |= RegressedSampleClock;
        else if (receipt.sampleAgeMs > MaximumSampleAgeMs) receipt.reasons |= StaleSample;
        if (sample.headroomBytes < HeadroomReserve) receipt.reasons |= HeadroomReserveLimit;
        else
        {
            const auto spendable = sample.headroomBytes - HeadroomReserve;
            if (bytes > spendable) receipt.reasons |= RequestHeadroom;
            if (used > spendable || bytes > spendable - used) receipt.reasons |= DebtHeadroom;
        }
        if (used > GlobalLimit || bytes > GlobalLimit - used) receipt.reasons |= GlobalDebtLimit;
        if (bytes > std::numeric_limits<std::uint64_t>::max() - used)
            receipt.reasons |= ArithmeticOverflow;
        return receipt;
    }

    struct Stats
    {
        std::uint64_t sequence = 0, reservationAttempts = 0, reservationAdmitted = 0, reservationDenied = 0;
        std::uint64_t retentionAttempts = 0, retentionSucceeded = 0, retentionFailed = 0;
        std::uint64_t currentDebt = 0, peakDebt = 0;
        std::array<std::uint64_t, RefusalCount> refusalCounts{};
        std::array<bool, RefusalCount> hasFirstRefusal{};
        std::array<ReservationReceipt, RefusalCount> firstRefusal{};
        ReservationReceipt lastReservation, lastRetention;
    };
    inline std::mutex receiptMutex;
    inline Stats published;

    // Fixed-size publication needs no allocation, including on allocation
    // failure. Recording takes no driver query and does not retain trace rows.
    inline void publish(ReservationReceipt& receipt)
    {
        std::lock_guard lock(receiptMutex);
        receipt.sequence = ++published.sequence;
        if (receipt.operation == Operation::Reservation)
        {
            ++published.reservationAttempts;
            if (receipt.admitted) ++published.reservationAdmitted;
            else ++published.reservationDenied;
            published.lastReservation = receipt;
        }
        else
        {
            ++published.retentionAttempts;
            if (receipt.retained) ++published.retentionSucceeded;
            else ++published.retentionFailed;
            published.lastRetention = receipt;
        }
        for (unsigned index = 0; index < RefusalCount; ++index)
            if (receipt.reasons & (1u << index))
            {
                ++published.refusalCounts[index];
                if (!published.hasFirstRefusal[index])
                {
                    published.hasFirstRefusal[index] = true;
                    published.firstRefusal[index] = receipt;
                }
            }
    }
    inline Stats stats()
    {
        std::lock_guard lock(receiptMutex);
        auto result = published;
        // Live debt can change independently of publication. Each receipt's
        // current/proposed debt is exact at its decision, not at this read.
        result.currentDebt = liveBytes.load();
        result.peakDebt = peakBytes.load();
        return result;
    }

    class Reservation;
    namespace Detail
    {
        template<class BeforeAllocate>
        std::shared_ptr<Reservation> reserveChecked(
            std::uint64_t, std::uint64_t, ReservationReceipt*, BeforeAllocate);
    }
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
        template<class BeforeAllocate>
        friend std::shared_ptr<Reservation> Detail::reserveChecked(
            std::uint64_t, std::uint64_t, ReservationReceipt*, BeforeAllocate);
        std::uint64_t mBytes;
    };
    // Hold through final-use completion AND actual targeted deletion. Objects
    // whose OSG allocation may be pooled must retain debt to context discard.
    namespace Detail
    {
    // The no-op hook in production adds no layout or policy difference. A
    // fixture may throw bad_alloc at the exact control-block allocation seam
    // to exercise rollback/publication without changing process allocators.
    template<class BeforeAllocate>
    std::shared_ptr<Reservation> reserveChecked(std::uint64_t bytes,
        std::uint64_t localLimit, ReservationReceipt* output, BeforeAllocate beforeAllocate)
    {
        const auto sample=Debug::V3GpuMemory::admissionSample();
        const auto now=milliseconds();
        auto used=liveBytes.load();
        ReservationReceipt receipt;
        for (;;)
        {
            receipt = evaluate(bytes, localLimit, sample, now, used);
            if (receipt.reasons)
            {
                ++denied;
                publish(receipt);
                if (output) *output = receipt;
                return {};
            }
            if (liveBytes.compare_exchange_weak(used, used + bytes)) break;
            // Failed CAS supplies the new debt. Re-evaluate every predicate so
            // a final refusal or admission is bound to that exact observation.
        }
        auto peak=peakBytes.load();
        while(peak<used+bytes && !peakBytes.compare_exchange_weak(peak,used+bytes)) {}
        std::shared_ptr<Reservation> result;
        try
        {
            beforeAllocate();
            result=std::make_shared<Reservation>(bytes,Reservation::Key{});
            ++admitted;
            receipt.admitted = true;
        }
        catch(...)
        {
            liveBytes.fetch_sub(bytes);
            ++denied;
            receipt.reasons |= ReservationAllocationFailed;
        }
        publish(receipt);
        if (output) *output = receipt;
        return result;
    }
    }
    inline std::shared_ptr<Reservation> reserve(std::uint64_t bytes,
        std::uint64_t localLimit=AssetPolicy::TextureAllocationLimit, ReservationReceipt* output=nullptr)
    { return Detail::reserveChecked(bytes, localLimit, output, [] {}); }

    // Conservative accounting for ordinary OSG-owned resources. A C++ graph
    // release alone cannot prove that the driver allocation left its pool.
    inline std::mutex retainedMutex;
    inline std::vector<std::shared_ptr<Reservation>> retainedUntilShutdown;
    namespace Detail
    {
    template<class BeforeAllocate>
    bool retainChecked(std::shared_ptr<Reservation> token, BeforeAllocate beforeAllocate)
    {
        if(!token) return false;
        ReservationReceipt receipt;
        receipt.operation = Operation::Retention;
        receipt.requestedBytes = token->bytes();
        receipt.currentDebt = liveBytes.load();
        receipt.proposedDebt = receipt.currentDebt;
        receipt.sample = Debug::V3GpuMemory::admissionSample();
        receipt.nowMs = milliseconds();
        receipt.sampleAgeMs = receipt.nowMs < receipt.sample.sampledAtMs
            ? std::numeric_limits<std::uint64_t>::max() : receipt.nowMs - receipt.sample.sampledAtMs;
        try
        {
            std::lock_guard lock(retainedMutex);
            beforeAllocate();
            retainedUntilShutdown.push_back(std::move(token));
            receipt.retained = true;
        }
        catch(...) { receipt.reasons = RetainAllocationFailed; }
        publish(receipt);
        return receipt.retained;
    }
    }
    inline bool retainUntilShutdown(std::shared_ptr<Reservation> token)
    {
        return Detail::retainChecked(std::move(token), [] {});
    }
    inline void discardAfterContextsClosed()
    { std::lock_guard lock(retainedMutex); retainedUntilShutdown.clear(); }
}
#endif
