#ifndef OPENMW_COMPONENTS_RESOURCE_SPECULATIVEBUDGET_H
#define OPENMW_COMPONENTS_RESOURCE_SPECULATIVEBUDGET_H

#include "openglpressure.hpp"

#include <atomic>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace Resource
{
    // P2 strong active-grid upgrades are optional for correctness but more
    // valuable than ordinary distant/background speculation. P1 reserves a
    // bounded lane for them and may admit them during caution/recovery while
    // still denying them under critical or unknown pressure.
    enum class SpeculativePriority : unsigned char { Background, NearFuture };

    // Optional preparation may be deferred, but MUST NOT be turned into a
    // missing-resource/error-marker cache entry by a loader's ordinary catch.
    struct SpeculativeDeferred : std::exception
    {
        const char* what() const noexcept override { return "optional resource preparation deferred"; }
    };

    class SpeculativeBudget
    {
    public:
        static constexpr std::uint64_t MiB = 1024ull * 1024;
        struct Config
        {
            std::uint64_t transientLimit = 1024 * MiB;
            unsigned maximumJobs = 4;
            unsigned reservedNearFutureJobs = 0;
        };
        struct Stats
        {
            std::uint64_t futureBytes = 0, knownOwnerBytes = 0, estimatedOwnerBytes = 0;
            std::uint64_t pendingOwnerBytes = 0, unknownOwners = 0, jobs = 0, priorityJobs = 0;
            std::uint64_t admitted = 0, denied = 0, nearFutureAdmitted = 0, nearFutureDenied = 0;
            std::uint64_t duplicateRequests = 0, sharedClaims = 0;
            std::uint64_t demandHits = 0, prefetchHits = 0, observedGrowth = 0;
        };
        struct Key
        {
            const void* identity = nullptr;
            unsigned kind = 0;
            bool operator<(const Key& other) const noexcept
            {
                if (kind != other.kind) return kind < other.kind;
                return std::less<const void*>{}(identity, other.identity);
            }
        };
    private:
        struct State;
    public:
        class Charge
        {
        public:
            Charge(const Charge&) = delete;
            Charge& operator=(const Charge&) = delete;
            ~Charge();
            std::uint64_t bytes() const noexcept { return mBytes; }
            // Caller detaches the last TRACKED owner first. Other osg owners
            // may remain: these counters never claim process/VRAM bytes freed.
            void pending();
        private:
            friend class SpeculativeBudget;
            Charge(std::shared_ptr<State> state, Key key, std::uint64_t bytes, bool known)
                : mState(std::move(state)), mKey(key), mBytes(bytes), mKnown(known) {}
            std::shared_ptr<State> mState;
            Key mKey;
            std::uint64_t mBytes;
            bool mKnown, mPending = false, mRegistered = false;
        };
        using ChargePtr = std::shared_ptr<Charge>;
        class Job
        {
        public:
            Job() = default;
            Job(const Job&) = delete;
            Job& operator=(const Job&) = delete;
            Job(Job&& other) noexcept
                : mState(std::move(other.mState)), mNearFuture(std::exchange(other.mNearFuture, false)) {}
            Job& operator=(Job&& other) noexcept
            {
                if (this != &other)
                {
                    finish();
                    mState = std::move(other.mState);
                    mNearFuture = std::exchange(other.mNearFuture, false);
                }
                return *this;
            }
            ~Job() { finish(); }
            explicit operator bool() const noexcept { return mState != nullptr; }
            void finish();
        private:
            friend class SpeculativeBudget;
            Job(std::shared_ptr<State> state, bool nearFuture)
                : mState(std::move(state)), mNearFuture(nearFuture) {}
            std::shared_ptr<State> mState;
            bool mNearFuture = false;
        };
        class Stage
        {
        public:
            Stage() = default;
            Stage(const Stage&) = delete;
            Stage& operator=(const Stage&) = delete;
            Stage(Stage&& other) noexcept : mState(std::move(other.mState)), mFuture(std::exchange(other.mFuture, 0)) {}
            Stage& operator=(Stage&&) = delete;
            ~Stage();
            std::uint64_t future() const noexcept { return mFuture; }
        private:
            friend class SpeculativeBudget;
            Stage(std::shared_ptr<State> state, std::uint64_t bytes) : mState(std::move(state)), mFuture(bytes) {}
            std::shared_ptr<State> mState;
            std::uint64_t mFuture = 0;
        };
        class Claim
        {
        public:
            Claim() = default;
            Claim(const Claim&) = delete;
            Claim& operator=(const Claim&) = delete;
            Claim(Claim&& other) noexcept : mState(std::move(other.mState)), mKey(std::move(other.mKey)) {}
            ~Claim();
        private:
            friend class SpeculativeBudget;
            Claim(std::shared_ptr<State> state, std::string key) : mState(std::move(state)), mKey(std::move(key)) {}
            std::shared_ptr<State> mState;
            std::string mKey;
        };
        SpeculativeBudget() : SpeculativeBudget(Config{}) {}
        explicit SpeculativeBudget(Config config);
        SpeculativeBudget(const SpeculativeBudget&) = delete;
        SpeculativeBudget& operator=(const SpeculativeBudget&) = delete;

        // Sample watermark is read BEFORE the OS query starts. Growth after
        // that watermark remains charged even if its owners have been released.
        const std::atomic<std::uint64_t>* watermark() const noexcept;
        Job tryJob(const OpenGlPressureSample& sample,
            SpeculativePriority priority = SpeculativePriority::Background);
        Stage reserve(const OpenGlPressureSample& sample, std::uint64_t bytes,
            SpeculativePriority priority = SpeculativePriority::Background);
        Claim claim(std::string key);
        ChargePtr track(Key key, std::uint64_t bytes, bool known, Stage* stage = nullptr);
        void hit(bool speculative);
        Stats stats() const;
    private:
        struct State
        {
            explicit State(Config c) : config(c) {}
            Config config;
            std::mutex mutex;
            Stats stats;
            std::map<Key, std::weak_ptr<Charge>> payloads;
            std::set<std::string> requests;
            std::atomic<std::uint64_t> growth{0}, demandHits{0}, prefetchHits{0};
            bool saturated = false;
            std::uint64_t generation = 0, generationJobs = 0, generationNearFutureJobs = 0;
        };
        static bool usable(const OpenGlPressureSample& s, SpeculativePriority priority) noexcept;
        static std::uint64_t add(std::uint64_t a, std::uint64_t b) noexcept
        { return b > UINT64_MAX - a ? UINT64_MAX : a + b; }
        std::shared_ptr<State> mState;
    };

    inline SpeculativeBudget::SpeculativeBudget(Config config) : mState(std::make_shared<State>(config)) {}
    inline const std::atomic<std::uint64_t>* SpeculativeBudget::watermark() const noexcept { return &mState->growth; }
    inline bool SpeculativeBudget::usable(
        const OpenGlPressureSample& s, SpeculativePriority priority) noexcept
    {
        const bool valid = s.generation && s.memory.physicalValid && s.memory.physicalTotal
            && s.memory.physicalAvailable <= s.memory.physicalTotal
            && (s.memory.commitValid || s.memory.systemCommitValid)
            && !(s.memory.lowMemoryValid && s.memory.lowMemory);
        if (!valid)
            return false;
        if (priority == SpeculativePriority::Background)
            return s.decision.state == OpenGlPressure::Normal && s.decision.admissionsPerSample;
        return s.decision.state == OpenGlPressure::Normal
            || s.decision.state == OpenGlPressure::Caution
            || s.decision.state == OpenGlPressure::Recovering;
    }

    inline SpeculativeBudget::Job SpeculativeBudget::tryJob(
        const OpenGlPressureSample& sample, SpeculativePriority priority)
    {
        std::lock_guard lock(mState->mutex);
        const bool nearFuture = priority == SpeculativePriority::NearFuture;
        if (sample.generation > mState->generation)
        {
            mState->generation = sample.generation;
            mState->generationJobs = 0;
            mState->generationNearFutureJobs = 0;
        }

        const unsigned reserved = (std::min)(
            mState->config.reservedNearFutureJobs, mState->config.maximumJobs);
        const unsigned backgroundJobs = static_cast<unsigned>(
            mState->stats.jobs >= mState->stats.priorityJobs
                ? mState->stats.jobs - mState->stats.priorityJobs : 0);
        const unsigned backgroundLimit = mState->config.maximumJobs - reserved;
        const bool prioritySampleLimited = sample.decision.state != OpenGlPressure::Normal
            || sample.decision.admissionsPerSample == 1;

        const bool deny = !usable(sample, priority) || sample.generation != mState->generation
            || mState->saturated || mState->stats.jobs >= mState->config.maximumJobs
            || (nearFuture && reserved && mState->stats.priorityJobs >= reserved)
            || (!nearFuture && reserved && backgroundJobs >= backgroundLimit)
            || (nearFuture && prioritySampleLimited && mState->generationNearFutureJobs >= 1)
            || (!nearFuture && sample.decision.admissionsPerSample == 1 && mState->generationJobs >= 1);
        if (deny)
        {
            ++mState->stats.denied;
            if (nearFuture)
                ++mState->stats.nearFutureDenied;
            return {};
        }

        ++mState->generationJobs;
        if (nearFuture)
            ++mState->generationNearFutureJobs;
        ++mState->stats.jobs;
        if (nearFuture)
            ++mState->stats.priorityJobs;
        ++mState->stats.admitted;
        if (nearFuture)
            ++mState->stats.nearFutureAdmitted;
        return Job(mState, nearFuture);
    }

    inline void SpeculativeBudget::Job::finish()
    {
        if (!mState)
            return;
        auto state = std::move(mState);
        std::lock_guard lock(state->mutex);
        --state->stats.jobs;
        if (mNearFuture)
            --state->stats.priorityJobs;
        mNearFuture = false;
    }

    inline SpeculativeBudget::Stage SpeculativeBudget::reserve(
        const OpenGlPressureSample& sample, std::uint64_t bytes, SpeculativePriority priority)
    {
        std::lock_guard lock(mState->mutex);
        const bool nearFuture = priority == SpeculativePriority::NearFuture;
        const auto growth = mState->growth.load(std::memory_order_relaxed);
        const auto future = add(mState->stats.futureBytes, bytes);
        const auto extra = add(
            future, growth >= sample.accountedGrowth ? growth - sample.accountedGrowth : UINT64_MAX);
        const auto fits = [extra](std::uint64_t available, std::uint64_t floor) {
            return available >= floor && extra <= available - floor;
        };

        auto physicalFloor = sample.decision.limits.physicalReserve;
        auto commitFloor = sample.decision.limits.commitReserve;
        if (nearFuture && sample.decision.state == OpenGlPressure::Caution)
        {
            physicalFloor = sample.decision.limits.physicalCritical;
            commitFloor = sample.decision.limits.commitCritical;
        }

        if (!usable(sample, priority) || !sample.growthWatermarkValid || mState->saturated
            || bytes == UINT64_MAX || future > mState->config.transientLimit
            || !fits(sample.memory.physicalAvailable, physicalFloor)
            || (sample.memory.commitValid && !fits(sample.memory.commitAvailable, commitFloor))
            || (sample.memory.systemCommitValid
                && !fits(sample.memory.systemCommitAvailable, commitFloor)))
        {
            ++mState->stats.denied;
            if (nearFuture)
                ++mState->stats.nearFutureDenied;
            throw SpeculativeDeferred{};
        }
        mState->stats.futureBytes = future;
        return Stage(mState, bytes);
    }
    inline SpeculativeBudget::Stage::~Stage()
    {
        if (!mState) return;
        std::lock_guard lock(mState->mutex);
        mState->stats.futureBytes -= mFuture;
    }
    inline SpeculativeBudget::Claim SpeculativeBudget::claim(std::string key)
    {
        std::lock_guard lock(mState->mutex);
        // Only optional immutable requests participate. Demand never waits.
        if (mState->requests.size() >= 4096 || !mState->requests.insert(key).second)
        { ++mState->stats.duplicateRequests; throw SpeculativeDeferred{}; }
        return Claim(mState, std::move(key));
    }
    inline SpeculativeBudget::Claim::~Claim()
    {
        if (!mState) return;
        std::lock_guard lock(mState->mutex);
        mState->requests.erase(mKey);
    }
    inline SpeculativeBudget::ChargePtr SpeculativeBudget::track(Key key, std::uint64_t bytes, bool known, Stage* stage)
    {
        if (!key.identity) return {};
        auto result = ChargePtr(new Charge(mState, key, bytes, known));
        std::lock_guard lock(mState->mutex);
        auto [it, inserted] = mState->payloads.try_emplace(key);
        (void)inserted;
        if (auto existing = it->second.lock())
        { ++mState->stats.sharedClaims; return existing; }
        // Allocate before changing totals for exception safety. Deleter may
        // reenter the budget: do not run it while holding this mutex.
        auto& checkedTotal = known ? mState->stats.knownOwnerBytes : mState->stats.estimatedOwnerBytes;
        if (bytes > UINT64_MAX - checkedTotal)
        { if (inserted) mState->payloads.erase(it); throw std::overflow_error("speculative owner accounting overflow"); }
        it->second = result;
        result->mRegistered = true;
        auto& total = known ? mState->stats.knownOwnerBytes : mState->stats.estimatedOwnerBytes;
        total = add(total, bytes);
        if (!bytes) ++mState->stats.unknownOwners;
        const auto previous = mState->growth.load(std::memory_order_relaxed);
        const auto next = add(previous, bytes);
        if (next == UINT64_MAX) mState->saturated = true;
        mState->growth.store(next, std::memory_order_release);
        if (stage && stage->mState == mState)
        {
            const auto transfer = (std::min)(bytes, stage->mFuture);
            stage->mFuture -= transfer;
            mState->stats.futureBytes -= transfer;
        }
        return result;
    }
    inline SpeculativeBudget::Charge::~Charge()
    {
        if (!mRegistered) return;
        std::lock_guard lock(mState->mutex);
        (mKnown ? mState->stats.knownOwnerBytes : mState->stats.estimatedOwnerBytes) -= mBytes;
        if (!mBytes) --mState->stats.unknownOwners;
        if (mPending) mState->stats.pendingOwnerBytes -= mBytes;
        // An older charge can finish concurrently with a new same-key owner.
        auto it = mState->payloads.find(mKey);
        if (it != mState->payloads.end() && it->second.expired()) mState->payloads.erase(it);
        // No subtraction from growth: release is not immediate OS free credit.
    }
    inline void SpeculativeBudget::Charge::pending()
    {
        std::lock_guard lock(mState->mutex);
        if (!mPending) { mPending = true; mState->stats.pendingOwnerBytes = add(mState->stats.pendingOwnerBytes, mBytes); }
    }
    inline void SpeculativeBudget::hit(bool speculative)
    {
        (speculative ? mState->prefetchHits : mState->demandHits).fetch_add(1, std::memory_order_relaxed);
    }
    inline SpeculativeBudget::Stats SpeculativeBudget::stats() const
    {
        std::lock_guard lock(mState->mutex);
        auto result = mState->stats;
        result.demandHits = mState->demandHits.load(std::memory_order_relaxed);
        result.prefetchHits = mState->prefetchHits.load(std::memory_order_relaxed);
        result.observedGrowth = mState->growth.load(std::memory_order_relaxed);
        return result;
    }

    // Thread-local context belongs to an optional WORKER operation, not to an
    // osg node. No graph walks/allocations on cache hits or frame publication.
    class SpeculativeScope
    {
    public:
        using Sample = OpenGlPressureSample (*)(void*);
        struct Context
        {
            SpeculativeBudget* budget = nullptr;
            Sample sample = nullptr;
            void* owner = nullptr;
            SpeculativePriority priority = SpeculativePriority::Background;
        };

        SpeculativeScope(SpeculativeBudget* budget, Sample sample, void* owner,
            SpeculativePriority priority = SpeculativePriority::Background)
            : SpeculativeScope(Context{ budget, sample, owner, priority })
        {
        }
        explicit SpeculativeScope(Context context)
            : mPrevious(sCurrent)
            , mBudget(context.budget)
            , mSample(context.sample)
            , mOwner(context.owner)
            , mPriority(context.priority)
        {
            if (mBudget)
                sCurrent = this;
        }
        ~SpeculativeScope() { if (mBudget) sCurrent = mPrevious; }
        SpeculativeScope(const SpeculativeScope&) = delete;
        SpeculativeScope& operator=(const SpeculativeScope&) = delete;
        static bool active() noexcept { return sCurrent != nullptr; }
        static Context capture() noexcept
        {
            return sCurrent
                ? Context{ sCurrent->mBudget, sCurrent->mSample, sCurrent->mOwner, sCurrent->mPriority }
                : Context{};
        }
        static void creditRetainedEstimate(std::uint64_t bytes) noexcept
        {
            if (!sCurrent || !bytes)
                return;
            sCurrent->mRetainedEstimate = bytes > UINT64_MAX - sCurrent->mRetainedEstimate
                ? UINT64_MAX : sCurrent->mRetainedEstimate + bytes;
        }
        std::uint64_t retainedEstimate() const noexcept { return mRetainedEstimate; }
        class Stage
        {
        public:
            Stage(std::uint64_t peak, std::uint64_t outputEstimate)
                : mScope(sCurrent), mPrevious(mScope ? mScope->mStage : nullptr), mEstimate(outputEstimate)
                , mLease(mScope ? mScope->mBudget->reserve(
                    mScope->mSample(mScope->mOwner), peak, mScope->mPriority) : SpeculativeBudget::Stage{})
            { if (mScope) mScope->mStage = this; }
            ~Stage() { if (mScope) mScope->mStage = mPrevious; }
            Stage(const Stage&) = delete;
            Stage& operator=(const Stage&) = delete;
        private:
            friend class SpeculativeScope;
            SpeculativeScope* mScope;
            Stage* mPrevious;
            std::uint64_t mEstimate;
            SpeculativeBudget::Stage mLease;
        };
        static SpeculativeBudget::Claim claim(std::string key)
        { return sCurrent ? sCurrent->mBudget->claim(std::move(key)) : SpeculativeBudget::Claim{}; }
        static SpeculativeBudget::ChargePtr track(SpeculativeBudget* budget, SpeculativeBudget::Key key,
            std::uint64_t bytes, bool known)
        {
            if (!budget) return {};
            auto* stage = sCurrent && sCurrent->mBudget == budget ? sCurrent->mStage : nullptr;
            if (!known && !bytes && stage) bytes = stage->mEstimate;
            auto result = budget->track(key, bytes, known, stage ? &stage->mLease : nullptr);
            if (result && sCurrent && sCurrent->mBudget == budget)
                sCurrent->mRetainedEstimate = result->bytes() > UINT64_MAX - sCurrent->mRetainedEstimate
                    ? UINT64_MAX : sCurrent->mRetainedEstimate + result->bytes();
            return result;
        }
    private:
        inline static thread_local SpeculativeScope* sCurrent = nullptr;
        SpeculativeScope* mPrevious;
        SpeculativeBudget* mBudget;
        Sample mSample;
        void* mOwner;
        SpeculativePriority mPriority = SpeculativePriority::Background;
        Stage* mStage = nullptr;
        std::uint64_t mRetainedEstimate = 0;
    };
}
#endif
