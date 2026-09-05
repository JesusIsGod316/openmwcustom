#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_RESIDENCYLEDGER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_RESIDENCYLEDGER_H

#include <components/rendercore/resources.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace RenderVsg
{
    enum class ResidencyCategory : std::uint8_t
    {
        TexturesImages,
        MeshVertexIndex,
        Skinning,
        InstanceMaterialLight,
        RenderTargetsHistory,
        ShadowMaps,
        WaterTargets,
        PostProcess,
        StagingTemporary,
        Count,
    };

    inline constexpr std::size_t ResidencyCategoryCount = static_cast<std::size_t>(ResidencyCategory::Count);

    struct ResidencyCounters
    {
        std::uint64_t logicalLiveBytes = 0;
        std::uint64_t residentBytes = 0;
        std::uint64_t pendingUploadBytes = 0;
        std::uint64_t pendingRetireBytes = 0;
        std::uint64_t pinnedBytes = 0;
        std::uint64_t evictableBytes = 0;
    };

    struct ResidencySnapshot
    {
        std::array<ResidencyCounters, ResidencyCategoryCount> categories{};
        ResidencyCounters total{};
    };

    class ResidencyLedger final
    {
    public:
        bool addLogicalLive(ResidencyCategory category, std::uint64_t bytes);
        bool removeLogicalLive(ResidencyCategory category, std::uint64_t bytes);

        bool beginUpload(ResidencyCategory category, std::uint64_t bytes);
        bool cancelUpload(ResidencyCategory category, std::uint64_t bytes);
        bool commitUpload(ResidencyCategory category, std::uint64_t bytes, bool pinned);

        // Marks resident allocation bytes for release after the frame which can
        // last reference them is known complete. Resident bytes remain charged
        // until collectRetired() observes that completion point.
        bool queueRetire(
            ResidencyCategory category, std::uint64_t bytes, bool pinned, RenderCore::FrameId lastUseFrame);

        [[nodiscard]] std::uint64_t collectRetired(RenderCore::FrameId completedFrame);
        [[nodiscard]] ResidencySnapshot snapshot() const;
        [[nodiscard]] std::size_t pendingRetirementCount() const;

    private:
        struct Retirement
        {
            ResidencyCategory category = ResidencyCategory::TexturesImages;
            std::uint64_t bytes = 0;
            bool pinned = false;
            RenderCore::FrameId lastUseFrame;
        };

        static std::size_t index(ResidencyCategory category) noexcept;
        static bool checkedAdd(std::uint64_t& target, std::uint64_t bytes) noexcept;
        static bool checkedSubtract(std::uint64_t& target, std::uint64_t bytes) noexcept;

        [[nodiscard]] std::uint64_t pendingClassBytesLocked(ResidencyCategory category, bool pinned) const noexcept;

        mutable std::mutex mMutex;
        std::array<ResidencyCounters, ResidencyCategoryCount> mCategories{};
        std::vector<Retirement> mRetirements;
    };
}

#endif