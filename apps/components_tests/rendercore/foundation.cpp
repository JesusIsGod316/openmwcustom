#include <components/rendercore/handles.hpp>
#include <components/rendercore/math.hpp>
#include <components/rendercore/resources.hpp>

#include <gtest/gtest.h>

#include <type_traits>

namespace
{
    static_assert(!std::is_same_v<RenderCore::MeshHandle, RenderCore::MaterialHandle>);
    static_assert(!std::is_convertible_v<RenderCore::MeshHandle, RenderCore::MaterialHandle>);
    static_assert(!std::is_convertible_v<RenderCore::WorldEpoch, RenderCore::ResourceRevision>);

    TEST(RenderCoreHandles, DefaultHandleIsInvalid)
    {
        const RenderCore::InstanceHandle handle;
        EXPECT_FALSE(handle.valid());
        EXPECT_FALSE(static_cast<bool>(handle));
        EXPECT_EQ(handle.slot(), RenderCore::InvalidHandleSlot);
        EXPECT_EQ(handle.generation(), 0u);
    }

    TEST(RenderCoreHandles, SlotAndGenerationMustBothBeValid)
    {
        const auto zeroGeneration = RenderCore::MeshHandle::fromParts(4u, 0u);
        const auto invalidSlot = RenderCore::MeshHandle::fromParts(RenderCore::InvalidHandleSlot, 1u);
        const auto valid = RenderCore::MeshHandle::fromParts(4u, 1u);

        EXPECT_FALSE(zeroGeneration.valid());
        EXPECT_FALSE(invalidSlot.valid());
        EXPECT_TRUE(valid.valid());
        EXPECT_EQ(valid.slot(), 4u);
        EXPECT_EQ(valid.generation(), 1u);
    }

    TEST(RenderCoreHandles, HandleValueEqualityIncludesGeneration)
    {
        const auto first = RenderCore::ChunkHandle::fromParts(3u, 7u);
        const auto same = RenderCore::ChunkHandle::fromParts(3u, 7u);
        const auto replacement = RenderCore::ChunkHandle::fromParts(3u, 8u);

        EXPECT_EQ(first, same);
        EXPECT_NE(first, replacement);
    }

    TEST(RenderCoreIdentity, MonotonicIdentityTypesStartInvalid)
    {
        EXPECT_FALSE(RenderCore::WorldEpoch{}.valid());
        EXPECT_FALSE(RenderCore::ResourceRevision{}.valid());
        EXPECT_FALSE(RenderCore::UpdateSequence{}.valid());

        EXPECT_TRUE(RenderCore::InitialWorldEpoch.valid());
        EXPECT_EQ(RenderCore::InitialWorldEpoch.value(), 1u);
        EXPECT_EQ(RenderCore::InitialResourceRevision.value(), 1u);
        EXPECT_EQ(RenderCore::InitialUpdateSequence.value(), 1u);
    }

    TEST(RenderCoreIdentity, MonotonicIdentityOrderingIsExplicit)
    {
        const RenderCore::UpdateSequence first{ 1 };
        const RenderCore::UpdateSequence second{ 2 };

        EXPECT_LT(first, second);
        EXPECT_GT(second, first);
    }

    TEST(RenderCoreMath, WorldTransformDefaultsToExplicitIdentity)
    {
        const RenderCore::WorldTransform transform;

        EXPECT_DOUBLE_EQ(transform.translation.x, 0.0);
        EXPECT_DOUBLE_EQ(transform.translation.y, 0.0);
        EXPECT_DOUBLE_EQ(transform.translation.z, 0.0);

        EXPECT_FLOAT_EQ(transform.rotation.w, 1.0f);
        EXPECT_FLOAT_EQ(transform.rotation.x, 0.0f);
        EXPECT_FLOAT_EQ(transform.rotation.y, 0.0f);
        EXPECT_FLOAT_EQ(transform.rotation.z, 0.0f);

        EXPECT_FLOAT_EQ(transform.scale.x, 1.0f);
        EXPECT_FLOAT_EQ(transform.scale.y, 1.0f);
        EXPECT_FLOAT_EQ(transform.scale.z, 1.0f);
    }
}
