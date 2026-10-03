#ifndef OPENMW_COMPONENTS_RESOURCE_RAMRETENTION_H
#define OPENMW_COMPONENTS_RESOURCE_RAMRETENTION_H

#include "openglpressure.hpp"

#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace Resource
{
    inline bool ramRetentionEnabled(unsigned candidate) noexcept
    {
        const char* name = candidate == 1 ? "OPENMW_RAM_C1_SELECTIVE"
            : candidate == 2 ? "OPENMW_RAM_C2_PARSED_NIF"
            : candidate == 3 ? "OPENMW_RAM_C3_IDLE_COLLISION"
            : candidate == 4 ? "OPENMW_RAM_C4_EARLY_RECLAIM"
            : candidate == 5 ? "OPENMW_RAM_C5_COLD_IMAGES" : nullptr;
        const char* value = name ? std::getenv(name) : nullptr;
        return value && std::string_view(value) == "1";
    }
    // Retention only: this does not change P1A thresholds, byte admission, or
    // required-work fallback. Consumers use the existing coherent cached sample.
    class RamHeadroomPolicy
    {
    public:
        struct Decision
        {
            bool valid = false;
            bool contract = false;
            bool compressionAllowed = false;
            std::uint64_t generation = 0;
        };
        Decision update(const OpenGlPressureSample& sample) noexcept
        {
            const auto& memory = sample.memory;
            const auto& limits = sample.decision.limits;
            const bool valid = sample.generation && memory.physicalValid && memory.physicalTotal
                && memory.physicalAvailable <= memory.physicalTotal
                && (memory.commitValid || memory.systemCommitValid)
                && limits.physicalReserve && limits.commitReserve;
            if (!valid || sample.decision.state == OpenGlPressure::Degraded)
            {
                mHealthySince = 0;
                return {};
            }
            // Caution/Critical remain authoritative. Recovering does not admit
            // new allocations; early contraction also stays active through it.
            const bool shrinking = sample.decision.state != OpenGlPressure::Normal
                || memory.physicalAvailable < limits.physicalRecovery
                || (memory.commitValid && memory.commitAvailable < limits.commitRecovery)
                || (memory.systemCommitValid && memory.systemCommitAvailable < limits.commitRecovery)
                || (memory.lowMemoryValid && memory.lowMemory);
            if (shrinking)
            {
                mContract = true;
                mHealthySince = 0;
            }
            else if (mContract && sample.generation != mLastGeneration)
            {
                if (!mHealthySince || sample.sampledAtMs < mHealthySince)
                    mHealthySince = sample.sampledAtMs;
                else if (sample.sampledAtMs - mHealthySince >= OpenGlPressureConfig::RecoveryMilliseconds)
                {
                    mContract = false;
                    mHealthySince = 0;
                }
            }
            mLastGeneration = sample.generation;
            return { true, mContract,
                !mContract && sample.decision.state == OpenGlPressure::Normal
                    && sample.decision.admissionsPerSample != 0, sample.generation };
        }
    private:
        bool mContract = false;
        std::uint64_t mLastGeneration = 0, mHealthySince = 0;
    };

    // Main-thread owner policy, indexed by actual accepted request time. The
    // bounded protected sets contain identities only, never resource graphs.
    template<class Key>
    class ColdOwnerRetention
    {
    public:
        void request(Key key, double time, bool nearFuture) noexcept
        {
            if (mDisabled || !std::isfinite(time)) return;
            mTime = (std::max)(mTime, time);
            try
            {
                erase(key);
                auto position = mRequests.emplace(time, key);
                try { mEntries.emplace(key, position); }
                catch (...) { mRequests.erase(position); throw; }
            }
            catch (...) { clear(); mDisabled = true; return; }
            if (nearFuture) near(key, time);
        }
        void near(Key key, double time)
        {
            if (!std::isfinite(time)) return;
            mTime = (std::max)(mTime, time);
            protect(mNear, key, time);
        }
        void demanded(Key key) { protect(mReturns, key, mTime); }
        void erase(Key key)
        {
            const auto found = mEntries.find(key);
            if (found == mEntries.end()) return;
            if (mNext == found->second) ++mNext;
            mRequests.erase(found->second); mEntries.erase(found);
        }
        void clear()
        {
            mEntries.clear(); mRequests.clear(); mNext = mRequests.end();
            mNear = {}; mReturns = {};
        }
        template<class Visitor>
        std::size_t retire(double time, double ttl, Visitor&& visitor,
            std::optional<Key> current = {}, std::optional<Key> ticket = {},
            std::size_t scanLimit = 32, std::size_t retireLimit = 4)
        {
            if (mDisabled || !std::isfinite(time) || !std::isfinite(ttl) || ttl < 0) return 0;
            mTime = (std::max)(mTime, time);
            std::size_t retired = 0;
            if (mNext == mRequests.end()) mNext = mRequests.begin();
            for (std::size_t scanned = 0; mNext != mRequests.end()
                && scanned < scanLimit && retired < retireLimit; ++scanned)
            {
                auto candidate = mNext++;
                if (candidate->first > time - ttl) break; // ordered, later keys are newer
                const auto& key = candidate->second;
                if ((current && key == *current) || (ticket && key == *ticket)
                    || protectedBy(mNear, key, time, 5.0) || protectedBy(mReturns, key, time, 60.0))
                    continue;
                if (visitor(key)) ++retired;
            }
            return retired;
        }
        std::size_t size() const noexcept { return mEntries.size(); }
        bool available() const noexcept { return !mDisabled; }
    private:
        struct Protection { std::optional<Key> key; double time = 0; };
        template<std::size_t N>
        static void protect(std::array<Protection, N>& set, Key key, double time)
        {
            auto* victim = &set[0];
            for (auto& entry : set)
            {
                if (entry.key && *entry.key == key) { entry.time = time; return; }
                if (!entry.key || entry.time < victim->time) victim = &entry;
            }
            *victim = {key, time};
        }
        template<std::size_t N>
        static bool protectedBy(const std::array<Protection, N>& set, Key key, double time, double ttl)
        {
            for (const auto& entry : set)
                if (entry.key && *entry.key == key && entry.time > time - ttl) return true;
            return false;
        }
        std::multimap<double, Key> mRequests;
        std::map<Key, typename std::multimap<double, Key>::iterator, std::less<Key>> mEntries;
        typename std::multimap<double, Key>::iterator mNext = mRequests.end();
        std::array<Protection, 32> mNear{};
        std::array<Protection, 8> mReturns{};
        double mTime = 0;
        bool mDisabled = false; // indexing failure leaves original retention
    };
}
#endif
