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
        struct State
        {
            std::mutex mutex;
            Stats stats;
            std::uint64_t sampleGeneration = 0, sampleAdmissions = 0, sampleOpeningReservations = 0;
        };
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
        // GL-P1A's conservative interim guard. Credits are NOT refunded within
        // an OS sample when a fast job finishes but keeps its cached output.
        // Existing running reservations may already be in the OS counters: we
        // deliberately overestimate here. Precise stage/retained-byte transfer
        // remains GL-P1B; these allowances are not actual allocation accounting.
        std::optional<Reservation> reserveOpenGl(const Misc::HostMemoryStatus& memory,
            std::uint64_t generation, unsigned sampleLimit,
            std::uint64_t physicalReserve, std::uint64_t commitReserve)
        {
            std::lock_guard lock(mState->mutex);
            auto& stats = mState->stats;
            if (generation > mState->sampleGeneration)
            {
                mState->sampleGeneration = generation;
                mState->sampleAdmissions = 0;
                mState->sampleOpeningReservations = stats.reservedEstimate;
            }
            const auto fits = [](std::uint64_t available, std::uint64_t floor, std::uint64_t extra) {
                return available >= floor && extra <= available - floor;
            };
            const auto projected = mState->sampleOpeningReservations
                + (mState->sampleAdmissions + 1) * Allowance;
            const bool healthy = generation != 0 && generation == mState->sampleGeneration
                && sampleLimit != 0 && mState->sampleAdmissions < (std::min)(MaximumJobs,
                    static_cast<std::uint64_t>(sampleLimit))
                && memory.physicalValid && memory.physicalTotal != 0
                && memory.physicalAvailable <= memory.physicalTotal
                && (memory.commitValid || memory.systemCommitValid)
                && !(memory.lowMemoryValid && memory.lowMemory)
                && fits(memory.physicalAvailable, physicalReserve, projected)
                && (!memory.commitValid || fits(memory.commitAvailable, commitReserve, projected))
                && (!memory.systemCommitValid || fits(memory.systemCommitAvailable, commitReserve, projected));
            if (!healthy || stats.pending >= MaximumJobs)
            {
                ++stats.denied;
                return std::nullopt;
            }
            ++mState->sampleAdmissions;
            ++stats.pending;
            ++stats.admitted;
            stats.reservedEstimate += Allowance;
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
