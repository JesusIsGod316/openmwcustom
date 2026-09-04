#include <components/rendercore/handles.hpp>
#include <components/rendercore/math.hpp>
#include <components/rendercore/resources.hpp>
#include <components/rendercore/slottable.hpp>

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
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

    TEST(RenderCoreSlotTable, RetireAndReuseRejectsStaleHandle)
    {
        RenderCore::SlotTable<RenderCore::InstanceHandle, int> table;
        const auto first = table.insert(10);
        ASSERT_TRUE(first.has_value());
        ASSERT_NE(table.get(*first), nullptr);
        EXPECT_EQ(*table.get(*first), 10);

        ASSERT_TRUE(table.retire(*first));
        EXPECT_EQ(table.get(*first), nullptr);

        const auto replacement = table.insert(20);
        ASSERT_TRUE(replacement.has_value());
        EXPECT_EQ(replacement->slot(), first->slot());
        EXPECT_EQ(replacement->generation(), first->generation() + 1u);
        EXPECT_EQ(table.get(*first), nullptr);
        ASSERT_NE(table.get(*replacement), nullptr);
        EXPECT_EQ(*table.get(*replacement), 20);
    }

    TEST(RenderCoreSlotTable, ReuseOrderIsDeterministicLowestSlot)
    {
        RenderCore::SlotTable<RenderCore::MeshHandle, int> table;
        const auto a = table.insert(1);
        const auto b = table.insert(2);
        const auto c = table.insert(3);
        ASSERT_TRUE(a && b && c);

        ASSERT_TRUE(table.retire(*b));
        ASSERT_TRUE(table.retire(*a));

        const auto firstReuse = table.insert(4);
        const auto secondReuse = table.insert(5);
        ASSERT_TRUE(firstReuse && secondReuse);
        EXPECT_EQ(firstReuse->slot(), 0u);
        EXPECT_EQ(secondReuse->slot(), 1u);
    }

    TEST(RenderCoreSlotTable, IdenticalOrderedOperationsProduceIdenticalHandles)
    {
        RenderCore::SlotTable<RenderCore::LightHandle, int> first;
        RenderCore::SlotTable<RenderCore::LightHandle, int> second;

        const auto a1 = first.insert(1);
        const auto b1 = first.insert(2);
        const auto a2 = second.insert(1);
        const auto b2 = second.insert(2);
        ASSERT_TRUE(a1 && b1 && a2 && b2);

        ASSERT_TRUE(first.retire(*a1));
        ASSERT_TRUE(second.retire(*a2));
        const auto c1 = first.insert(3);
        const auto c2 = second.insert(3);
        ASSERT_TRUE(c1 && c2);

        EXPECT_EQ(*a1, *a2);
        EXPECT_EQ(*b1, *b2);
        EXPECT_EQ(*c1, *c2);
    }

    TEST(RenderCoreSlotTable, GenerationWrapPolicyTombstonesInsteadOfAliasingZero)
    {
        constexpr auto retired = RenderCore::detail::retireGeneration<std::uint32_t>(
            std::numeric_limits<std::uint32_t>::max());
        static_assert(retired.tombstone);
        static_assert(retired.next == 0u);

        EXPECT_TRUE(retired.tombstone);
        EXPECT_EQ(retired.next, 0u);
    }

    TEST(RenderCoreSlotTable, RetireAllInvalidatesLiveHandlesAndPreservesGenerationProgress)
    {
        RenderCore::SlotTable<RenderCore::ChunkHandle, int> table;
        const auto first = table.insert(10);
        const auto second = table.insert(20);
        ASSERT_TRUE(first && second);

        table.retireAll();
        EXPECT_EQ(table.liveCount(), 0u);
        EXPECT_FALSE(table.contains(*first));
        EXPECT_FALSE(table.contains(*second));

        const auto replacement = table.insert(30);
        ASSERT_TRUE(replacement);
        EXPECT_EQ(replacement->slot(), 0u);
        EXPECT_EQ(replacement->generation(), first->generation() + 1u);
    }
}
