#include "gpupopulationcull.hpp"

#include <vsg/state/BindDescriptorSet.h>
#include <vsg/commands/BindIndexBuffer.h>
#include <vsg/commands/Dispatch.h>
#include <vsg/commands/PipelineBarrier.h>
#include <vsg/nodes/StateGroup.h>
#include <vsg/nodes/VertexIndexDraw.h>
#include <vsg/state/Buffer.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/ComputePipeline.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/PipelineLayout.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/vk/Context.h>
#include <vsg/vk/CommandBuffer.h>
#include <vsg/vk/vk_buffer.h>

#include <glm/common.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace RenderVsg
{
    namespace
    {
        constexpr std::uint32_t CullWorkgroupSize = 64;

        class PopulationIndirectDraw final : public vsg::Inherit<vsg::Command, PopulationIndirectDraw>
        {
        public:
            std::uint32_t indexCount = 0;
            std::uint32_t instanceCount = 0;
            std::uint32_t firstIndex = 0;
            std::int32_t vertexOffset = 0;
            std::uint32_t firstInstance = 0;
            std::uint32_t firstBinding = 0;
            vsg::BufferInfoList arrays;
            vsg::ref_ptr<vsg::BufferInfo> indices;
            std::uint32_t indirectViewId = 0;
            vsg::ref_ptr<vsg::BufferInfo> indirect;
            std::uint32_t indirectDrawCount = 0;
            bool compacted = false;
            vsg::ref_ptr<vsg::BufferInfo> compactTranslations;
            vsg::ref_ptr<vsg::BufferInfo> compactRotations;
            vsg::ref_ptr<vsg::BufferInfo> compactScales;

            void compile(vsg::Context& context) override
            {
                if (arrays.empty() || !indices)
                    return;

                bool requiresCreateAndCopy = indices->requiresCopy(context.deviceID);
                if (!requiresCreateAndCopy)
                {
                    for (const auto& array : arrays)
                    {
                        if (array->requiresCopy(context.deviceID))
                        {
                            requiresCreateAndCopy = true;
                            break;
                        }
                    }
                }
                if (requiresCreateAndCopy)
                {
                    vsg::BufferInfoList combined(arrays);
                    combined.push_back(indices);
                    vsg::createBufferAndTransferData(context, combined,
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                        VK_SHARING_MODE_EXCLUSIVE);
                }
                vsg::assignVulkanArrayData(context.deviceID, arrays, mVulkanData[context.deviceID]);
                mIndexType = vsg::computeIndexType(indices->data);
            }

            void record(vsg::CommandBuffer& commandBuffer) const override
            {
                const auto& data = mVulkanData[commandBuffer.deviceID];
                VkCommandBuffer commands = commandBuffer;
                const bool indirectMain = commandBuffer.viewID == indirectViewId
                    && indirect && indirect->buffer && indirectDrawCount != 0;

                if (indirectMain && compacted)
                {
                    if (data.vkBuffers.size() < 4 || !compactTranslations || !compactTranslations->buffer
                        || !compactRotations || !compactRotations->buffer || !compactScales
                        || !compactScales->buffer)
                        return;

                    vkCmdBindVertexBuffers(commands, firstBinding, 1, data.vkBuffers.data(), data.offsets.data());
                    const VkBuffer compactBuffers[] = {
                        compactTranslations->buffer->vk(commandBuffer.deviceID),
                        compactRotations->buffer->vk(commandBuffer.deviceID),
                        compactScales->buffer->vk(commandBuffer.deviceID),
                    };
                    const VkDeviceSize compactOffsets[] = {
                        compactTranslations->offset, compactRotations->offset, compactScales->offset,
                    };
                    vkCmdBindVertexBuffers(commands, firstBinding + 1, 3, compactBuffers, compactOffsets);
                    if (data.vkBuffers.size() > 4)
                        vkCmdBindVertexBuffers(commands, firstBinding + 4,
                            static_cast<std::uint32_t>(data.vkBuffers.size() - 4),
                            data.vkBuffers.data() + 4, data.offsets.data() + 4);
                }
                else
                {
                    vkCmdBindVertexBuffers(commands, firstBinding, static_cast<std::uint32_t>(data.vkBuffers.size()),
                        data.vkBuffers.data(), data.offsets.data());
                }

                vkCmdBindIndexBuffer(commands, indices->buffer->vk(commandBuffer.deviceID), indices->offset, mIndexType);
                if (indirectMain)
                    vkCmdDrawIndexedIndirect(commands, indirect->buffer->vk(commandBuffer.deviceID),
                        indirect->offset, indirectDrawCount, sizeof(VkDrawIndexedIndirectCommand));
                else
                    vkCmdDrawIndexed(commands, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
            }

        private:
            vsg::vk_buffer<vsg::VulkanArrayData> mVulkanData;
            VkIndexType mIndexType = VK_INDEX_TYPE_UINT16;
        };

        struct AssetBounds
        {
            glm::dvec3 minimum{ std::numeric_limits<double>::max() };
            glm::dvec3 maximum{ std::numeric_limits<double>::lowest() };
            bool valid = false;
        };

        [[nodiscard]] std::optional<AssetBounds> computeAssetBounds(
            const RenderCore::RenderWorld& world, const StaticAssetPlan& asset)
        {
            AssetBounds result;
            for (const StaticDrawPlan& draw : asset.draws)
            {
                const RenderCore::MeshRecord* mesh = world.get(draw.mesh);
                if (!mesh || !mesh->payload || mesh->payload->positions.empty())
                    return std::nullopt;
                const glm::dmat4 transform(draw.worldTransform);
                for (const glm::vec3& position : mesh->payload->positions)
                {
                    const glm::dvec4 point = transform * glm::dvec4(position, 1.0);
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)
                        || !std::isfinite(point.z) || std::abs(point.w - 1.0) > 1e-6)
                        return std::nullopt;
                    result.minimum = glm::min(result.minimum, glm::dvec3(point));
                    result.maximum = glm::max(result.maximum, glm::dvec3(point));
                    result.valid = true;
                }
            }
            if (!result.valid)
                return std::nullopt;
            return result;
        }

        struct PackedPlacementVisibility
        {
            vsg::vec4 minimum;
            vsg::vec4 maximum;
            vsg::vec4 lod;
        };

        [[nodiscard]] std::optional<PackedPlacementVisibility> packPlacementVisibility(
            const AssetBounds& asset, const RenderCore::PopulationInstanceRecord& placement)
        {
            const glm::dmat4 matrix = staticInstancePlacementMatrix(placement.transform);
            glm::dvec3 minimum(std::numeric_limits<double>::max());
            glm::dvec3 maximum(std::numeric_limits<double>::lowest());
            for (unsigned corner = 0; corner < 8; ++corner)
            {
                const glm::dvec3 local(
                    corner & 1 ? asset.maximum.x : asset.minimum.x,
                    corner & 2 ? asset.maximum.y : asset.minimum.y,
                    corner & 4 ? asset.maximum.z : asset.minimum.z);
                const glm::dvec4 point = matrix * glm::dvec4(local, 1.0);
                if (!std::isfinite(point.x) || !std::isfinite(point.y)
                    || !std::isfinite(point.z) || std::abs(point.w - 1.0) > 1e-6)
                    return std::nullopt;
                minimum = glm::min(minimum, glm::dvec3(point));
                maximum = glm::max(maximum, glm::dvec3(point));
            }
            const glm::dvec4 lodCenter = matrix * glm::dvec4(placement.lod.center, 1.0);
            if (!std::isfinite(lodCenter.x) || !std::isfinite(lodCenter.y)
                || !std::isfinite(lodCenter.z) || std::abs(lodCenter.w - 1.0) > 1e-6)
                return std::nullopt;

            double maximumDistance = static_cast<double>(placement.lod.maximumDistance)
                * static_cast<double>(placement.lod.scale);
            if (!std::isfinite(maximumDistance)
                || maximumDistance >= static_cast<double>(std::numeric_limits<float>::max()))
                maximumDistance = static_cast<double>(std::numeric_limits<float>::max());
            maximumDistance = std::max(0.0, maximumDistance);

            const auto finiteFloat = [](double value) {
                return std::isfinite(value)
                    && value >= -static_cast<double>(std::numeric_limits<float>::max())
                    && value <= static_cast<double>(std::numeric_limits<float>::max());
            };
            for (const double value : {minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z,
                     lodCenter.x, lodCenter.y, lodCenter.z})
                if (!finiteFloat(value))
                    return std::nullopt;

            return PackedPlacementVisibility{
                .minimum = vsg::vec4(static_cast<float>(minimum.x), static_cast<float>(minimum.y),
                    static_cast<float>(minimum.z), 0.0f),
                .maximum = vsg::vec4(static_cast<float>(maximum.x), static_cast<float>(maximum.y),
                    static_cast<float>(maximum.z), 0.0f),
                .lod = vsg::vec4(static_cast<float>(lodCenter.x), static_cast<float>(lodCenter.y),
                    static_cast<float>(lodCenter.z), static_cast<float>(maximumDistance)),
            };
        }

        [[nodiscard]] bool packPlacement(
            vsg::vec4Array& packed, std::size_t index, const AssetBounds& asset,
            const RenderCore::PopulationInstanceRecord& placement)
        {
            const auto visibility = packPlacementVisibility(asset, placement);
            if (!visibility)
                return false;
            const std::size_t base = index * 3u;
            packed.set(base + 0u, visibility->minimum);
            packed.set(base + 1u, visibility->maximum);
            packed.set(base + 2u, visibility->lod);
            return true;
        }

        [[nodiscard]] bool compactPopulationSafe(
            const RenderCore::RenderWorld& world, const StaticPopulationPlan& plan) noexcept
        {
            // Atomic compaction changes visible instance order. Keep P3A's
            // placement-ordered command stream for any draw whose result can
            // depend on submission order.
            for (const StaticDrawPlan& draw : plan.asset.draws)
            {
                const RenderCore::MaterialRecord* material = world.get(draw.material);
                if (!material || material->alphaBlendEnabled || material->decal
                    || material->stencil.enabled || !material->depthWrite)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool packCompactPlacement(vsg::vec4Array& packed, std::size_t index,
            const AssetBounds& asset, const RenderCore::PopulationInstanceRecord& placement,
            const RenderCore::WorldPosition& origin)
        {
            const auto visibility = packPlacementVisibility(asset, placement);
            if (!visibility)
                return false;
            const std::size_t base = index * 6u;
            packed.set(base + 0u, visibility->minimum);
            packed.set(base + 1u, visibility->maximum);
            packed.set(base + 2u, visibility->lod);

            const glm::dvec3 relative = placement.transform.translation - origin;
            const auto finiteFloat = [](double value) {
                return std::isfinite(value)
                    && value >= -static_cast<double>(std::numeric_limits<float>::max())
                    && value <= static_cast<double>(std::numeric_limits<float>::max());
            };
            if (!finiteFloat(relative.x) || !finiteFloat(relative.y) || !finiteFloat(relative.z))
                return false;
            packed.set(base + 3u, vsg::vec4(static_cast<float>(relative.x),
                static_cast<float>(relative.y), static_cast<float>(relative.z), 0.0f));
            packed.set(base + 4u, vsg::vec4(placement.transform.rotation.x, placement.transform.rotation.y,
                placement.transform.rotation.z, placement.transform.rotation.w));
            packed.set(base + 5u, vsg::vec4(
                placement.transform.scale.x, placement.transform.scale.y, placement.transform.scale.z, 0.0f));
            return true;
        }

        void collectDraws(vsg::Group& group, std::vector<vsg::VertexIndexDraw*>& draws)
        {
            for (auto& child : group.children)
            {
                if (auto* draw = dynamic_cast<vsg::VertexIndexDraw*>(child.get()))
                {
                    draws.push_back(draw);
                    continue;
                }
                if (auto* nested = dynamic_cast<vsg::Group*>(child.get()))
                    collectDraws(*nested, draws);
            }
        }

        void replaceDraws(vsg::Group& group, const std::vector<vsg::ref_ptr<vsg::BufferInfo>>& indirectRanges,
            std::uint32_t indirectViewId, std::uint32_t placementCount, bool compacted,
            vsg::ref_ptr<vsg::BufferInfo> compactTranslations, vsg::ref_ptr<vsg::BufferInfo> compactRotations,
            vsg::ref_ptr<vsg::BufferInfo> compactScales, std::size_t& drawIndex)
        {
            for (auto& child : group.children)
            {
                if (auto* direct = dynamic_cast<vsg::VertexIndexDraw*>(child.get()))
                {
                    auto replacement = PopulationIndirectDraw::create();
                    replacement->indexCount = direct->indexCount;
                    replacement->instanceCount = direct->instanceCount;
                    replacement->firstIndex = direct->firstIndex;
                    replacement->vertexOffset = static_cast<std::int32_t>(direct->vertexOffset);
                    replacement->firstInstance = direct->firstInstance;
                    replacement->firstBinding = direct->firstBinding;
                    replacement->arrays = direct->arrays;
                    replacement->indices = direct->indices;
                    replacement->indirectViewId = indirectViewId;
                    replacement->indirect = indirectRanges.at(drawIndex++);
                    replacement->indirectDrawCount = compacted ? 1u : placementCount;
                    replacement->compacted = compacted;
                    replacement->compactTranslations = compactTranslations;
                    replacement->compactRotations = compactRotations;
                    replacement->compactScales = compactScales;
                    child = replacement;
                    continue;
                }
                if (auto* nested = dynamic_cast<vsg::Group*>(child.get()))
                    replaceDraws(*nested, indirectRanges, indirectViewId, placementCount, compacted,
                        compactTranslations, compactRotations, compactScales, drawIndex);
            }
        }

        [[nodiscard]] const char* cullShaderSource()
        {
            return R"glsl(
#version 450
layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer PlacementBuffer
{
    vec4 placementData[];
};

layout(set = 0, binding = 1, std430) readonly buffer DrawTemplateBuffer
{
    uvec4 drawTemplates[];
};

struct DrawCommand
{
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int vertexOffset;
    uint firstInstance;
};

layout(set = 0, binding = 2, std430) buffer DrawCommandBuffer
{
    DrawCommand commands[];
};

layout(set = 0, binding = 3, std430) readonly buffer ViewBuffer
{
    vec4 viewData[];
};

void main()
{
    uint placement = gl_GlobalInvocationID.x;
    uint placementCount = uint(placementData.length()) / 3u;
    if (placementCount == 0u || placement >= placementCount || viewData.length() < 5)
        return;

    vec3 minimum = placementData[placement * 3u + 0u].xyz;
    vec3 maximum = placementData[placement * 3u + 1u].xyz;
    vec4 lod = placementData[placement * 3u + 2u];
    mat4 clipMatrix = mat4(viewData[0], viewData[1], viewData[2], viewData[3]);

    bool outsideLeft = true;
    bool outsideRight = true;
    bool outsideBottom = true;
    bool outsideTop = true;
    bool outsideNear = true;
    bool outsideFar = true;
    for (uint corner = 0u; corner < 8u; ++corner)
    {
        vec3 world = vec3(
            (corner & 1u) != 0u ? maximum.x : minimum.x,
            (corner & 2u) != 0u ? maximum.y : minimum.y,
            (corner & 4u) != 0u ? maximum.z : minimum.z);
        vec4 clip = clipMatrix * vec4(world, 1.0);
        float epsilon = 1.0e-5 * max(1.0, abs(clip.w));
        outsideLeft = outsideLeft && clip.x < -clip.w - epsilon;
        outsideRight = outsideRight && clip.x > clip.w + epsilon;
        outsideBottom = outsideBottom && clip.y < -clip.w - epsilon;
        outsideTop = outsideTop && clip.y > clip.w + epsilon;
        outsideNear = outsideNear && clip.z < -epsilon;
        outsideFar = outsideFar && clip.z > clip.w + epsilon;
    }

    bool visible = !(outsideLeft || outsideRight || outsideBottom
        || outsideTop || outsideNear || outsideFar);
    float maximumDistance = lod.w;
    if (maximumDistance <= 0.0)
        visible = false;
    else if (maximumDistance < 3.0e38)
        visible = visible && distance(lod.xyz, viewData[4].xyz) <= maximumDistance;

    uint drawCount = uint(drawTemplates.length());
    for (uint draw = 0u; draw < drawCount; ++draw)
    {
        uvec4 source = drawTemplates[draw];
        uint commandIndex = draw * placementCount + placement;
        commands[commandIndex].indexCount = source.x;
        commands[commandIndex].instanceCount = visible ? 1u : 0u;
        commands[commandIndex].firstIndex = source.y;
        commands[commandIndex].vertexOffset = int(source.z);
        commands[commandIndex].firstInstance = placement;
    }
}
)glsl";
        }
    }

    vsg::ref_ptr<vsg::vec4Array> createGpuPopulationCullViewData()
    {
        auto result = vsg::vec4Array::create(5);
        result->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
        for (std::size_t i = 0; i < result->size(); ++i)
            result->set(i, vsg::vec4(0.0f, 0.0f, 0.0f, 0.0f));
        result->dirty();
        return result;
    }

    void updateGpuPopulationCullViewData(vsg::vec4Array& data, const RenderCore::FrameView& view)
    {
        if (data.size() < 5)
            return;
        const glm::mat4 clip = view.current.projection.matrix * view.current.view;
        for (glm::length_t column = 0; column < 4; ++column)
            data.set(static_cast<std::size_t>(column),
                vsg::vec4(clip[column].x, clip[column].y, clip[column].z, clip[column].w));
        data.set(4, vsg::vec4(
            static_cast<float>(view.current.worldPosition.x),
            static_cast<float>(view.current.worldPosition.y),
            static_cast<float>(view.current.worldPosition.z), view.lodScale));
        data.dirty();
    }

    GpuPopulationCullBuild enableGpuPopulationCull(
        const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan,
        vsg::Group& graphicsRoot,
        vsg::Device& device,
        std::uint32_t indirectViewId,
        vsg::ref_ptr<vsg::vec4Array> viewData,
        std::string& diagnostic)
    {
        GpuPopulationCullBuild result;
        if (plan.placements.empty() || !viewData || viewData->size() < 5)
            return result;
        if (plan.placements.size() > std::numeric_limits<std::uint32_t>::max() / 3u)
        {
            diagnostic = "P3 GPU population cull exceeds packed placement-buffer range";
            return result;
        }

        std::vector<vsg::VertexIndexDraw*> draws;
        collectDraws(graphicsRoot, draws);
        if (draws.empty())
            return result;
        if (draws.size() > std::numeric_limits<std::uint32_t>::max())
        {
            diagnostic = "P3 GPU population cull exceeds Vulkan draw-template range";
            return result;
        }

        const auto assetBounds = computeAssetBounds(world, plan.asset);
        if (!assetBounds)
        {
            diagnostic = "P3 GPU population cull could not derive conservative model bounds";
            return result;
        }
        const auto placementVectorCount = static_cast<std::uint32_t>(plan.placements.size() * 3u);
        auto placements = vsg::vec4Array::create(placementVectorCount);
        for (std::size_t i = 0; i < plan.placements.size(); ++i)
        {
            if (!packPlacement(*placements, i, *assetBounds, plan.placements[i]))
            {
                diagnostic = "P3 GPU population cull could not pack finite placement bounds";
                return result;
            }
        }

        auto drawTemplates = vsg::uivec4Array::create(static_cast<std::uint32_t>(draws.size()));
        for (std::size_t i = 0; i < draws.size(); ++i)
        {
            const auto* draw = draws[i];
            if (draw->vertexOffset != 0)
            {
                diagnostic = "P3 GPU population cull currently requires zero indexed vertexOffset";
                return result;
            }
            drawTemplates->set(i, vsg::uivec4(
                draw->indexCount, draw->firstIndex, static_cast<std::uint32_t>(draw->vertexOffset), 0u));
        }

        const std::uint64_t commandCount64
            = static_cast<std::uint64_t>(plan.placements.size()) * static_cast<std::uint64_t>(draws.size());
        if (commandCount64 > std::numeric_limits<std::uint32_t>::max())
        {
            diagnostic = "P3 GPU population cull command table is too large";
            return result;
        }
        const VkDeviceSize commandBytes
            = static_cast<VkDeviceSize>(commandCount64) * sizeof(VkDrawIndexedIndirectCommand);
        auto commandBuffer = vsg::createBufferAndMemory(&device, commandBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
            VK_SHARING_MODE_EXCLUSIVE, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!commandBuffer)
        {
            diagnostic = "P3 GPU population cull could not allocate indirect command storage";
            return result;
        }
        auto commandInfo = vsg::BufferInfo::create(commandBuffer, 0, commandBytes);

        std::vector<vsg::ref_ptr<vsg::BufferInfo>> indirectRanges;
        indirectRanges.reserve(draws.size());
        const VkDeviceSize oneDrawBytes
            = static_cast<VkDeviceSize>(plan.placements.size()) * sizeof(VkDrawIndexedIndirectCommand);
        for (std::size_t draw = 0; draw < draws.size(); ++draw)
            indirectRanges.push_back(vsg::BufferInfo::create(
                commandBuffer, static_cast<VkDeviceSize>(draw) * oneDrawBytes, oneDrawBytes));

        auto descriptorLayout = vsg::DescriptorSetLayout::create(vsg::DescriptorSetLayoutBindings{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}});
        auto pipelineLayout = vsg::PipelineLayout::create(
            vsg::DescriptorSetLayouts{descriptorLayout}, vsg::PushConstantRanges{});
        auto shader = vsg::ShaderStage::create(VK_SHADER_STAGE_COMPUTE_BIT, "main", cullShaderSource());
        auto pipeline = vsg::ComputePipeline::create(pipelineLayout, shader);
        auto descriptorSet = vsg::DescriptorSet::create(descriptorLayout, vsg::Descriptors{
            vsg::DescriptorBuffer::create(placements, 0, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(drawTemplates, 1, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(vsg::BufferInfoList{commandInfo}, 2, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
            vsg::DescriptorBuffer::create(viewData, 3, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)});

        auto dispatchState = vsg::StateGroup::create();
        dispatchState->add(vsg::BindComputePipeline::create(pipeline));
        dispatchState->add(vsg::BindDescriptorSet::create(
            VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, descriptorSet));
        const auto placementCount = static_cast<std::uint32_t>(plan.placements.size());
        dispatchState->addChild(vsg::Dispatch::create(
            (placementCount + CullWorkgroupSize - 1u) / CullWorkgroupSize, 1, 1));

        // The command table is persistent across frames. Close both halves of
        // the dependency chain: a later frame must not overwrite commands while
        // an earlier main-view draw can still read them, and this frame's draw
        // must not consume commands before compute has finished writing them.
        auto beforeWrite = vsg::BufferMemoryBarrier::create(
            VK_ACCESS_INDIRECT_COMMAND_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            commandBuffer, 0, commandBytes);
        auto beforeWriteCommand = vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, beforeWrite);
        auto afterWrite = vsg::BufferMemoryBarrier::create(
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
            commandBuffer, 0, commandBytes);
        auto afterWriteCommand = vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
            0, afterWrite);

        result.compute = vsg::Group::create();
        result.compute->addChild(beforeWriteCommand);
        result.compute->addChild(dispatchState);
        result.compute->addChild(afterWriteCommand);

        std::size_t replacementIndex = 0;
        replaceDraws(graphicsRoot, indirectRanges, indirectViewId, placementCount, replacementIndex);
        if (replacementIndex != draws.size())
        {
            diagnostic = "P3 GPU population cull draw replacement lost graph identity";
            result.compute = {};
            return result;
        }

        result.stats.placements = plan.placements.size();
        result.stats.draws = draws.size();
        result.stats.indirectCommands = static_cast<std::size_t>(commandCount64);
        result.active = true;
        return result;
    }
}
