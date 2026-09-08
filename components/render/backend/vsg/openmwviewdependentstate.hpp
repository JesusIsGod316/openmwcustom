#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OPENMWVIEWDEPENDENTSTATE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_OPENMWVIEWDEPENDENTSTATE_H

#include "locallightbuffer.hpp"

#include <components/rendercore/framerenderstate.hpp>

#include <vsg/core/Array.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/state/ViewDependentState.h>

#include <array>
#include <cstddef>

namespace vsg
{
    class BufferInfo;
    class RecordTraversal;
    class ResourceRequirements;
    class View;
}

namespace RenderVsg
{
    inline constexpr std::uint32_t OpenMwLocalLightDescriptorBinding = 5;
    inline constexpr std::uint32_t OpenMwEnvironmentDescriptorBinding = 6;
    inline constexpr std::size_t OpenMwLocalLightVec4Stride = 5;
    inline constexpr std::size_t OpenMwEnvironmentVec4Count = 3;

    using OpenMwEnvironmentValues = std::array<vsg::vec4, OpenMwEnvironmentVec4Count>;

    [[nodiscard]] OpenMwEnvironmentValues packOpenMwEnvironment(const RenderCore::FrameEnvironmentState& environment,
        const RenderCore::ProjectionState& projection) noexcept;

    // Extends VSG's supported per-view descriptor lifetime instead of creating
    // a parallel descriptor binder. Ambient/directional/shadow ownership stays
    // with VSG; exact OpenMW point-light data occupies binding 5.
    class OpenMwViewDependentState final : public vsg::Inherit<vsg::ViewDependentState, OpenMwViewDependentState>
    {
    public:
        explicit OpenMwViewDependentState(vsg::View* view);

        void init(vsg::ResourceRequirements& requirements) override;
        void traverse(vsg::RecordTraversal& traversal) const override;

        [[nodiscard]] bool setLocalLights(LocalLightBufferPlan plan);
        [[nodiscard]] bool localLightsCurrent(const RenderCore::RenderWorld& world) const noexcept;
        void setRadiusFadeEnabled(bool enabled) noexcept { mRadiusFadeEnabled = enabled; }
        void setEnvironment(const RenderCore::FrameEnvironmentState& environment,
            const RenderCore::ProjectionState& projection) noexcept;

        [[nodiscard]] std::size_t localLightCount() const noexcept { return mPlan.lights.size(); }

    private:
        LocalLightBufferPlan mPlan;
        vsg::ref_ptr<vsg::vec4Array> mOpenMwLightData;
        vsg::ref_ptr<vsg::BufferInfo> mOpenMwLightBufferInfo;
        vsg::ref_ptr<vsg::vec4Array> mOpenMwEnvironmentData;
        vsg::ref_ptr<vsg::BufferInfo> mOpenMwEnvironmentBufferInfo;
        RenderCore::FrameEnvironmentState mEnvironment;
        RenderCore::ProjectionState mProjection;
        bool mRadiusFadeEnabled = true;
    };
}

#endif
