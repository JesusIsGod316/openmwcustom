#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_GPUSCENETABLES_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_GPUSCENETABLES_H

#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

namespace RenderVsg
{
    // Phase 2 backend-owned persistent table identity. This deliberately stores
    // only stable slot/generation/revision metadata in the first slice: later
    // Vulkan storage-buffer rows can be packed/uploaded from the exact dirty
    // ranges without changing RenderCore ownership or rescanning RenderWorld.
    class GpuSceneTables final
    {
    public:
        enum class Kind : std::uint8_t
        {
            Mesh,
            Model,
            Material,
            Texture,
            Skeleton,
            Instance,
            Chunk,
            Light,
            Count,
        };

        struct Slot
        {
            std::uint32_t generation = 0;
            RenderCore::ResourceRevision revision;
            bool live = false;

            friend bool operator==(const Slot&, const Slot&) = default;
        };

        struct DirtyRange
        {
            std::uint32_t first = std::numeric_limits<std::uint32_t>::max();
            std::uint32_t last = 0;

            [[nodiscard]] bool dirty() const noexcept
            {
                return first != std::numeric_limits<std::uint32_t>::max();
            }
            [[nodiscard]] std::uint32_t count() const noexcept
            {
                return dirty() ? last - first : 0;
            }
            void include(std::uint32_t slot) noexcept
            {
                first = std::min(first, slot);
                last = std::max(last, slot + 1);
            }
            void clear() noexcept
            {
                first = std::numeric_limits<std::uint32_t>::max();
                last = 0;
            }
        };

        struct DeltaStats
        {
            std::size_t creates = 0;
            std::size_t updates = 0;
            std::size_t reparents = 0;
            std::size_t retires = 0;
        };

        void reset(RenderCore::WorldEpoch epoch, RenderCore::RenderWorldRevision revision) noexcept
        {
            for (auto& table : mTables)
                table.clear();
            clearDirty();
            mEpoch = epoch;
            mWorldRevision = revision;
            mLastSequence = {};
            mLightSerial = 1;
            mHealthy = epoch.valid() && revision.valid();
            mStats = {};
        }

        [[nodiscard]] bool apply(
            const RenderCore::RenderWorld& world, const RenderCore::RenderWorldUpdateBatch& batch) noexcept
        {
            if (!mHealthy || batch.epoch() != world.epoch() || batch.epoch() != mEpoch
                || !batch.sealed() || !batch.sequence().valid())
            {
                mHealthy = false;
                return false;
            }
            if (mLastSequence.valid())
            {
                const auto next = RenderCore::advanceMonotonic(mLastSequence);
                if (!next || batch.sequence() != *next)
                {
                    mHealthy = false;
                    return false;
                }
            }
            else if (batch.sequence() != RenderCore::InitialUpdateSequence)
            {
                mHealthy = false;
                return false;
            }

            bool valid = true;
            bool lightTouched = false;
            for (const RenderCore::RenderWorldUpdateOperation& operation : batch.operations())
            {
                std::visit([&](const auto& value) {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, RenderCore::CreateMesh>)
                    { ++mStats.creates; valid = valid && sync(Kind::Mesh, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateMesh>)
                    { ++mStats.updates; valid = valid && sync(Kind::Mesh, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireMesh>)
                    { ++mStats.retires; valid = valid && sync(Kind::Mesh, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateModel>)
                    { ++mStats.creates; valid = valid && sync(Kind::Model, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateModel>)
                    { ++mStats.updates; valid = valid && sync(Kind::Model, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireModel>)
                    { ++mStats.retires; valid = valid && sync(Kind::Model, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateMaterial>)
                    { ++mStats.creates; valid = valid && sync(Kind::Material, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateMaterial>)
                    { ++mStats.updates; valid = valid && sync(Kind::Material, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireMaterial>)
                    { ++mStats.retires; valid = valid && sync(Kind::Material, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateTexture>)
                    { ++mStats.creates; valid = valid && sync(Kind::Texture, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateTexture>)
                    { ++mStats.updates; valid = valid && sync(Kind::Texture, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireTexture>)
                    { ++mStats.retires; valid = valid && sync(Kind::Texture, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateSkeleton>)
                    { ++mStats.creates; valid = valid && sync(Kind::Skeleton, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateSkeleton>)
                    { ++mStats.updates; valid = valid && sync(Kind::Skeleton, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireSkeleton>)
                    { ++mStats.retires; valid = valid && sync(Kind::Skeleton, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateInstance>)
                    { ++mStats.creates; valid = valid && sync(Kind::Instance, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateInstance>)
                    { ++mStats.updates; valid = valid && sync(Kind::Instance, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::ReparentInstance>)
                    { ++mStats.reparents; valid = valid && sync(Kind::Instance, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireInstance>)
                    { ++mStats.retires; valid = valid && sync(Kind::Instance, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateChunk>)
                    { ++mStats.creates; valid = valid && sync(Kind::Chunk, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateChunk>)
                    { ++mStats.updates; valid = valid && sync(Kind::Chunk, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireChunk>)
                    { ++mStats.retires; valid = valid && sync(Kind::Chunk, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::CreateLight>)
                    { lightTouched = true; ++mStats.creates; valid = valid && sync(Kind::Light, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::UpdateLight>)
                    { lightTouched = true; ++mStats.updates; valid = valid && sync(Kind::Light, value.handle, world.get(value.handle)); }
                    else if constexpr (std::is_same_v<T, RenderCore::RetireLight>)
                    { lightTouched = true; ++mStats.retires; valid = valid && sync(Kind::Light, value.handle, world.get(value.handle)); }
                }, operation);
                if (!valid)
                    break;
            }
            if (!valid)
            {
                mHealthy = false;
                return false;
            }

            if (lightTouched)
            {
                if (mLightSerial == std::numeric_limits<std::uint64_t>::max())
                {
                    mHealthy = false;
                    return false;
                }
                ++mLightSerial;
            }
            mLastSequence = batch.sequence();
            mWorldRevision = world.revision();
            return true;
        }

        [[nodiscard]] bool current(const RenderCore::RenderWorld& world) const noexcept
        {
            return mHealthy && mEpoch == world.epoch() && mWorldRevision == world.revision();
        }

        [[nodiscard]] bool healthy() const noexcept { return mHealthy; }
        [[nodiscard]] RenderCore::WorldEpoch epoch() const noexcept { return mEpoch; }
        [[nodiscard]] RenderCore::RenderWorldRevision worldRevision() const noexcept { return mWorldRevision; }
        [[nodiscard]] RenderCore::UpdateSequence lastSequence() const noexcept { return mLastSequence; }
        [[nodiscard]] std::uint64_t lightSerial() const noexcept { return mLightSerial; }
        [[nodiscard]] const DeltaStats& stats() const noexcept { return mStats; }

        [[nodiscard]] const DirtyRange& dirty(Kind kind) const noexcept
        {
            return mDirty[index(kind)];
        }

        [[nodiscard]] const std::vector<Slot>& table(Kind kind) const noexcept
        {
            return mTables[index(kind)];
        }

        [[nodiscard]] std::size_t live(Kind kind) const noexcept
        {
            return std::count_if(mTables[index(kind)].begin(), mTables[index(kind)].end(),
                [](const Slot& slot) { return slot.live; });
        }

        void clearDirty() noexcept
        {
            for (auto& range : mDirty)
                range.clear();
        }

    private:
        [[nodiscard]] static constexpr std::size_t index(Kind kind) noexcept
        {
            return static_cast<std::size_t>(kind);
        }

        template <class Handle, class Record>
        [[nodiscard]] bool sync(Kind kind, Handle handle, const Record* record) noexcept
        {
            if (!handle.valid())
                return false;
            auto& table = mTables[index(kind)];
            if (handle.slot() >= table.size())
                table.resize(static_cast<std::size_t>(handle.slot()) + 1);
            Slot& slot = table[handle.slot()];
            slot.generation = handle.generation();
            slot.revision = record ? record->revision : RenderCore::ResourceRevision{};
            slot.live = record != nullptr;
            if (record && !record->revision.valid())
                return false;
            mDirty[index(kind)].include(handle.slot());
            return true;
        }

        static constexpr std::size_t TableCount = static_cast<std::size_t>(Kind::Count);
        std::array<std::vector<Slot>, TableCount> mTables;
        std::array<DirtyRange, TableCount> mDirty;
        RenderCore::WorldEpoch mEpoch;
        RenderCore::RenderWorldRevision mWorldRevision;
        RenderCore::UpdateSequence mLastSequence;
        std::uint64_t mLightSerial = 1;
        DeltaStats mStats;
        bool mHealthy = false;
    };
}

#endif
