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
#include <memory>
#include <string>

namespace RenderVsg
{
    struct GpuPopulationUploadStats
    {
        std::size_t changedRows = 0;
        std::size_t uploadRows = 0;
        std::size_t uploadBytes = 0;
    };

    // Prepare is read-only; publication happens only after the whole residency
    // transaction succeeds. Upload debt survives skipped/failed submissions.
    // The command-buffer upload copies bytes during recording, not into mapped
    // memory that an older GPU frame might still read.
    class PersistentGpuPopulation
    {
    public:
        struct Update
        {
            virtual ~Update() = default;
            GpuPopulationUploadStats stats;
            glm::dvec3 minimum{};
            glm::dvec3 maximum{};
        };
        virtual ~PersistentGpuPopulation() = default;
        [[nodiscard]] virtual std::shared_ptr<const Update> prepare(
            const StaticPopulationPlan& plan, std::string& diagnostic) const = 0;
        [[nodiscard]] virtual bool publish(std::shared_ptr<const Update> update) noexcept = 0;
        virtual void submitted() noexcept = 0;
    };

    struct GpuPopulationCullStats
    {
        std::size_t placements = 0;
        std::size_t draws = 0;
        std::size_t indirectCommands = 0;
        bool compacted = false;
    };

    struct GpuPopulationCullBuild
    {
        vsg::ref_ptr<vsg::Group> compute;
        // Must execute before ALL views, including VSG shadow pre-renders.
        // It is not a child of the main-only culling graph.
        vsg::ref_ptr<vsg::Group> uploads;
        std::shared_ptr<PersistentGpuPopulation> persistent;
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
    // Call only from the unsealed StaticGraphFinalizer construction seam.
    // Empty, incomplete or already-sealed topology returns an explicit reason
    // and retains the exact direct graph. Compaction downgrade also reports a
    // reason even when the P3A build is active.
    //
    // The compute graph writes every VkDrawIndexedIndirectCommand field each
    // frame and never reads results back to the CPU. P3A writes one command per
    // placement. P3B can instead compact visible transforms on the GPU and emit
    // one command per draw for order-independent populations. Hi-Z remains a
    // later P3 slice.
    [[nodiscard]] GpuPopulationCullBuild enableGpuPopulationCull(
        const RenderCore::RenderWorld& world,
        const StaticPopulationPlan& plan,
        vsg::Group& graphicsRoot,
        vsg::Device& device,
        std::uint32_t indirectViewId,
        vsg::ref_ptr<vsg::vec4Array> viewData,
        bool compactCommands,
        std::string& diagnostic,
        bool retainPlacements = false);
}

#endif
