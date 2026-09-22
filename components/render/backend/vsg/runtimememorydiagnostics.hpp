#ifndef OPENMW_RENDER_VSG_RUNTIMEMEMORYDIAGNOSTICS_H
#define OPENMW_RENDER_VSG_RUNTIMEMEMORYDIAGNOSTICS_H
#include <components/debug/runtimediagnostics.hpp>
#include <components/rendercore/renderworld.hpp>
#include <vsg/vk/PhysicalDevice.h>
#include <vsg/vk/Device.h>
#include <vsg/vk/MemoryBufferPools.h>
#include <unordered_set>

namespace RenderVsg
{
    inline std::uint64_t meshCapacityBytes(const RenderCore::MeshPayload& mesh) noexcept
    {
        using Debug::RuntimeDiagnostics::capacityBytes;
        auto bytes = capacityBytes(mesh.positions) + capacityBytes(mesh.normals) + capacityBytes(mesh.tangents)
            + capacityBytes(mesh.bitangents) + capacityBytes(mesh.colors) + capacityBytes(mesh.texCoordSets)
            + capacityBytes(mesh.indices) + capacityBytes(mesh.surfaces);
        for (const auto& uv : mesh.texCoordSets) bytes += capacityBytes(uv);
        return bytes;
    }

    // CPU-owned neutral payload capacity only. Shared mesh payloads are counted
    // once, with an explicit cap/coverage marker. Not a total process heap census.
    inline void reportNeutralMemory(const RenderCore::RenderWorld& world) noexcept
    {
        try
        {
            constexpr std::size_t limit = 32768;
            std::unordered_set<const RenderCore::MeshPayload*> payloads;
            std::uint64_t records = 0, bytes = 0, duplicateReferences = 0, capped = 0;
            world.forEachMesh([&](RenderCore::MeshHandle, const RenderCore::MeshRecord& mesh) {
                ++records;
                if (!mesh.payload) return;
                if (payloads.size() >= limit) { ++capped; return; }
                if (payloads.insert(mesh.payload.get()).second) bytes += meshCapacityBytes(*mesh.payload);
                else ++duplicateReferences;
            });
            Debug::RuntimeDiagnostics::emit("neutral_memory", "render_world", "Mesh vector capacities; skins/morphs/models/allocator metadata excluded", {
                {"epoch", world.epoch().value()}, {"mesh_records", records}, {"unique_payloads", payloads.size()},
                {"mesh_capacity_bytes", bytes}, {"shared_payload_refs", duplicateReferences},
                {"unmeasured_after_limit", capped}, {"instances", world.instanceCount()}, {"chunks", world.chunkCount()} });
        }
        catch (...) { Debug::RuntimeDiagnostics::emit("coverage", "render_world", "neutral census unavailable", {{"available", 0}}); }
    }
    inline void reportVulkanMemory(vsg::PhysicalDevice& physical, vsg::Device& device) noexcept
    {
        try
        {
            VkPhysicalDeviceMemoryProperties2 properties{};
            properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
            VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{};
            budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
            const bool supported = physical.supportsDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
            if (supported) properties.pNext = &budget;
            vkGetPhysicalDeviceMemoryProperties2(physical.vk(), &properties);
            for (std::uint32_t i = 0; i < properties.memoryProperties.memoryHeapCount; ++i)
            {
                const auto& heap = properties.memoryProperties.memoryHeaps[i];
                Debug::RuntimeDiagnostics::emit("vulkan_heap", "driver_estimate", {}, {
                    {"heap", i}, {"budget_available", supported}, {"heap_size_bytes", heap.size},
                    {"device_local", (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0},
                    {"budget_bytes", budget.heapBudget[i]}, {"usage_bytes", budget.heapUsage[i]} });
            }
            if (auto pools = device.deviceMemoryBufferPools.ref_ptr())
                Debug::RuntimeDiagnostics::emit("vsg_pool", "device_memory_pool", "Excludes allocations outside this VSG pool", {
                    {"reserved_bytes", pools->computeMemoryTotalReserved()},
                    {"available_bytes", pools->computeMemoryTotalAvailable()} });
        }
        catch (...) { Debug::RuntimeDiagnostics::emit("coverage", "vulkan_memory", "budget/pool query unavailable", {{"available", 0}}); }
    }
}
#endif
