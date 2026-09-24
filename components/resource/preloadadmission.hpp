#ifndef OPENMW_COMPONENTS_RESOURCE_PRELOADADMISSION_H
#define OPENMW_COMPONENTS_RESOURCE_PRELOADADMISSION_H

#include <components/misc/hostmemory.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace Resource
{
    // Admission estimates, NOT allocated bytes or a cap on required loading.
    // Bound queued + running speculative jobs before they can consume memory.
    // A single mod asset can exceed the allowance; between-asset pressure checks
    // remain necessary. Completed owners still use the normal retention policy.
    class PreloadAdmission
    {
    public:
        static constexpr std::uint64_t Allowance = 256ull * 1024 * 1024;
        static constexpr std::uint64_t MaximumJobs = 4;
        struct Stats
        {
            std::uint64_t pending = 0, reservedEstimate = 0, admitted = 0, denied = 0, released = 0;
        };
    private:
        struct State { std::mutex mutex; Stats stats; };
    public:
        class Reservation
        {
        public:
            Reservation() = default; // disabled control, no reservation
            Reservation(const Reservation&) = delete;
            Reservation& operator=(const Reservation&) = delete;
            Reservation(Reservation&& other) noexcept : mState(std::move(other.mState)) {}
            Reservation& operator=(Reservation&& other) noexcept
            {
                if (this != &other) { release(); mState = std::move(other.mState); }
                return *this;
            }
            ~Reservation() { release(); }
        private:
            friend class PreloadAdmission;
            explicit Reservation(std::shared_ptr<State> state) : mState(std::move(state)) {}
            void release() noexcept
            {
                if (!mState) return;
                const auto state = std::move(mState);
                std::lock_guard lock(state->mutex);
                --state->stats.pending;
                state->stats.reservedEstimate -= Allowance;
                ++state->stats.released;
            }
            std::shared_ptr<State> mState;
        };

        std::optional<Reservation> reserve(const Misc::HostMemoryStatus& memory,
            bool pressured, std::uint64_t physicalReserve, std::uint64_t privateSoft)
        {
            std::lock_guard lock(mState->mutex);
            auto& stats = mState->stats;
            const std::uint64_t projected = stats.reservedEstimate + Allowance;
            const bool validPhysical = memory.physicalValid && memory.physicalTotal != 0
                && memory.physicalAvailable <= memory.physicalTotal;
            // Missing OS counters are not zero bytes. Still apply the job limit.
            const bool fits = (!validPhysical || (memory.physicalAvailable >= physicalReserve
                && projected <= memory.physicalAvailable - physicalReserve))
                && (!memory.processValid || privateSoft == 0 || (memory.privateCommit < privateSoft
                    && projected <= privateSoft - memory.privateCommit))
                && (!memory.commitValid || (memory.commitAvailable >= physicalReserve
                    && projected <= memory.commitAvailable - physicalReserve));
            if (pressured || stats.pending >= MaximumJobs || !fits)
            {
                ++stats.denied;
                return std::nullopt;
            }
            ++stats.pending; ++stats.admitted; stats.reservedEstimate = projected;
            return Reservation(mState);
        }
        Stats stats() const
        {
            std::lock_guard lock(mState->mutex);
            return mState->stats;
        }
    private:
        std::shared_ptr<State> mState = std::make_shared<State>();
    };
}
#endif
