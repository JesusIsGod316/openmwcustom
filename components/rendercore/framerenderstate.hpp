#ifndef OPENMW_COMPONENTS_RENDERCORE_FRAMERENDERSTATE_H
#define OPENMW_COMPONENTS_RENDERCORE_FRAMERENDERSTATE_H

#include "records.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace RenderCore
{
    enum class ClipDepthRange : std::uint8_t
    {
        NegativeOneToOne,
        ZeroToOne,
    };

    enum class DepthDirection : std::uint8_t
    {
        Forward,
        Reversed,
    };

    enum class ClipYDirection : std::uint8_t
    {
        Up,
        Down,
    };

    struct ProjectionState
    {
        glm::mat4 matrix{ 1.0f };
        ClipDepthRange depthRange = ClipDepthRange::ZeroToOne;
        DepthDirection depthDirection = DepthDirection::Reversed;
        ClipYDirection yDirection = ClipYDirection::Down;
        double nearPlane = 0.1;
        double farPlane = 10000.0;
        bool infiniteFar = false;
    };

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

    enum class RenderTargetKind : std::uint8_t
    {
        Swapchain,
        Offscreen,
        History,
    };

    enum class RenderTargetFormat : std::uint8_t
    {
        SurfaceColor,
        Rgba8Srgb,
        Rgba16Float,
        Depth32Float,
    };

    struct RenderTargetDesc
    {
        RenderTargetHandle identity;
        RenderTargetKind kind = RenderTargetKind::Offscreen;
        Extent2D extent;
        RenderTargetFormat colorFormat = RenderTargetFormat::SurfaceColor;
        std::optional<RenderTargetFormat> depthFormat;
        std::uint32_t sampleCount = 1;
        HistoryEpoch historyEpoch = InitialHistoryEpoch;
        bool historyValid = false;
        bool transient = false;
    };

    enum class RenderPassLoad : std::uint8_t
    {
        Load,
        Clear,
        Discard,
    };

    enum class RenderPassStore : std::uint8_t
    {
        Store,
        Discard,
    };

    struct RenderPassDesc
    {
        RenderPassHandle identity;
        std::optional<ViewHandle> view;
        RenderTargetHandle output;
        std::vector<RenderTargetHandle> inputs;
        std::vector<RenderPassHandle> dependencies;
        RenderPassLoad colorLoad = RenderPassLoad::Clear;
        RenderPassLoad depthLoad = RenderPassLoad::Clear;
        RenderPassStore colorStore = RenderPassStore::Store;
        RenderPassStore depthStore = RenderPassStore::Store;
        bool present = false;
    };

    struct CameraState
    {
        WorldPosition worldPosition{ 0.0, 0.0, 0.0 };
        Rotation worldOrientation{ 1.0f, 0.0f, 0.0f, 0.0f };
        glm::mat4 view{ 1.0f };
        ProjectionState projection;
    };

    struct FrameView
    {
        ViewHandle identity;
        std::uint32_t viewIndex = 0;
        ViewKind kind = ViewKind::Main;
        RenderTargetHandle outputTarget;
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

    // Some auxiliary cameras and targets are derived by the backend from a
    // primary semantic view. A family keeps their ownership and limits in the
    // neutral frame contract without pretending the game owns backend-specific
    // cascade matrices or image allocations.
    struct DerivedViewFamilyDesc
    {
        ViewHandle identity;
        ViewHandle sourceView;
        ViewKind kind = ViewKind::Shadow;
        Extent2D extent;
        std::uint32_t viewCount = 0;
        float maximumDistance = 0.0f;
        std::uint64_t semanticIncludeMask = ~std::uint64_t{ 0 };
        std::uint64_t semanticExcludeMask = 0;
        bool transient = true;
    };

    struct DynamicTransformState
    {
        InstanceHandle instance;
        ResourceRevision instanceRevision;
        WorldTransform current;
        WorldTransform previous;
        bool historyValid = false;
    };

    // Evaluated actor state is published at the frame boundary instead of
    // exposing mutable OSG controller or bone nodes to the backend. Local bone
    // order is exactly SkeletonPayload::bones, allowing each backend to derive
    // current and previous skin matrices for rendering and motion vectors.
    struct SkeletonPoseState
    {
        InstanceHandle instance;
        ResourceRevision instanceRevision;
        SkeletonHandle skeleton;
        ResourceRevision skeletonRevision;
        std::vector<glm::mat4> current;
        std::vector<glm::mat4> previous;
        bool historyValid = false;
    };

    struct MorphWeightState
    {
        InstanceHandle instance;
        ResourceRevision instanceRevision;
        MeshHandle mesh;
        ResourceRevision meshRevision;
        // Compound models identify the exact geometry node. Direct mesh
        // instances leave modelNode unset.
        std::optional<ModelNodeIndex> modelNode;
        std::vector<float> current;
        std::vector<float> previous;
        bool historyValid = false;
    };

    enum class FogDistanceMode : std::uint8_t
    {
        Planar,
        Radial,
    };

    enum class FogFalloffMode : std::uint8_t
    {
        Linear,
        Exponential,
    };

    struct FrameEnvironmentState
    {
        bool interior = false;
        Color ambient{ 0.0f, 0.0f, 0.0f, 1.0f };
        Color fogColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float fogStart = 0.0f;
        float fogEnd = 0.0f;
        FogDistanceMode fogDistanceMode = FogDistanceMode::Planar;
        FogFalloffMode fogFalloffMode = FogFalloffMode::Linear;
        bool fogEnabled = false;
        glm::vec3 sunDirection{ 0.0f, 0.0f, -1.0f };
        Color sunDiffuse{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color sunSpecular{ 1.0f, 1.0f, 1.0f, 1.0f };
        Color sunDiscColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        // Lighting and visible solar-disc/glare state are distinct. Interiors
        // retain authored directional lighting while hiding the sun itself.
        bool sunLightEnabled = true;
        bool sunVisible = true;
        // Shadow-map resources are fixed when the backend is constructed, but
        // outdoor/interior policy remains frame-varying across transitions.
        bool shadowsEnabled = false;
        // Matches the OpenMW shader choice: non-classic and clustered lighting
        // fade point lights over the final quarter of their effective radius.
        bool localLightRadiusFade = true;
        // Kept distinct from radius fading: clustered mode also owns tile/light
        // selection and far-plane fading, so a backend must not silently treat
        // it as the unclustered loop merely because their radius curves match.
        bool clusteredLocalLighting = false;
        // CP4D weather values are frame-varying semantic inputs. Sky/cloud
        // textures and geometry remain persistent resources rather than being
        // copied into every frame snapshot.
        Color skyColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        float nightSkyFactor = 0.0f;
        float cloudBlendFactor = 0.0f;
        float cloudSpeed = 0.0f;
        glm::vec3 windDirection{ 0.0f, 1.0f, 0.0f };
        float windSpeed = 0.0f;
        float precipitationIntensity = 0.0f;
        bool precipitationEnabled = false;
        bool storm = false;
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
        std::vector<RenderTargetDesc> renderTargets;
        std::vector<RenderPassDesc> renderPasses;
        std::vector<FrameView> views;
        std::vector<DerivedViewFamilyDesc> derivedViewFamilies;
        std::vector<DynamicTransformState> dynamicTransforms;
        std::vector<SkeletonPoseState> skeletonPoses;
        std::vector<MorphWeightState> morphWeights;
        std::vector<DynamicMaterialState> dynamicMaterials;
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
        [[nodiscard]] const std::vector<RenderTargetDesc>& renderTargets() const noexcept
        {
            return mDesc.renderTargets;
        }
        [[nodiscard]] const std::vector<RenderPassDesc>& renderPasses() const noexcept { return mDesc.renderPasses; }
        [[nodiscard]] const std::vector<FrameView>& views() const noexcept { return mDesc.views; }
        [[nodiscard]] const std::vector<DerivedViewFamilyDesc>& derivedViewFamilies() const noexcept
        {
            return mDesc.derivedViewFamilies;
        }
        [[nodiscard]] const std::vector<DynamicTransformState>& dynamicTransforms() const noexcept
        {
            return mDesc.dynamicTransforms;
        }
        [[nodiscard]] const std::vector<SkeletonPoseState>& skeletonPoses() const noexcept
        {
            return mDesc.skeletonPoses;
        }
        [[nodiscard]] const std::vector<MorphWeightState>& morphWeights() const noexcept { return mDesc.morphWeights; }
        [[nodiscard]] const std::vector<DynamicMaterialState>& dynamicMaterials() const noexcept
        {
            return mDesc.dynamicMaterials;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            if (!mDesc.frameId.valid() || !mDesc.worldEpoch.valid() || !mDesc.renderWorldRevision.valid()
                || !mDesc.historyEpoch.valid() || !mDesc.renderExtent.valid() || !mDesc.outputExtent.valid()
                || !finite(mDesc.simulationTime) || !finite(mDesc.frameDelta) || !finite(mDesc.jitter)
                || !finite(mDesc.projectionOffset) || !finite(mDesc.environment))
                return false;

            for (std::size_t i = 0; i < mDesc.renderTargets.size(); ++i)
            {
                const RenderTargetDesc& target = mDesc.renderTargets[i];
                if (!target.identity.valid() || !target.extent.valid() || target.sampleCount == 0
                    || !target.historyEpoch.valid() || (target.historyValid && !mDesc.historyValid)
                    || !known(target.kind) || !known(target.colorFormat)
                    || (target.depthFormat && !known(*target.depthFormat))
                    || (target.kind == RenderTargetKind::Swapchain && target.transient))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.renderTargets.size(); ++j)
                {
                    if (target.identity == mDesc.renderTargets[j].identity)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.views.size(); ++i)
            {
                const FrameView& view = mDesc.views[i];
                if (!view.identity.valid() || !view.outputTarget.valid() || !findTarget(view.outputTarget)
                    || !view.extent.valid() || !view.historyEpoch.valid() || !finite(view.lodScale)
                    || view.lodScale <= 0.0f || !finite(view.current) || !finite(view.previous))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.views.size(); ++j)
                {
                    if (view.viewIndex == mDesc.views[j].viewIndex || view.identity == mDesc.views[j].identity)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.derivedViewFamilies.size(); ++i)
            {
                const DerivedViewFamilyDesc& family = mDesc.derivedViewFamilies[i];
                if (!family.identity.valid() || !family.sourceView.valid() || !findView(family.sourceView)
                    || family.kind == ViewKind::Main || !known(family.kind) || !family.extent.valid()
                    || family.viewCount == 0 || family.viewCount > 32 || !finite(family.maximumDistance)
                    || family.maximumDistance <= 0.0f
                    || (family.semanticIncludeMask & family.semanticExcludeMask) != 0)
                    return false;
                for (std::size_t j = i + 1; j < mDesc.derivedViewFamilies.size(); ++j)
                {
                    if (family.identity == mDesc.derivedViewFamilies[j].identity)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.renderPasses.size(); ++i)
            {
                const RenderPassDesc& pass = mDesc.renderPasses[i];
                const RenderTargetDesc* output = findTarget(pass.output);
                if (!pass.identity.valid() || !output || (pass.view && !findView(*pass.view)) || !known(pass.colorLoad)
                    || !known(pass.depthLoad) || !known(pass.colorStore) || !known(pass.depthStore)
                    || (pass.present && output->kind != RenderTargetKind::Swapchain))
                    return false;
                for (std::size_t inputIndex = 0; inputIndex < pass.inputs.size(); ++inputIndex)
                {
                    const RenderTargetHandle input = pass.inputs[inputIndex];
                    if (!findTarget(input) || input == pass.output
                        || std::find(pass.inputs.begin() + inputIndex + 1, pass.inputs.end(), input)
                            != pass.inputs.end())
                        return false;
                }
                for (std::size_t dependencyIndex = 0; dependencyIndex < pass.dependencies.size(); ++dependencyIndex)
                {
                    const RenderPassHandle dependency = pass.dependencies[dependencyIndex];
                    if (dependency == pass.identity || !findEarlierPass(dependency, i))
                        return false;
                    if (std::find(pass.dependencies.begin() + dependencyIndex + 1, pass.dependencies.end(), dependency)
                        != pass.dependencies.end())
                        return false;
                }
                for (std::size_t j = i + 1; j < mDesc.renderPasses.size(); ++j)
                    if (pass.identity == mDesc.renderPasses[j].identity)
                        return false;
            }

            for (std::size_t i = 0; i < mDesc.dynamicTransforms.size(); ++i)
            {
                const DynamicTransformState& transform = mDesc.dynamicTransforms[i];
                if (!transform.instance.valid() || !transform.instanceRevision.valid() || !finite(transform.current)
                    || !finite(transform.previous))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.dynamicTransforms.size(); ++j)
                {
                    if (transform.instance == mDesc.dynamicTransforms[j].instance)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.skeletonPoses.size(); ++i)
            {
                const SkeletonPoseState& pose = mDesc.skeletonPoses[i];
                if (!pose.instance.valid() || !pose.instanceRevision.valid() || !pose.skeleton.valid()
                    || !pose.skeletonRevision.valid() || pose.current.empty()
                    || pose.current.size() != pose.previous.size() || (pose.historyValid && !mDesc.historyValid))
                    return false;
                for (std::size_t bone = 0; bone < pose.current.size(); ++bone)
                {
                    if (!finite(pose.current[bone]) || !finite(pose.previous[bone]))
                        return false;
                }
                for (std::size_t j = i + 1; j < mDesc.skeletonPoses.size(); ++j)
                {
                    if (pose.instance == mDesc.skeletonPoses[j].instance)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.morphWeights.size(); ++i)
            {
                const MorphWeightState& morph = mDesc.morphWeights[i];
                if (!morph.instance.valid() || !morph.instanceRevision.valid() || !morph.mesh.valid()
                    || !morph.meshRevision.valid() || morph.current.empty()
                    || morph.current.size() != morph.previous.size() || (morph.historyValid && !mDesc.historyValid))
                    return false;
                for (std::size_t target = 0; target < morph.current.size(); ++target)
                {
                    if (!finite(morph.current[target]) || !finite(morph.previous[target]))
                        return false;
                }
                for (std::size_t j = i + 1; j < mDesc.morphWeights.size(); ++j)
                {
                    const MorphWeightState& other = mDesc.morphWeights[j];
                    if (morph.instance == other.instance && morph.modelNode == other.modelNode)
                        return false;
                }
            }

            for (std::size_t i = 0; i < mDesc.dynamicMaterials.size(); ++i)
            {
                const DynamicMaterialState& material = mDesc.dynamicMaterials[i];
                if (!material.material.valid() || !finite(material))
                    return false;
                for (std::size_t j = i + 1; j < mDesc.dynamicMaterials.size(); ++j)
                {
                    if (material.material == mDesc.dynamicMaterials[j].material)
                        return false;
                }
                for (std::size_t binding = 0; binding < material.textureTransforms.size(); ++binding)
                {
                    for (std::size_t other = binding + 1; other < material.textureTransforms.size(); ++other)
                    {
                        if (material.textureTransforms[binding].bindingIndex
                            == material.textureTransforms[other].bindingIndex)
                            return false;
                    }
                }
            }
            return true;
        }

    private:
        [[nodiscard]] static bool known(RenderTargetKind value) noexcept
        {
            return value == RenderTargetKind::Swapchain || value == RenderTargetKind::Offscreen
                || value == RenderTargetKind::History;
        }

        [[nodiscard]] static bool known(ViewKind value) noexcept
        {
            return value == ViewKind::Main || value == ViewKind::Shadow || value == ViewKind::Reflection
                || value == ViewKind::Refraction || value == ViewKind::Map || value == ViewKind::Preview
                || value == ViewKind::PrecipitationOcclusion || value == ViewKind::Debug;
        }

        [[nodiscard]] static bool known(RenderTargetFormat value) noexcept
        {
            return value == RenderTargetFormat::SurfaceColor || value == RenderTargetFormat::Rgba8Srgb
                || value == RenderTargetFormat::Rgba16Float || value == RenderTargetFormat::Depth32Float;
        }

        [[nodiscard]] static bool known(RenderPassLoad value) noexcept
        {
            return value == RenderPassLoad::Load || value == RenderPassLoad::Clear || value == RenderPassLoad::Discard;
        }

        [[nodiscard]] static bool known(RenderPassStore value) noexcept
        {
            return value == RenderPassStore::Store || value == RenderPassStore::Discard;
        }

        [[nodiscard]] const RenderTargetDesc* findTarget(RenderTargetHandle identity) const noexcept
        {
            const auto found = std::find_if(mDesc.renderTargets.begin(), mDesc.renderTargets.end(),
                [identity](const RenderTargetDesc& target) { return target.identity == identity; });
            return found == mDesc.renderTargets.end() ? nullptr : &*found;
        }

        [[nodiscard]] const FrameView* findView(ViewHandle identity) const noexcept
        {
            const auto found = std::find_if(mDesc.views.begin(), mDesc.views.end(),
                [identity](const FrameView& view) { return view.identity == identity; });
            return found == mDesc.views.end() ? nullptr : &*found;
        }

        [[nodiscard]] bool findEarlierPass(RenderPassHandle identity, std::size_t before) const noexcept
        {
            return std::find_if(mDesc.renderPasses.begin(), mDesc.renderPasses.begin() + before,
                       [identity](const RenderPassDesc& pass) { return pass.identity == identity; })
                != mDesc.renderPasses.begin() + before;
        }

        [[nodiscard]] static bool finite(float value) noexcept { return std::isfinite(value); }
        [[nodiscard]] static bool finite(double value) noexcept { return std::isfinite(value); }

        [[nodiscard]] static bool finite(const glm::vec2& value) noexcept { return finite(value.x) && finite(value.y); }

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

        [[nodiscard]] static bool finite(const LocalTransform& value) noexcept
        {
            return finite(value.translation) && finite(value.rotation) && finite(value.scale);
        }

        [[nodiscard]] static bool finite(const WorldTransform& value) noexcept
        {
            return finite(value.translation) && finite(value.rotation) && finite(value.scale);
        }

        [[nodiscard]] static bool finite(const TextureTransform& value) noexcept
        {
            return finite(value.offset) && finite(value.scale) && finite(value.center) && finite(value.rotation);
        }

        [[nodiscard]] static bool finite(const CameraState& value) noexcept
        {
            return finite(value.worldPosition) && finite(value.worldOrientation) && finite(value.view)
                && finite(value.projection);
        }

        [[nodiscard]] static bool finite(const ProjectionState& value) noexcept
        {
            return finite(value.matrix) && finite(value.nearPlane) && finite(value.farPlane) && value.nearPlane > 0.0
                && (value.infiniteFar || value.farPlane > value.nearPlane)
                && (value.depthRange == ClipDepthRange::NegativeOneToOne
                    || value.depthRange == ClipDepthRange::ZeroToOne)
                && (value.depthDirection == DepthDirection::Forward || value.depthDirection == DepthDirection::Reversed)
                && (value.yDirection == ClipYDirection::Up || value.yDirection == ClipYDirection::Down);
        }

        [[nodiscard]] static bool finite(const FrameEnvironmentState& value) noexcept
        {
            const bool fogModesValid
                = (value.fogDistanceMode == FogDistanceMode::Planar || value.fogDistanceMode == FogDistanceMode::Radial)
                && (value.fogFalloffMode == FogFalloffMode::Linear
                    || value.fogFalloffMode == FogFalloffMode::Exponential);
            const bool fogRangeValid = !value.fogEnabled || (value.fogEnd > value.fogStart && value.fogEnd > 0.0f);
            return finite(value.ambient) && finite(value.fogColor) && finite(value.fogStart) && finite(value.fogEnd)
                && fogModesValid && fogRangeValid && finite(value.sunDirection) && finite(value.sunDiffuse)
                && finite(value.sunSpecular) && finite(value.sunDiscColor) && finite(value.skyColor)
                && finite(value.nightSkyFactor)
                && value.nightSkyFactor >= 0.0f && value.nightSkyFactor <= 1.0f && finite(value.cloudBlendFactor)
                && value.cloudBlendFactor >= 0.0f && value.cloudBlendFactor <= 1.0f && finite(value.cloudSpeed)
                && finite(value.windDirection) && finite(value.windSpeed) && value.windSpeed >= 0.0f
                && finite(value.precipitationIntensity) && value.precipitationIntensity >= 0.0f
                && value.precipitationIntensity <= 1.0f && finite(value.waterHeight);
        }

        [[nodiscard]] static bool finite(const DynamicMaterialState& value) noexcept
        {
            if ((value.diffuse && !finite(*value.diffuse)) || (value.ambient && !finite(*value.ambient))
                || (value.specular && !finite(*value.specular)) || (value.emission && !finite(*value.emission))
                || (value.alpha && !finite(*value.alpha))
                || (value.emissiveMultiplier && !finite(*value.emissiveMultiplier)))
                return false;
            for (const DynamicTextureTransformState& transform : value.textureTransforms)
            {
                if (!finite(transform.transform))
                    return false;
            }
            return true;
        }

        FrameRenderStateDesc mDesc;
    };
}

#endif
