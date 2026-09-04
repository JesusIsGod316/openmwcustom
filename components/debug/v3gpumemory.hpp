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
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>

// windows.h still exposes legacy near/far macros. OpenMW uses near/far as
// ordinary C++ parameter names in stereo and post-processing headers, so never
// allow those Win32 compatibility macros to escape this helper.
#ifdef near
#undef near
#endif
#ifdef far
#undef far
#endif
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

    struct AdapterSample
    {
        std::uint64_t mUsedBytes = 0;
        std::uint64_t mFreeBytes = 0;
        std::uint64_t mTotalBytes = 0;
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

    // NVML is deliberately discovered at runtime. OpenMW neither links against
    // it nor requires NVIDIA's SDK or driver DLL to be present.
    class NvmlSampler
    {
    public:
        ~NvmlSampler()
        {
            if (mInitialized && mShutdown)
                mShutdown();
            if (mNvml)
                FreeLibrary(mNvml);
        }

        bool query(AdapterSample& sample)
        {
            if (!initialize() || !mMemoryInfo)
                return false;

            NvmlMemory memory{};
            if (mMemoryInfo(mDevice, &memory) != NvmlSuccess)
                return false;

            sample.mUsedBytes = memory.mUsed;
            sample.mFreeBytes = memory.mFree;
            sample.mTotalBytes = memory.mTotal;
            return sample.mTotalBytes != 0;
        }

    private:
        static constexpr int NvmlSuccess = 0;
        struct NvmlDevice;
        using DeviceHandle = NvmlDevice*;
        struct NvmlMemory
        {
            unsigned long long mTotal;
            unsigned long long mFree;
            unsigned long long mUsed;
        };

        using Init = int (*)();
        using Shutdown = int (*)();
        using DeviceGetCount = int (*)(unsigned int*);
        using DeviceGetHandleByIndex = int (*)(unsigned int, DeviceHandle*);
        using DeviceGetMemoryInfo = int (*)(DeviceHandle, NvmlMemory*);

        template <class T>
        T load(const char* name)
        {
            return reinterpret_cast<T>(GetProcAddress(mNvml, name));
        }

        bool initialize()
        {
            if (mAttempted)
                return mInitialized && mDevice != nullptr;
            mAttempted = true;

            mNvml = LoadLibraryW(L"nvml.dll");
            if (!mNvml)
                return false;

            const Init init = load<Init>("nvmlInit_v2");
            mShutdown = load<Shutdown>("nvmlShutdown");
            const DeviceGetCount getCount = load<DeviceGetCount>("nvmlDeviceGetCount_v2");
            const DeviceGetHandleByIndex getHandle
                = load<DeviceGetHandleByIndex>("nvmlDeviceGetHandleByIndex_v2");
            mMemoryInfo = load<DeviceGetMemoryInfo>("nvmlDeviceGetMemoryInfo");
            if (!init || !mShutdown || !getCount || !getHandle || !mMemoryInfo || init() != NvmlSuccess)
                return false;

            mInitialized = true;
            unsigned int count = 0;
            if (getCount(&count) != NvmlSuccess || count == 0)
                return false;

            // The target system has one NVIDIA display adapter. Selecting the
            // device with the most local memory also behaves sensibly on hybrid
            // systems without introducing PCI/DXGI matching dependencies.
            for (unsigned int index = 0; index < count; ++index)
            {
                DeviceHandle candidate = nullptr;
                NvmlMemory memory{};
                if (getHandle(index, &candidate) != NvmlSuccess || !candidate
                    || mMemoryInfo(candidate, &memory) != NvmlSuccess)
                    continue;
                if (!mDevice || memory.mTotal > mSelectedTotalBytes)
                {
                    mDevice = candidate;
                    mSelectedTotalBytes = memory.mTotal;
                }
            }
            return mDevice != nullptr;
        }

        bool mAttempted = false;
        bool mInitialized = false;
        HMODULE mNvml = nullptr;
        Shutdown mShutdown = nullptr;
        DeviceGetMemoryInfo mMemoryInfo = nullptr;
        DeviceHandle mDevice = nullptr;
        std::uint64_t mSelectedTotalBytes = 0;
    };
#else
    class DxgiSampler
    {
    public:
        bool query(Sample&) { return false; }
    };

    class NvmlSampler
    {
    public:
        bool query(AdapterSample&) { return false; }
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

    inline bool softPressure()
    {
        const PressureState state = pressureState();
        return state == PressureState::Soft || state == PressureState::Hard;
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
        const bool dxgiAvailable = sampler.query(sample) && sample.mBudgetBytes != 0;
        static NvmlSampler nvmlSampler;
        AdapterSample adapterSample;
        const bool nvmlAvailable = nvmlSampler.query(adapterSample);

        constexpr std::uint64_t MiB = std::uint64_t{ 1024 } * std::uint64_t{ 1024 };
        const std::uint64_t configuredSoftBytes
            = static_cast<std::uint64_t>(std::max(configuredSoftBudgetMb, 1)) * MiB;
        const std::uint64_t configuredHardBytes
            = static_cast<std::uint64_t>(std::max(configuredHardBudgetMb, configuredSoftBudgetMb + 128)) * MiB;

        std::uint64_t softBytes = configuredSoftBytes;
        std::uint64_t hardBytes = configuredHardBytes;
        PressureState state = PressureState::Unavailable;
        if (dxgiAvailable)
        {
            // Respect both the user's configured 8-GB-class limits and WDDM's actual
            // process budget. This preserves headroom when Windows temporarily grants
            // less than the adapter's physical capacity.
            softBytes = std::min(configuredSoftBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.90));
            hardBytes = std::min(configuredHardBytes, static_cast<std::uint64_t>(sample.mBudgetBytes * 0.97));
            if (hardBytes <= softBytes)
                hardBytes
                    = std::min<std::uint64_t>(sample.mBudgetBytes, softBytes + std::uint64_t{ 128 } * MiB);

            state = PressureState::Comfortable;
            if (sample.mUsageBytes >= hardBytes)
                state = PressureState::Hard;
            else if (sample.mUsageBytes >= softBytes)
                state = PressureState::Soft;
        }

        if (nvmlAvailable && adapterSample.mTotalBytes != 0)
        {
            // Adapter-wide guard for shared pressure that DXGI's process-local
            // accounting cannot see. Because V3.7 uses this only for admission
            // control (never destructive eviction), it is intentionally early.
            const double adapterUsedRatio = static_cast<double>(adapterSample.mUsedBytes)
                / static_cast<double>(adapterSample.mTotalBytes);
            constexpr std::uint64_t AdapterSoftFreeBytes = std::uint64_t{ 900 } * MiB;
            constexpr std::uint64_t AdapterHardFreeBytes = std::uint64_t{ 450 } * MiB;

            PressureState adapterState = PressureState::Comfortable;
            if (adapterUsedRatio >= 0.94 || adapterSample.mFreeBytes <= AdapterHardFreeBytes)
                adapterState = PressureState::Hard;
            else if (adapterUsedRatio >= 0.88 || adapterSample.mFreeBytes <= AdapterSoftFreeBytes)
                adapterState = PressureState::Soft;

            if (state == PressureState::Unavailable || static_cast<int>(adapterState) > static_cast<int>(state))
                state = adapterState;
        }

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
        const double adapterUsedMb = static_cast<double>(adapterSample.mUsedBytes) / static_cast<double>(MiB);
        const double adapterFreeMb = static_cast<double>(adapterSample.mFreeBytes) / static_cast<double>(MiB);
        const double adapterTotalMb = static_cast<double>(adapterSample.mTotalBytes) / static_cast<double>(MiB);

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ',' << std::fixed
            << std::setprecision(1) << usageMb << ',' << budgetMb << ',' << availableMb << ',' << reservationMb << ','
            << pressurePercent << ',' << (static_cast<double>(softBytes) / static_cast<double>(MiB)) << ','
            << (static_cast<double>(hardBytes) / static_cast<double>(MiB)) << ','
            << V3Diagnostics::csvQuote(pressureName(state)) << ',' << (nvmlAvailable ? 1 : 0) << ','
            << adapterUsedMb << ',' << adapterFreeMb << ',' << adapterTotalMb;
        writer.writeLine(row.str());
    }
}

#endif
