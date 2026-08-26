#ifndef OPENMW_COMPONENTS_DEBUG_V3GPUMEMORY_H
#define OPENMW_COMPONENTS_DEBUG_V3GPUMEMORY_H

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include "v3diagnostics.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#endif

namespace Debug::V3GpuMemory
{
    enum class PressureState : int
    {
        Unavailable = -1,
        Comfortable = 0,
        Soft = 1,
        Hard = 2,
    };

    struct Sample
    {
        std::uint64_t mUsageBytes = 0;
        std::uint64_t mBudgetBytes = 0;
        std::uint64_t mAvailableForReservationBytes = 0;
        std::uint64_t mCurrentReservationBytes = 0;
    };

#ifdef _WIN32
    class DxgiSampler
    {
    public:
        ~DxgiSampler()
        {
            if (mAdapter)
                mAdapter->Release();
            if (mDxgi)
                FreeLibrary(mDxgi);
        }

        bool query(Sample& sample)
        {
            if (!initialize())
                return false;

            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            const HRESULT result
                = mAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info);
            if (FAILED(result))
                return false;

            sample.mUsageBytes = static_cast<std::uint64_t>(info.CurrentUsage);
            sample.mBudgetBytes = static_cast<std::uint64_t>(info.Budget);
            sample.mAvailableForReservationBytes = static_cast<std::uint64_t>(info.AvailableForReservation);
            sample.mCurrentReservationBytes = static_cast<std::uint64_t>(info.CurrentReservation);
            return true;
        }

    private:
        bool initialize()
        {
            if (mAttempted)
                return mAdapter != nullptr;
            mAttempted = true;

            mDxgi = LoadLibraryW(L"dxgi.dll");
            if (!mDxgi)
                return false;

            using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
            const auto createFactory
                = reinterpret_cast<CreateFactory>(GetProcAddress(mDxgi, "CreateDXGIFactory1"));
            if (!createFactory)
                return false;

            IDXGIFactory1* factory = nullptr;
            if (FAILED(createFactory(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory)
                return false;

            IDXGIAdapter1* bestAdapter = nullptr;
            SIZE_T bestDedicatedMemory = 0;
            for (UINT index = 0;; ++index)
            {
                IDXGIAdapter1* adapter = nullptr;
                const HRESULT result = factory->EnumAdapters1(index, &adapter);
                if (result == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(result) || !adapter)
                    continue;

                DXGI_ADAPTER_DESC1 description{};
                if (SUCCEEDED(adapter->GetDesc1(&description))
                    && (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0
                    && description.DedicatedVideoMemory >= bestDedicatedMemory)
                {
                    if (bestAdapter)
                        bestAdapter->Release();
                    bestAdapter = adapter;
                    bestDedicatedMemory = description.DedicatedVideoMemory;
                }
                else
                    adapter->Release();
            }
            factory->Release();

            if (!bestAdapter)
                return false;

            IDXGIAdapter3* adapter3 = nullptr;
            const HRESULT queryResult
                = bestAdapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3));
            bestAdapter->Release();
            if (FAILED(queryResult) || !adapter3)
                return false;

            mAdapter = adapter3;
            return true;
        }

        bool mAttempted = false;
        HMODULE mDxgi = nullptr;
        IDXGIAdapter3* mAdapter = nullptr;
    };
#else
    class DxgiSampler
    {
    public:
        bool query(Sample&) { return false; }
    };
#endif

    inline std::atomic<int> sPressureState{ static_cast<int>(PressureState::Unavailable) };

    inline PressureState pressureState()
    {
        return static_cast<PressureState>(sPressureState.load(std::memory_order_relaxed));
    }

    inline bool hardPressure()
    {
        return pressureState() == PressureState::Hard;
    }

    inline const char* pressureName(PressureState state)
    {
        switch (state)
        {
            case PressureState::Comfortable:
                return "comfortable";
            case PressureState::Soft:
                return "soft";
            case PressureState::Hard:
                return "hard";
            case PressureState::Unavailable:
            default:
                return "unavailable";
        }
    }

    inline void sampleIfDue(bool telemetryEnabled, bool managementEnabled, int configuredSoftBudgetMb,
        int configuredHardBudgetMb)
    {
        if (!telemetryEnabled && !managementEnabled)
            return;

        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastSample{};
        const Clock::time_point now = Clock::now();
        if (lastSample != Clock::time_point{}
            && std::chrono::duration<double>(now - lastSample).count() < 1.0)
            return;
        lastSample = now;

        static DxgiSampler sampler;
        Sample sample;
        if (!sampler.query(sample) || sample.mBudgetBytes == 0)
        {
            sPressureState.store(static_cast<int>(PressureState::Unavailable), std::memory_order_relaxed);
            return;
        }

        constexpr std::uint64_t MiB = 1024ull * 1024ull;
        const std::uint64_t configuredSoftBytes
            = static_cast<std::uint64_t>(std::max(configuredSoftBudgetMb, 1)) * MiB;
        const std::uint64_t configuredHardBytes
            = static_cast<std::uint64_t>(std::max(configuredHardBudgetMb, configuredSoftBudgetMb + 128)) * MiB;

        // Respect both the user's configured 8-GB-class limits and WDDM's actual
        // process budget. This preserves headroom when Windows temporarily grants
        // less than the adapter's physical capacity.
        std::uint64_t softBytes
            = std::min(configuredSoftBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.90));
        std::uint64_t hardBytes
            = std::min(configuredHardBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.97));
        if (hardBytes <= softBytes)
            hardBytes = std::min(sample.mBudgetBytes, softBytes + 128ull * MiB);

        PressureState state = PressureState::Comfortable;
        if (sample.mUsageBytes >= hardBytes)
            state = PressureState::Hard;
        else if (sample.mUsageBytes >= softBytes)
            state = PressureState::Soft;
        sPressureState.store(static_cast<int>(state), std::memory_order_relaxed);

        if (!telemetryEnabled)
            return;

        auto& writer = V3Diagnostics::gpuMemoryWriter();
        if (!writer.enabled())
            return;

        const double usageMb = static_cast<double>(sample.mUsageBytes) / static_cast<double>(MiB);
        const double budgetMb = static_cast<double>(sample.mBudgetBytes) / static_cast<double>(MiB);
        const double availableMb
            = static_cast<double>(sample.mAvailableForReservationBytes) / static_cast<double>(MiB);
        const double reservationMb
            = static_cast<double>(sample.mCurrentReservationBytes) / static_cast<double>(MiB);
        const double pressurePercent
            = sample.mBudgetBytes != 0 ? 100.0 * static_cast<double>(sample.mUsageBytes) / sample.mBudgetBytes : 0.0;

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ',' << std::fixed
            << std::setprecision(1) << usageMb << ',' << budgetMb << ',' << availableMb << ',' << reservationMb << ','
            << pressurePercent << ',' << (static_cast<double>(softBytes) / static_cast<double>(MiB)) << ','
            << (static_cast<double>(hardBytes) / static_cast<double>(MiB)) << ','
            << V3Diagnostics::csvQuote(pressureName(state));
        writer.writeLine(row.str());
    }
}

#endif
