#ifndef OPENMW_COMPONENTS_FILES_CONSTRAINEDREADPOLICY_H
#define OPENMW_COMPONENTS_FILES_CONSTRAINEDREADPOLICY_H

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace Files::ConstrainedReadPolicy
{
    inline constexpr std::size_t ControlBytes = 8192;
    inline constexpr std::size_t CandidateBytes = 65536;
    inline constexpr std::size_t MaximumBudgetBytes = 32 * 1024 * 1024;
    // Charged in addition to the complete allocation requests, not instead of
    // the inline control buffer. These are conservative owned-overhead bounds.
    inline constexpr std::size_t StateOverheadBytes = 128;
    inline constexpr std::size_t BufferOverheadBytes = 64;

    struct Options
    {
        std::size_t refillBytes = ControlBytes;
        std::size_t budgetBytes = MaximumBudgetBytes;
        bool residentSeek = false;

        static const Options& startup()
        {
            // The launcher sets these before process creation. No environment
            // reads or budget atomics occur on the refill/get hot path.
            static const Options value = [] {
                Options result;
                if (const char* text = std::getenv("OPENMW_P9_FILE_REFILL_BYTES"))
                    if (std::string_view(text) == "65536")
                        result.refillBytes = CandidateBytes;
                if (const char* text = std::getenv("OPENMW_P9_FILE_RESIDENT_SEEK"))
                    result.residentSeek = std::string_view(text) == "1";
                if (const char* text = std::getenv("OPENMW_P9_FILE_BUFFER_BUDGET_BYTES"))
                {
                    const std::string_view input(text);
                    std::size_t parsed = 0;
                    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), parsed);
                    if (error == std::errc{} && end == input.data() + input.size()
                        && parsed <= MaximumBudgetBytes)
                        result.budgetBytes = parsed;
                }
                return result;
            }();
            return value;
        }
    };

    struct Snapshot
    {
        std::size_t limit = 0;
        std::size_t used = 0;
        std::size_t peak = 0;
        std::size_t states = 0;
        std::size_t buffers = 0;
    };

    class Budget
    {
    public:
        static Budget& instance()
        {
            static Budget value(Options::startup().budgetBytes);
            return value;
        }

        bool acquire(std::size_t bytes, bool buffer) noexcept
        {
            auto used = mUsed.load(std::memory_order_relaxed);
            do
            {
                if (bytes > mLimit || used > mLimit - bytes)
                    return false;
            } while (!mUsed.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
            auto peak = mPeak.load(std::memory_order_relaxed);
            while (peak < used + bytes
                && !mPeak.compare_exchange_weak(peak, used + bytes, std::memory_order_relaxed))
            {
            }
            (buffer ? mBuffers : mStates).fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        void release(std::size_t bytes, bool buffer) noexcept
        {
            (buffer ? mBuffers : mStates).fetch_sub(1, std::memory_order_relaxed);
            mUsed.fetch_sub(bytes, std::memory_order_relaxed);
        }

        Snapshot snapshot() const noexcept
        {
            // Fields are independent concurrent snapshots, not a transaction.
            return { mLimit, mUsed.load(std::memory_order_relaxed), mPeak.load(std::memory_order_relaxed),
                mStates.load(std::memory_order_relaxed), mBuffers.load(std::memory_order_relaxed) };
        }

    private:
        explicit Budget(std::size_t limit) : mLimit(limit) {}
        const std::size_t mLimit;
        std::atomic<std::size_t> mUsed{ 0 }, mPeak{ 0 }, mStates{ 0 }, mBuffers{ 0 };
    };

    class Reservation
    {
    public:
        Reservation() = default;
        Reservation(std::size_t bytes, bool buffer)
        {
            auto& budget = Budget::instance();
            if (budget.acquire(bytes, buffer))
            {
                mBudget = &budget;
                mBytes = bytes;
                mBuffer = buffer;
            }
        }
        ~Reservation() { reset(); }
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept
            : mBudget(std::exchange(other.mBudget, nullptr)), mBytes(other.mBytes), mBuffer(other.mBuffer)
        {
        }
        Reservation& operator=(Reservation&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                mBudget = std::exchange(other.mBudget, nullptr);
                mBytes = other.mBytes;
                mBuffer = other.mBuffer;
            }
            return *this;
        }
        explicit operator bool() const noexcept { return mBudget != nullptr; }
        std::size_t bytes() const noexcept { return mBudget ? mBytes : 0; }
        void reset() noexcept
        {
            if (mBudget)
            {
                mBudget->release(mBytes, mBuffer);
                mBudget = nullptr;
            }
        }

    private:
        Budget* mBudget = nullptr;
        std::size_t mBytes = 0;
        bool mBuffer = false;
    };

#ifdef P9_FILE_STREAM_TEST_HOOKS
    // Compiled only into the dedicated native fixture. Production has neither
    // these globals nor a branch/virtual dispatch for fault injection.
    namespace TestHooks
    {
        inline std::atomic<unsigned> allocationFailures{ 0 }, readFailures{ 0 }, seekFailures{ 0 }, zeroReads{ 0 };
        inline std::atomic<unsigned> tellFailures{ 0 }, readFailuresAfterAdvance{ 0 }, seekFailuresAfterAdvance{ 0 };
        inline std::atomic<std::size_t> nextReadLimit{ 0 };
        inline bool take(std::atomic<unsigned>& counter)
        {
            auto value = counter.load();
            while (value && !counter.compare_exchange_weak(value, value - 1))
            {
            }
            return value != 0;
        }
    }
#endif
}

#endif
