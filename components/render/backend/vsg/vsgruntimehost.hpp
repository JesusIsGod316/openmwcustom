#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEHOST_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEHOST_H

#include "framecamera.hpp"
#include <components/debug/runtimediagnostics.hpp>
#include "dynamicactorplan.hpp"
#include "framecompletion.hpp"
#include "frameresourcepool.hpp"
#include "immediateeffectrealizer.hpp"
#include "openmwviewdependentstate.hpp"
#include "offscreenrendertarget.hpp"
#include "sdlvulkanwindow.hpp"
#include "skybackdrop.hpp"
#include "nativesky.hpp"
#include "staticassetrealizer.hpp"
#include "staticpopulationresidency.hpp"
#include "staticworldresidency.hpp"
#include "staticworldsyncstate.hpp"
#include "vsgsubmission.hpp"
#include "viewcompilemanager.hpp"
#include "uipipeline.hpp"
#include "watersurface.hpp"

#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/rendercore/renderer.hpp>

#include <vsg/core/ref_ptr.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace vsg
{
    class Node;
    class Group;
    class AmbientLight;
    class CommandGraph;
    class DirectionalLight;
    class ImageView;
    class MatrixTransform;
    class RenderGraph;
    class SharedObjects;
    class Switch;
    class View;
    class Viewer;
}

namespace VsgMyGui
{
    class RenderManager;
}

namespace RenderVsg
{
    struct VsgShadowOptions
    {
        bool enabled = false;
        std::uint32_t cascadeCount = 1;
        std::uint32_t mapResolution = 2048;
        double maximumDistance = 1e8;
        double depthBias = 0.005;
        double splitLambda = 0.5;
        bool actorCasters = true;
        bool terrainCasters = true;
        bool objectCasters = true;
    };

    struct VsgRuntimeHostOptions
    {
        std::size_t maximumFramesInFlight = VsgRecordAndSubmitRingSize;
        StaticPlanOptions staticPlan;
        VsgShadowOptions shadows;
        struct Water
        {
            bool enabled = false;
            bool reflection = true;
            bool refraction = true;
            std::uint32_t targetSize = 512;
            float reflectionLodScale = 0.5f;
            float refractionLodScale = 0.5f;
            vsg::ref_ptr<vsg::Data> normalMap;
        } water;
    };

    // Production-shaped CP3C host for one SDL-owned swapchain and one semantic
    // main view. CP4F extends the same command graph with bounded persistent
    // auxiliary map/preview/debug targets while keeping all backend objects
    // private to RenderVsg. Engine selection remains separate so the established
    // OpenGL host stays intact until all required compatibility facets are implemented.
    class VsgRuntimeHost final : public RenderCore::SemanticRenderer
    {
    public:
        struct AuxiliaryRgba8Readback
        {
            RenderCore::RenderTargetHandle target;
            RenderCore::Extent2D extent;
            std::vector<std::uint8_t> rgba;
        };

        VsgRuntimeHost(vsg::ref_ptr<SdlVulkanWindow> window, StaticTextureResolver textureResolver,
            VsgRuntimeHostOptions options = {});
        ~VsgRuntimeHost() override;

        [[nodiscard]] RenderCore::RenderBackendKind backendKind() const noexcept override;
        RenderCore::RenderFrameResult renderFrame(
            const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame) override;
        RenderCore::RenderFrameResult renderGuiFrame(
            const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame);
        void waitIdle() override;

        [[nodiscard]] bool configureNamedSwitchState(
            RenderCore::NightDaySwitchState state, bool dayNightSwitchesEnabled) noexcept
        {
            if (!RenderCore::validNightDaySwitchState(state))
                return false;
            mOptions.staticPlan.nightDaySwitchState = state;
            mOptions.staticPlan.dayNightSwitchesEnabled = dayNightSwitchesEnabled;
            return true;
        }

        [[nodiscard]] std::size_t residentStaticInstanceCount() const noexcept;
        [[nodiscard]] std::size_t pendingRetirementCount() const noexcept;
        [[nodiscard]] std::size_t lastCompiledDynamicRootCount() const noexcept
        {
            return mLastCompiledDynamicRootCount;
        }
        [[nodiscard]] const UiPipeline& uiPipeline() const noexcept { return mUiPipeline; }
        void attachGuiRenderer(VsgMyGui::RenderManager* renderer) noexcept;
        void detachGuiRenderer(const VsgMyGui::RenderManager* renderer) noexcept;
        [[nodiscard]] VsgMyGui::RenderManager* guiRenderer() noexcept { return mGuiRenderer; }
        // Snapshot and compile mutable MyGUI state before the application releases
        // its Lua worker. renderFrame() only publishes this immutable generation.
        [[nodiscard]] bool prepareGui();
        [[nodiscard]] vsg::ref_ptr<vsg::ImageView> auxiliaryColorImage(
            RenderCore::RenderTargetHandle target) const noexcept;
        [[nodiscard]] std::optional<std::vector<AuxiliaryRgba8Readback>> readbackAuxiliaryRgba8(
            std::span<const RenderCore::RenderTargetHandle> targets);
        [[nodiscard]] bool retireAuxiliarySurface(RenderCore::RenderTargetHandle target);
        [[nodiscard]] std::size_t compilationContextCount()
        {
            return static_cast<ViewCompileManager&>(*mViewer->compileManager).contextCount();
        }
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        RenderCore::RenderFrameResult renderFrameImpl(
            const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame, bool guiOnly);
        using StaticResident = vsg::ref_ptr<vsg::Node>;
        struct StaticPopulationResident
        {
            vsg::ref_ptr<vsg::Switch> visibility;
        };
        struct WaterViewRuntime
        {
            std::unique_ptr<NativeSky> nativeSky;
            OffscreenRenderTarget target;
            FrameCameraObjects camera;
            vsg::ref_ptr<vsg::View> view;
            vsg::ref_ptr<OpenMwViewDependentState> state;
        };
        struct AuxiliaryViewRuntime
        {
            RenderCore::ViewHandle identity;
            RenderCore::RenderTargetHandle targetIdentity;
            RenderCore::ViewKind kind = RenderCore::ViewKind::Preview;
            RenderCore::RenderTargetFormat colorFormat = RenderCore::RenderTargetFormat::Rgba8Srgb;
            std::optional<RenderCore::RenderTargetFormat> depthFormat = RenderCore::RenderTargetFormat::Depth32Float;
            OffscreenRenderTarget target;
            vsg::ref_ptr<vsg::Switch> commandVisibility;
            FrameCameraObjects camera;
            vsg::ref_ptr<vsg::View> view;
            vsg::ref_ptr<OpenMwViewDependentState> state;
            vsg::ref_ptr<vsg::AmbientLight> ambientLight;
            vsg::ref_ptr<vsg::DirectionalLight> sunLight;
            WaterSurface waterSurface;
            std::unique_ptr<ViewCompileManager::Registration> compilation;
            bool active = false;
            bool isolated = false;
            std::uint64_t sceneRevision = 0;
            vsg::ref_ptr<vsg::Group> isolatedHolder;
            vsg::ref_ptr<vsg::Group> isolatedPublished;
        };
        struct DynamicActorResident
        {
            RenderCore::WorldEpoch epoch;
            std::optional<DynamicActorPlan> contract;
            float opacity = 1.f;
            vsg::ref_ptr<vsg::MatrixTransform> placement;
            vsg::ref_ptr<vsg::Node> published;
            std::vector<StaticRealizationResult::MutableDrawStreams> mutableDraws;
        };
        struct ImmediateEffectResident
        {
            RenderCore::ImmediateEffectDraw contract;
            vsg::ref_ptr<vsg::MatrixTransform> placement;
            vsg::ref_ptr<vsg::Node> published;
            std::vector<StaticRealizationResult::MutableDrawStreams> mutableDraws;
        };

        [[nodiscard]] bool synchronizeStaticWorld(const RenderCore::RenderWorld& world);
        [[nodiscard]] bool synchronizePopulationVisibility(
            const RenderCore::RenderWorld& world, const RenderCore::FrameView& mainView);
        [[nodiscard]] bool synchronizeDynamicActors(
            const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame);
        [[nodiscard]] bool synchronizeLocalLights(const RenderCore::RenderWorld& world);
        [[nodiscard]] bool synchronizeAuxiliaryViews(const RenderCore::FrameRenderState& frame);
        [[nodiscard]] bool synchronizeGui();
        [[nodiscard]] bool ensureActiveGraphicsPipelinesRealized();
        void reportStrictFrameDiagnostics(
            const RenderCore::FrameRenderState& frame, const RenderCore::FrameView& mainView);
        [[nodiscard]] const RenderCore::FrameView* selectMainView(
            const RenderCore::FrameRenderState& frame) const noexcept;
        [[nodiscard]] bool shadowViewFamilyCompatible(
            const RenderCore::FrameRenderState& frame) const noexcept;
        [[nodiscard]] bool waterViewsCompatible(const RenderCore::FrameRenderState& frame,
            const RenderCore::FrameView*& reflection, const RenderCore::FrameView*& refraction) const noexcept;
        RenderCore::RenderFrameResult finish(
            RenderCore::RenderFrameResult result, std::string diagnostic = {});

        void reportRuntimeMemory(const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame) noexcept;
        Debug::RuntimeDiagnostics::Sampler mRuntimeDiagnosticSampler;
        std::array<std::uint64_t, static_cast<std::size_t>(ImmediateEffectMismatch::Count)> mEffectRebuildReasons{};
        unsigned mEffectDiagnosticExamples = 8;
        std::uint64_t mDiagnosticReleasedDynamic = 0, mDiagnosticReleasedGui = 0;
        VsgRuntimeHostOptions mOptions;
        std::size_t mLastCompiledDynamicRootCount = 0;
        StaticTextureResolver mTextureResolver;
        vsg::ref_ptr<SdlVulkanWindow> mWindow;
        vsg::ref_ptr<vsg::SharedObjects> mSharedObjects;
        vsg::ref_ptr<vsg::Viewer> mViewer;
        vsg::ref_ptr<vsg::Group> mSceneRoot;
        vsg::ref_ptr<vsg::Switch> mSceneVisibility;
        vsg::ref_ptr<vsg::Group> mStaticRoot;
        // Stable holder attached to the shared scene plus separately-owned published
        // dynamic content. Replacement never mutates mSceneRoot child ordering and
        // the old generation stays strongly owned through frame-safe retirement.
        vsg::ref_ptr<vsg::Group> mDynamicRoot;
        vsg::ref_ptr<vsg::Group> mDynamicPublishedRoot;
        // Stable holder attached to the main view plus separately-owned published
        // content. Replacing GUI content never relies on child ordering in mMainOnlyRoot.
        vsg::ref_ptr<vsg::Group> mGuiRoot;
        vsg::ref_ptr<vsg::Group> mGuiPublishedRoot;
        vsg::ref_ptr<vsg::Group> mGuiPreparedRoot;
        vsg::ref_ptr<vsg::Group> mMainOnlyRoot;
        vsg::ref_ptr<vsg::View> mView;
        vsg::ref_ptr<OpenMwViewDependentState> mOpenMwViewState;
        vsg::ref_ptr<vsg::RenderGraph> mRenderGraph;
        vsg::ref_ptr<vsg::CommandGraph> mCommandGraph;
        std::optional<WaterViewRuntime> mReflectionView;
        std::optional<WaterViewRuntime> mRefractionView;
        std::vector<AuxiliaryViewRuntime> mAuxiliaryViews;
        vsg::ref_ptr<vsg::AmbientLight> mAmbientLight;
        vsg::ref_ptr<vsg::DirectionalLight> mSunLight;
        SkyBackdrop mSkyBackdrop;
        NativeSky mNativeSky;
        WaterSurface mWaterSurface;
        FrameCameraObjects mCamera;
        StaticWorldResidency<StaticResident> mStaticResidency;
        StaticWorldSyncState mStaticSyncState;
        DynamicActorPlanCache mActorPlanCache;
        StaticPopulationResidency<StaticPopulationResident> mStaticPopulationResidency;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Group>> mDynamicRetirements;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Group>> mGuiRetirements;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Group>> mIsolatedSceneRetirements;
        FrameRetirementQueue<AuxiliaryViewRuntime> mAuxiliaryRetirements;
        std::optional<RenderCore::FrameId> mDynamicLastUse;
        std::optional<RenderCore::FrameId> mGuiLastUse;
        std::optional<RenderCore::FrameId> mCompletedThrough;
        FrameResourcePool<ImmediateEffectResident> mImmediateEffectResidents{ VsgRecordAndSubmitRingSize };
        FrameResourcePool<DynamicActorResident> mDynamicActorResidents{ VsgRecordAndSubmitRingSize };
        UiPipeline mUiPipeline;
        VsgMyGui::RenderManager* mGuiRenderer = nullptr;
        VsgSubmissionCompletion mCompletion;
        std::string mLastDiagnostic;
        std::optional<std::uint64_t> mStrictQcLastFrameSignature;
        std::optional<std::uint64_t> mStrictQcLastViewSignature;
        std::uint32_t mStrictQcInitialFramesReported = 0;
        bool mGuiPrepared = false;
        bool mWaitedIdle = false;
    };
}

#endif
