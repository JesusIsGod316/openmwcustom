#ifndef OPENMW_COMPONENTS_RENDERCORE_HANDLES_H
#define OPENMW_COMPONENTS_RENDERCORE_HANDLES_H

#include <cstdint>
#include <limits>

namespace RenderCore
{
    inline constexpr std::uint32_t InvalidHandleSlot = std::numeric_limits<std::uint32_t>::max();

    template <class Tag>
    class Handle final
    {
    public:
        using Slot = std::uint32_t;
        using Generation = std::uint32_t;

        constexpr Handle() noexcept = default;

        [[nodiscard]] static constexpr Handle fromParts(Slot slot, Generation generation) noexcept
        {
            return Handle(slot, generation);
        }

        [[nodiscard]] constexpr bool valid() const noexcept
        {
            return mSlot != InvalidHandleSlot && mGeneration != 0;
        }

        explicit constexpr operator bool() const noexcept { return valid(); }

        [[nodiscard]] constexpr Slot slot() const noexcept { return mSlot; }
        [[nodiscard]] constexpr Generation generation() const noexcept { return mGeneration; }

        friend constexpr bool operator==(Handle, Handle) noexcept = default;

    private:
        constexpr Handle(Slot slot, Generation generation) noexcept
            : mSlot(slot)
            , mGeneration(generation)
        {
        }

        Slot mSlot = InvalidHandleSlot;
        Generation mGeneration = 0;
    };

    struct MeshHandleTag final
    {
    };
    struct MaterialHandleTag final
    {
    };
    struct TextureHandleTag final
    {
    };
    struct SkeletonHandleTag final
    {
    };
    struct InstanceHandleTag final
    {
    };
    struct ChunkHandleTag final
    {
    };
    struct LightHandleTag final
    {
    };

    using MeshHandle = Handle<MeshHandleTag>;
    using MaterialHandle = Handle<MaterialHandleTag>;
    using TextureHandle = Handle<TextureHandleTag>;
    using SkeletonHandle = Handle<SkeletonHandleTag>;
    using InstanceHandle = Handle<InstanceHandleTag>;
    using ChunkHandle = Handle<ChunkHandleTag>;
    using LightHandle = Handle<LightHandleTag>;
}

#endif
