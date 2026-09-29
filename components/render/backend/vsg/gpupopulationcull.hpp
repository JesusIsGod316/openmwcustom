#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_GPUPOPULATIONCULL_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_GPUPOPULATIONCULL_H

#include "staticworldplan.hpp"

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/Array.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Group.h>
#include <vsg/vk/Device.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace RenderVsg
{
    struct GpuPopulationCullStats
    {
        std::size_t placements = 0;
        std::size_t draws = 0;
        std::size_t indirectCommands = 0;
    };

    struct GpuPopulationCullBuild
    {
        vsg::ref_ptr<vsg::Group> compute;
        GpuPopulationCullStats stats;
        bool active = false;
    };

    // Five vec4 values shared by every P3A population:
    // columns 0..3 of projection*view, then camera.xyz/lodScale.
    [[nodiscard]] vsg::ref_ptr<vsg::vec4Array> createGpuPopulationCullViewData();

    void updateGpuPopulationCullViewData(
        vsg::vec4Array& data, const RenderCore::FrameView& view);

    // Replaces eligible hardware-instanced VertexIndexDraw commands below
    // graphicsRoot with commands that issue vkCmdDrawIndexedIndirect for
    // indirectViewId. All other view IDs retain the exact direct draw fallback.
    //
    // The compute graph writes every VkDrawIndexedIndirectCommand field each
    // frame and never reads results back to the CPU. The first P3 slice performs
    // conservative per-placement frustum and authored maximum-distance culling.
    // Hi-Z and command compaction intentionally remain later P3 slices.
    [[nodiscard]] GpuPopulationCullBuild enableGpuPopulationCull(
        const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan,
        vsg::Group& graphicsRoot,
        vsg::Device& device,
        std::uint32_t indirectViewId,
        vsg::ref_ptr<vsg::vec4Array> viewData,
        std::string& diagnostic);
}

#endif
