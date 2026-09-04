#ifndef OPENMW_COMPONENTS_RENDERCORE_RESOURCES_H
#define OPENMW_COMPONENTS_RENDERCORE_RESOURCES_H

#include <compare>
#include <cstdint>

namespace RenderCore
{
    template <class Tag>
    class MonotonicId final
    {
    public:
        using Value = std::uint64_t;

        constexpr MonotonicId() noexcept = default;
        explicit constexpr MonotonicId(Value value) noexcept
            : mValue(value)
        {
        }

        [[nodiscard]] constexpr bool valid() const noexcept { return mValue != 0; }
        explicit constexpr operator bool() const noexcept { return valid(); }
        [[nodiscard]] constexpr Value value() const noexcept { return mValue; }

        friend constexpr bool operator==(MonotonicId, MonotonicId) noexcept = default;
        friend constexpr auto operator<=>(MonotonicId, MonotonicId) noexcept = default;

    private:
        Value mValue = 0;
    };

    struct WorldEpochTag final
    {
    };
    struct ResourceRevisionTag final
    {
    };
    struct RenderWorldRevisionTag final
    {
    };
    struct UpdateSequenceTag final
    {
    };
    struct FrameIdTag final
    {
    };
    struct HistoryEpochTag final
    {
    };

    using WorldEpoch = MonotonicId<WorldEpochTag>;
    using ResourceRevision = MonotonicId<ResourceRevisionTag>;
    using RenderWorldRevision = MonotonicId<RenderWorldRevisionTag>;
    using UpdateSequence = MonotonicId<UpdateSequenceTag>;
    using FrameId = MonotonicId<FrameIdTag>;
    using HistoryEpoch = MonotonicId<HistoryEpochTag>;

    inline constexpr WorldEpoch InitialWorldEpoch{ 1 };
    inline constexpr ResourceRevision InitialResourceRevision{ 1 };
    inline constexpr RenderWorldRevision InitialRenderWorldRevision{ 1 };
    inline constexpr UpdateSequence InitialUpdateSequence{ 1 };
    inline constexpr FrameId InitialFrameId{ 1 };
    inline constexpr HistoryEpoch InitialHistoryEpoch{ 1 };
}

#endif
