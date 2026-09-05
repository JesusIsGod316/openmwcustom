#ifndef OPENMW_COMPONENTS_RENDERCORE_FRAMERENDERSTATE_H
#define OPENMW_COMPONENTS_RENDERCORE_FRAMERENDERSTATE_H

#include "handles.hpp"
#include "math.hpp"
#include "resources.hpp"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace RenderCore
{
    struct Extent2D
    {
        std::uint32_t width = 0;
        std::uint32_t height = 0;

        [[nodiscard]] constexpr bool valid() const noexcept { return width != 0 && height != 0; }
        friend constexpr bool operator==(const Extent2D&, const Extent2D&) noexcept = default;
    };

    enum class ViewKind : std::uint8_t
    {
        Main,
        Shadow,
        Reflection,
        Refraction,
        Map,
        Preview,
        PrecipitationOcclusion,
        Debug,
    };

    struct CameraState
    {
        WorldPosition worldPosition{ 0.0, 0.0, 0.0 };
        Rotation worldOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::mat4 view{ 1.0f };
        glm::mat4 projection{ 1.0f };
    };

    struct FrameView
    {
        std::uint32_t viewIndex = 0;
        ViewKind kind = ViewKind::Main;
        CameraState current;
        CameraState previous;
        Extent2D extent;
        float lodScale = 1.0f;
        std::uint64_t semanticIncludeMask = ~std::uint64_t{ 0 };
        std::uint64_t semanticExcludeMask = 0;
        HistoryEpoch historyEpoch = InitialHistoryEpoch;
        bool temporal = false;
        bool historyValid = false;
    };

    struct DynamicTransformState
    {
        InstanceHandle instance;
        WorldTransform current;
        WorldTransform previous;
        bool historyValid = false;
    };

    struct FrameEnvironmentState
    {
        bool interior = false;
        Color ambient{ 0.0f, 0.0f, 0.0f, 1.0f };
        Color fogColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float fogStart = 0.0f;
        float fogEnd = 0.0f;
        glm::vec3 sunDirection{ 0.0f, 0.0f, -1.0f };
        Color sunDiffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color sunSpecular{ 1.0f, 1.0f, 1.0f, 1.0f };
        bool sunVisible = true;
        bool skyEnabled = true;
        bool waterEnabled = false;
        double waterHeight = 0.0;
        bool underwater = false;
    };

    struct FrameRenderStateDesc
    {
        FrameId frameId = InitialFrameId;
        WorldEpoch worldEpoch = InitialWorldEpoch;
        RenderWorldRevision renderWorldRevision = InitialRenderWorldRevision;
        HistoryEpoch historyEpoch = InitialHistoryEpoch;
        double simulationTime = 0.0;
        double frameDelta = 0.0;
        Extent2D renderExtent;
        Extent2D outputExtent;
        glm::vec2 jitter{ 0.0f, 0.0f };
        glm::vec2 projectionOffset{ 0.0f, 0.0f };
        bool historyValid = false;
        FrameEnvironmentState environment;
        std::vector<FrameView> views;
        std::vector<DynamicTransformState> dynamicTransforms;
    };

    // Immutable-by-interface snapshot. Producers assemble a FrameRenderStateDesc,
    // then move it into this object at the frame boundary. Backends receive only
    // const accessors and therefore cannot mutate simulation/render publication state.
    class FrameRenderState final
    {
    public:
        explicit FrameRenderState(FrameRenderStateDesc desc)
            : mDesc(std::move(desc))
        {
        }

        [[nodiscard]] FrameId frameId() const noexcept { return mDesc.frameId; }
        [[nodiscard]] WorldEpoch worldEpoch() const noexcept { return mDesc.worldEpoch; }
        [[nodiscard]] RenderWorldRevision renderWorldRevision() const noexcept { return mDesc.renderWorldRevision; }
        [[nodiscard]] HistoryEpoch historyEpoch() const noexcept { return mDesc.historyEpoch; }
        [[nodiscard]] double simulationTime() const noexcept { return mDesc.simulationTime; }
        [[nodiscard]] double frameDelta() const noexcept { return mDesc.frameDelta; }
        [[nodiscard]] Extent2D renderExtent() const noexcept { return mDesc.renderExtent; }
        [[nodiscard]] Extent2D outputExtent() const noexcept { return mDesc.outputExtent; }
        [[nodiscard]] const glm::vec2& jitter() const noexcept { return mDesc.jitter; }
        [[nodiscard]] const glm::vec2& projectionOffset() const noexcept { return mDesc.projectionOffset; }
        [[nodiscard]] bool historyValid() const noexcept { return mDesc.historyValid; }
        [[nodiscard]] const FrameEnvironmentState& environment() const noexcept { return mDesc.environment; }
        [[nodiscard]] const std::vector<FrameView>& views() const noexcept { return mDesc.views; }
        [[nodiscard]] const std::vector<DynamicTransformState>& dynamicTransforms() const noexcept
        {
            return mDesc.dynamicTransforms;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            if (!mDesc.frameId.valid() || !mDesc.worldEpoch.valid() || !mDesc.renderWorldRevision.valid()
                || !mDesc.historyEpoch.valid() || !mDesc.renderExtent.valid() || !mDesc.outputExtent.valid()
                || !finite(mDesc.simulationTime) || !finite(mDesc.frameDelta) || !finite(mDesc.jitter)
                || !finite(mDesc.projectionOffset) || !finite(mDesc.environment))
                return false;

            for (std::size_t i = 0; i < mDesc.views.size(); ++i)
            {
                const FrameView& view = mDesc.views[i];
                if (!view.extent.valid() || !view.historyEpoch.valid() || !finite(view.lodScale) || view.lodScale <= 0.0f
                    || !finite(view.current) || !finite(view.previous))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.views.size(); ++j)
                {
                    if (view.viewIndex == mDesc.views[j].viewIndex)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.dynamicTransforms.size(); ++i)
            {
                const DynamicTransformState& transform = mDesc.dynamicTransforms[i];
                if (!transform.instance.valid() || !finite(transform.current) || !finite(transform.previous))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.dynamicTransforms.size(); ++j)
                {
                    if (transform.instance == mDesc.dynamicTransforms[j].instance)
                        return false;
                }
            }
            return true;
        }

    private:
        [[nodiscard]] static bool finite(float value) noexcept { return std::isfinite(value); }
        [[nodiscard]] static bool finite(double value) noexcept { return std::isfinite(value); }

        [[nodiscard]] static bool finite(const glm::vec2& value) noexcept
        {
            return finite(value.x) && finite(value.y);
        }

        [[nodiscard]] static bool finite(const glm::vec3& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] static bool finite(const glm::vec4& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z) && finite(value.w);
        }

        [[nodiscard]] static bool finite(const glm::dvec3& value) noexcept
        {
            return finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] static bool finite(const glm::quat& value) noexcept
        {
            return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
        }

        [[nodiscard]] static bool finite(const glm::mat4& value) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
            {
                for (glm::length_t row = 0; row < 4; ++row)
                {
                    if (!finite(value[column][row]))
                        return false;
                }
            }
            return true;
        }

        [[nodiscard]] static bool finite(const CameraState& value) noexcept
        {
            return finite(value.worldPosition) && finite(value.worldOrientation) && finite(value.view)
                && finite(value.projection);
        }

        [[nodiscard]] static bool finite(const WorldTransform& value) noexcept
        {
            return finite(value.translation) && finite(value.rotation) && finite(value.scale);
        }

        [[nodiscard]] static bool finite(const FrameEnvironmentState& value) noexcept
        {
            return finite(value.ambient) && finite(value.fogColor) && finite(value.fogStart) && finite(value.fogEnd)
                && finite(value.sunDirection) && finite(value.sunDiffuse) && finite(value.sunSpecular)
                && finite(value.waterHeight);
        }

        FrameRenderStateDesc mDesc;
    };
}

#endif
