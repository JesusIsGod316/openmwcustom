#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEHOST_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_VSGRUNTIMEHOST_H

#include "framecamera.hpp"
#include "framecompletion.hpp"
#include "openmwviewdependentstate.hpp"
#include "offscreenrendertarget.hpp"
#include "sdlvulkanwindow.hpp"
#include "skybackdrop.hpp"
#include "staticassetrealizer.hpp"
#include "staticpopulationresidency.hpp"
#include "staticworldresidency.hpp"
#include "vsgsubmission.hpp"
#include "uipipeline.hpp"
#include "watersurface.hpp"

#include <components/rendercore/renderer.hpp>

#include <vsg/core/ref_ptr.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
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
        void waitIdle() override;

        [[nodiscard]] std::size_t residentStaticInstanceCount() const noexcept;
        [[nodiscard]] std::size_t pendingRetirementCount() const noexcept;
        [[nodiscard]] const UiPipeline& uiPipeline() const noexcept { return mUiPipeline; }
        void attachGuiRenderer(VsgMyGui::RenderManager* renderer) noexcept;
        void detachGuiRenderer(const VsgMyGui::RenderManager* renderer) noexcept;
        [[nodiscard]] VsgMyGui::RenderManager* guiRenderer() noexcept { return mGuiRenderer; }
        [[nodiscard]] vsg::ref_ptr<vsg::ImageView> auxiliaryColorImage(
            RenderCore::RenderTargetHandle target) const noexcept;
        // Map persistence is intentionally a rare, explicit readback rather than
        // a per-frame synchronization point. All requested RGBA8 targets are
        // transitioned, copied and restored in one graphics-queue submission.
        [[nodiscard]] std::optional<std::vector<AuxiliaryRgba8Readback>> readbackAuxiliaryRgba8(
            std::span<const RenderCore::RenderTargetHandle> targets);
        // Explicit retirement is required for long-lived logical map slots: a
        // sampled target stays native to VSG until its producer retires that
        // logical surface. Retirement synchronizes before removing the command
        // graph so no in-flight submission can retain freed target resources.
        [[nodiscard]] bool retireAuxiliarySurface(RenderCore::RenderTargetHandle target);
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        using StaticResident = vsg::ref_ptr<vsg::Node>;
        struct StaticPopulationResident
        {
            vsg::ref_ptr<vsg::Switch> visibility;
        };
        struct WaterViewRuntime
        {
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
            FrameCameraObjects camera;
            vsg::ref_ptr<vsg::View> view;
            vsg::ref_ptr<OpenMwViewDependentState> state;
            bool active = false;
        };

        [[nodiscard]] bool synchronizeStaticWorld(const RenderCore::RenderWorld& world);
        [[nodiscard]] bool synchronizePopulationVisibility(
            const RenderCore::RenderWorld& world, const RenderCore::FrameView& mainView);
        [[nodiscard]] bool synchronizeDynamicActors(
            const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame);
        [[nodiscard]] bool synchronizeLocalLights(const RenderCore::RenderWorld& world);
        [[nodiscard]] bool synchronizeAuxiliaryViews(const RenderCore::FrameRenderState& frame);
        [[nodiscard]] bool synchronizeGui();
        [[nodiscard]] const RenderCore::FrameView* selectMainView(
            const RenderCore::FrameRenderState& frame) const noexcept;
        [[nodiscard]] bool shadowViewFamilyCompatible(
            const RenderCore::FrameRenderState& frame) const noexcept;
        [[nodiscard]] bool waterViewsCompatible(const RenderCore::FrameRenderState& frame,
            const RenderCore::FrameView*& reflection, const RenderCore::FrameView*& refraction) const noexcept;
        RenderCore::RenderFrameResult finish(
            RenderCore::RenderFrameResult result, std::string diagnostic = {});

        VsgRuntimeHostOptions mOptions;
        StaticTextureResolver mTextureResolver;
        vsg::ref_ptr<SdlVulkanWindow> mWindow;
        vsg::ref_ptr<vsg::SharedObjects> mSharedObjects;
        vsg::ref_ptr<vsg::Viewer> mViewer;
        vsg::ref_ptr<vsg::Group> mSceneRoot;
        vsg::ref_ptr<vsg::Group> mStaticRoot;
        vsg::ref_ptr<vsg::Group> mDynamicRoot;
        vsg::ref_ptr<vsg::Group> mGuiRoot;
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
        WaterSurface mWaterSurface;
        FrameCameraObjects mCamera;
        StaticWorldResidency<StaticResident> mStaticResidency;
        StaticPopulationResidency<StaticPopulationResident> mStaticPopulationResidency;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Group>> mDynamicRetirements;
        FrameRetirementQueue<vsg::ref_ptr<vsg::Group>> mGuiRetirements;
        std::optional<RenderCore::FrameId> mDynamicLastUse;
        std::optional<RenderCore::FrameId> mGuiLastUse;
        UiPipeline mUiPipeline;
        VsgMyGui::RenderManager* mGuiRenderer = nullptr;
        VsgSubmissionCompletion mCompletion;
        std::string mLastDiagnostic;
        bool mWaitedIdle = false;
    };
}

#endif