#include "vsgruntimehost.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

#include "dynamicactorplan.hpp"
#include "allocationdiagnostics.hpp"
#include <bit>
#include "runtimememorydiagnostics.hpp"
#include "immediateeffectrealizer.hpp"
#include "legacymaterialshader.hpp"
#include "populationvisibility.hpp"
#include "staticassetconformance.hpp"

#include <components/vsgmygui/rendermanager.hpp>

#include <components/debug/debuglog.hpp>
#include <components/rendercore/deformation.hpp>

#include <vsg/app/CommandGraph.h>
#include <vsg/io/DatabasePager.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/lighting/HardShadows.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/nodes/Switch.h>
#include <vsg/state/ResourceHints.h>
#include <vsg/utils/SharedObjects.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] RenderCore::FrameView initialView()
        {
            RenderCore::FrameView result;
            result.identity = RenderCore::ViewHandle::fromParts(0, 1);
            result.outputTarget = RenderCore::RenderTargetHandle::fromParts(0, 1);
            result.extent = { 1, 1 };
            return result;
        }

        [[nodiscard]] bool vsgProjectionCompatible(const RenderCore::ProjectionState& projection) noexcept
        {
            return projection.depthRange == RenderCore::ClipDepthRange::ZeroToOne
                && projection.depthDirection == RenderCore::DepthDirection::Reversed
                && projection.yDirection == RenderCore::ClipYDirection::Down;
        }

        constexpr vsg::Mask ShadowTraversalMask = 0x1;
        constexpr vsg::Mask ReflectionTraversalMask = 0x2;
        constexpr vsg::Mask RefractionTraversalMask = 0x4;
        constexpr std::size_t MaximumAuxiliaryViews = 64;
        constexpr std::uint64_t MaximumAuxiliaryPixels = 64ull * 512ull * 512ull;

        [[nodiscard]] bool hasSemanticFlag(
            std::uint64_t flags, RenderCore::InstanceSemanticFlag flag) noexcept
        {
            return (flags & RenderCore::semanticFlag(flag)) != 0;
        }

        [[nodiscard]] bool isGenericAuxiliaryView(RenderCore::ViewKind kind) noexcept
        {
            return kind == RenderCore::ViewKind::Map || kind == RenderCore::ViewKind::Preview
                || kind == RenderCore::ViewKind::Debug;
        }

        [[nodiscard]] vsg::Mask placementMask(bool castsShadow, std::uint64_t semanticFlags) noexcept
        {
            vsg::Mask result = castsShadow ? vsg::MASK_ALL : (vsg::MASK_ALL & ~ShadowTraversalMask);
            if (!hasSemanticFlag(semanticFlags, RenderCore::InstanceSemanticFlag::ReflectionEligible))
                result &= ~ReflectionTraversalMask;
            if (!hasSemanticFlag(semanticFlags, RenderCore::InstanceSemanticFlag::RefractionEligible))
                result &= ~RefractionTraversalMask;
            return result;
        }

        [[nodiscard]] vsg::ref_ptr<vsg::Switch> maskedNode(
            vsg::Mask mask, vsg::ref_ptr<vsg::Node> child)
        {
            auto result = vsg::Switch::create();
            result->addChild(mask, std::move(child));
            return result;
        }

        [[nodiscard]] std::string compileFailureDiagnostic(std::string_view subject, const vsg::CompileResult& result)
        {
            std::string diagnostic(subject);
            diagnostic += " compilation failed before scene publication (result " + std::to_string(result.result)
                + ")";
            if (!result.message.empty())
                diagnostic += ": " + result.message;
            return diagnostic;
        }

        [[nodiscard]] bool strictQcEnabled() noexcept
        {
            const char* value = std::getenv("OPENMW_V4_STRICT_QC");
            return value && value[0] != '\0' && value[0] != '0';
        }

        [[nodiscard]] std::string_view viewKindName(RenderCore::ViewKind kind) noexcept
        {
            switch (kind)
            {
                case RenderCore::ViewKind::Main:
                    return "main";
                case RenderCore::ViewKind::Shadow:
                    return "shadow";
                case RenderCore::ViewKind::Reflection:
                    return "reflection";
                case RenderCore::ViewKind::Refraction:
                    return "refraction";
                case RenderCore::ViewKind::Map:
                    return "map";
                case RenderCore::ViewKind::Preview:
                    return "preview";
                case RenderCore::ViewKind::PrecipitationOcclusion:
                    return "precipitation-occlusion";
                case RenderCore::ViewKind::Debug:
                    return "debug";
            }
            return "unknown";
        }

        void hashCombine(std::uint64_t& seed, std::uint64_t value) noexcept
        {
            seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        }
    }

    VsgRuntimeHost::VsgRuntimeHost(
        vsg::ref_ptr<SdlVulkanWindow> window, StaticTextureResolver textureResolver, VsgRuntimeHostOptions options)
        : mOptions(options)
        , mTextureResolver(std::move(textureResolver))
        , mWindow(std::move(window))
        , mSharedObjects(vsg::SharedObjects::create())
        , mViewer(vsg::Viewer::create())
        , mSceneRoot(vsg::Group::create())
        , mStaticRoot(vsg::Group::create())
        , mDynamicRoot(vsg::Group::create())
        , mGuiRoot(vsg::Group::create())
        , mMainOnlyRoot(vsg::Group::create())
        , mAmbientLight(vsg::AmbientLight::create())
        , mSunLight(vsg::DirectionalLight::create())
        , mSkyBackdrop(SkyBackdrop::create())
        , mCamera(FrameCameraObjects::create(initialView()))
        , mCompletion(options.maximumFramesInFlight)
    {
        if (!mWindow || !mWindow->valid() || !mTextureResolver
            || options.maximumFramesInFlight != VsgRecordAndSubmitRingSize)
            throw std::invalid_argument(
                "VsgRuntimeHost requires a valid window/resolver and the exact VSG 1.1.15 three-frame task ring");
        if (options.shadows.enabled
            && (options.shadows.cascadeCount == 0 || options.shadows.cascadeCount > 8
                || options.shadows.mapResolution < 256 || options.shadows.mapResolution > 4096
                || options.shadows.maximumDistance <= 0.0
                || options.shadows.maximumDistance > std::numeric_limits<float>::max() || options.shadows.depthBias < 0.0
                || options.shadows.splitLambda < 0.0 || options.shadows.splitLambda > 1.0))
            throw std::invalid_argument("VsgRuntimeHost received unsafe or invalid CP4D shadow settings");
        if (options.water.enabled
            && (options.water.targetSize < 64 || options.water.targetSize > 2048
                || options.water.reflectionLodScale <= 0.0f || options.water.refractionLodScale <= 0.0f))
            throw std::invalid_argument("VsgRuntimeHost received unsafe or invalid CP4E water settings");

        mViewer->addWindow(mWindow);
        mView = vsg::View::create(mCamera.camera);
        mOpenMwViewState = OpenMwViewDependentState::create(mView.get());
        mOpenMwViewState->shaderSet = createLegacyCompatibilityShaderSet();
        if (!mOpenMwViewState->shaderSet)
            throw std::runtime_error("VsgRuntimeHost could not create its OpenMW view shader contract");
        mView->viewDependentState = mOpenMwViewState;
        mAmbientLight->name = "OpenMW ambient";
        mSunLight->name = "OpenMW sun";
        if (options.shadows.enabled)
        {
            mSunLight->shadowSettings = vsg::HardShadows::create(options.shadows.cascadeCount);
            mOpenMwViewState->maxShadowDistance = options.shadows.maximumDistance;
            mOpenMwViewState->shadowMapBias = options.shadows.depthBias;
            mOpenMwViewState->lambda = options.shadows.splitLambda;
        }
        mView->addChild(mAmbientLight);
        mView->addChild(mSunLight);
        if (!mSkyBackdrop)
            throw std::runtime_error("VsgRuntimeHost could not create its CP4D sky backdrop");
        // The backdrop belongs only to the color-producing main view. Native
        // VSG shadow traversals use ShadowTraversalMask and must never record
        // this color-only pipeline into a depth-only shadow render pass.
        mView->addChild(maskedNode(vsg::MASK_ALL & ~ShadowTraversalMask, mSkyBackdrop.node()));
        mSceneRoot->addChild(mStaticRoot);
        mSceneRoot->addChild(mDynamicRoot);
        const VkExtent2D initialExtent = mWindow->extent2D();
        mUiPipeline = createUiPipeline(std::max(1u, initialExtent.width), std::max(1u, initialExtent.height));
        if (!mUiPipeline)
            throw std::runtime_error("VsgRuntimeHost could not create its MyGUI pipeline");
        mSceneVisibility = vsg::Switch::create();
        mSceneVisibility->addChild(vsg::MASK_ALL, mSceneRoot);
        mView->addChild(mSceneVisibility);
        const auto makeWaterView = [&](RenderCore::ViewKind kind) -> std::optional<WaterViewRuntime> {
            WaterViewRuntime result;
            RenderCore::FrameView initial = initialView();
            initial.kind = kind;
            initial.extent = { options.water.targetSize, options.water.targetSize };
            result.target = createOffscreenRenderTarget(mWindow->getOrCreateDevice(), initial.extent);
            if (!result.target)
                return std::nullopt;
            result.camera = FrameCameraObjects::create(initial);
            result.view = vsg::View::create(
                result.camera.camera, vsg::ref_ptr<vsg::Node>{}, vsg::RECORD_LIGHTS);
            result.view->mask = vsg::MASK_OFF;
            result.state = OpenMwViewDependentState::create(result.view.get());
            result.state->shaderSet = createLegacyCompatibilityShaderSet();
            if (!result.state->shaderSet)
                return std::nullopt;
            result.view->viewDependentState = result.state;
            result.view->addChild(mAmbientLight);
            result.view->addChild(mSunLight);
            result.view->addChild(mSceneRoot);
            result.view->bins = createStaticConformanceBins();
            result.target.renderGraph->addChild(result.view);
            return result;
        };
        if (options.water.enabled && options.water.reflection)
            mReflectionView = makeWaterView(RenderCore::ViewKind::Reflection);
        if (options.water.enabled && options.water.refraction)
            mRefractionView = makeWaterView(RenderCore::ViewKind::Refraction);
        if ((options.water.enabled && options.water.reflection && !mReflectionView)
            || (options.water.enabled && options.water.refraction && !mRefractionView))
            throw std::runtime_error("VsgRuntimeHost could not create persistent CP4E water targets");

        if (options.water.enabled)
            mWaterSurface = WaterSurface::create(mReflectionView ? mReflectionView->target.color : nullptr,
                mRefractionView ? mRefractionView->target.color : nullptr, options.water.normalMap);
        if (options.water.enabled && !mWaterSurface)
            throw std::runtime_error("VsgRuntimeHost could not create its CP4E water surface");
        if (mWaterSurface)
            mMainOnlyRoot->addChild(mWaterSurface.node());
        // Disabling depth is insufficient: the world's deferred bins execute
        // after ordinary View children and would paint over a direct GUI node.
        static_assert(UiOverlayBinNumber > StaticBackToFrontBinNumber);
        mMainOnlyRoot->addChild(createUiOverlayLayer(mGuiRoot));
        mView->addChild(mMainOnlyRoot);
        mView->bins = createStaticConformanceBins();
        mView->bins.push_back(vsg::Bin::create(UiOverlayBinNumber, vsg::Bin::NO_SORT));
        mRenderGraph = vsg::RenderGraph::create(mWindow);
        mRenderGraph->addChild(mView);
        mCommandGraph = vsg::CommandGraph::create(mWindow);
        if (mReflectionView)
            mCommandGraph->addChild(mReflectionView->target.renderGraph);
        if (mRefractionView)
            mCommandGraph->addChild(mRefractionView->target.renderGraph);
        mCommandGraph->addChild(mRenderGraph);
        mViewer->assignRecordAndSubmitTaskAndPresentation({ mCommandGraph });
        auto resourceHints = vsg::ResourceHints::create();
        if (options.shadows.enabled)
        {
            resourceHints->numShadowMapsRange
                = { options.shadows.cascadeCount, options.shadows.cascadeCount };
            resourceHints->shadowMapSize = { options.shadows.mapResolution, options.shadows.mapResolution };
        }
        const vsg::CompileResult compile = mViewer->compile(resourceHints);
        if (!compile)
            throw std::runtime_error("VsgRuntimeHost initial graph compilation failed: " + compile.message);
        // Viewer::compile initializes view-dependent shadow resources during
        // resource collection. Discover persistent contexts only AFTER that
        // step, otherwise shadow Views have not been allocated yet.
        auto previousCompileManager = mViewer->compileManager;
        auto compileManager = ViewCompileManager::create(*mViewer, resourceHints);
        compileManager->resourceScavenger = previousCompileManager->resourceScavenger;
        if (mViewer->instrumentation)
            compileManager->assignInstrumentation(mViewer->instrumentation);
        for (const auto& task : mViewer->recordAndSubmitTasks)
            if (task->databasePager && task->databasePager->compileManager == previousCompileManager)
                task->databasePager->compileManager = compileManager;
        mViewer->compileManager = compileManager;
        // VSG's viewer context discovery uses an ordinary visitor: masked-off
        // water Views above are deliberately absent from it. Register their
        // framebuffer contexts once while keeping rendering disabled until a
        // frame actually requests water. Otherwise later exact-view compilation
        // has no matching context and leaves exterior pipelines unrealized.
        if (!std::getenv("OPENMW_V4_UNREGISTERED_WATER_VIEWS"))
        {
            if (mReflectionView)
                mViewer->compileManager->add(*mReflectionView->target.renderGraph->framebuffer, mReflectionView->view);
            if (mRefractionView)
                mViewer->compileManager->add(*mRefractionView->target.renderGraph->framebuffer, mRefractionView->view);
        }
    }

    void VsgRuntimeHost::attachGuiRenderer(VsgMyGui::RenderManager* renderer) noexcept
    {
        mGuiRenderer = renderer;
    }

    void VsgRuntimeHost::detachGuiRenderer(const VsgMyGui::RenderManager* renderer) noexcept
    {
        if (mGuiRenderer == renderer)
            mGuiRenderer = nullptr;
    }

    VsgRuntimeHost::~VsgRuntimeHost()
    {
        waitIdle();
        if (mDynamicRoot)
            mDynamicRoot->children.clear();
        if (mGuiRoot)
            mGuiRoot->children.clear();
        mDynamicPublishedRoot = {};
        mGuiPublishedRoot = {};
        if (mSceneRoot)
            mSceneRoot->children.clear();
        if (mMainOnlyRoot)
            mMainOnlyRoot->children.clear();
        mAuxiliaryViews.clear();
        mReflectionView.reset();
        mRefractionView.reset();
        mRenderGraph = {};
        mCommandGraph = {};
        mOpenMwViewState = {};
        mView = {};
        mViewer = {};
        mWindow = {};
    }

    RenderCore::RenderBackendKind VsgRuntimeHost::backendKind() const noexcept
    {
        return RenderCore::RenderBackendKind::VsgVulkan;
    }

    vsg::ref_ptr<vsg::ImageView> VsgRuntimeHost::auxiliaryColorImage(
        RenderCore::RenderTargetHandle target) const noexcept
    {
        const auto found = std::find_if(mAuxiliaryViews.begin(), mAuxiliaryViews.end(),
            [&](const AuxiliaryViewRuntime& value) { return value.targetIdentity == target; });
        return found == mAuxiliaryViews.end() ? vsg::ref_ptr<vsg::ImageView>{} : found->target.color;
    }

    bool VsgRuntimeHost::retireAuxiliarySurface(RenderCore::RenderTargetHandle target)
    {
        const auto found = std::find_if(mAuxiliaryViews.begin(), mAuxiliaryViews.end(),
            [&](const AuxiliaryViewRuntime& value) { return value.targetIdentity == target; });
        if (found == mAuxiliaryViews.end())
            return true;
        if (!mCommandGraph || !found->target.renderGraph || !found->commandVisibility)
        {
            mLastDiagnostic = "persistent auxiliary target retirement found an invalid command graph";
            return false;
        }

        waitIdle();
        const auto graph = std::find(
            mCommandGraph->children.begin(), mCommandGraph->children.end(), found->commandVisibility);
        if (graph == mCommandGraph->children.end())
        {
            mLastDiagnostic = "persistent auxiliary target retirement could not resolve its command graph child";
            return false;
        }
        mCommandGraph->children.erase(graph);
        mAuxiliaryViews.erase(found);
        return true;
    }

    const RenderCore::FrameView* VsgRuntimeHost::selectMainView(
        const RenderCore::FrameRenderState& frame) const noexcept
    {
        const auto main = std::find_if(frame.views().begin(), frame.views().end(), [](const auto& view) {
            return view.kind == RenderCore::ViewKind::Main;
        });
        if (main == frame.views().end() || main != frame.views().begin()
            || main->semanticIncludeMask != ~std::uint64_t{ 0 } || main->semanticExcludeMask != 0)
            return nullptr;
        const auto target = std::find_if(frame.renderTargets().begin(), frame.renderTargets().end(),
            [&](const auto& value) { return value.identity == main->outputTarget; });
        const auto present = std::find_if(frame.renderPasses().begin(), frame.renderPasses().end(),
            [](const auto& pass) { return pass.present; });
        if (target == frame.renderTargets().end() || target->kind != RenderCore::RenderTargetKind::Swapchain
            || present == frame.renderPasses().end() || !present->view || *present->view != main->identity
            || present->output != main->outputTarget
            || std::count_if(frame.renderPasses().begin(), frame.renderPasses().end(),
                   [](const auto& pass) { return pass.present; })
                != 1)
            return nullptr;
        return &*main;
    }

    bool VsgRuntimeHost::waterViewsCompatible(const RenderCore::FrameRenderState& frame,
        const RenderCore::FrameView*& reflection, const RenderCore::FrameView*& refraction) const noexcept
    {
        reflection = nullptr;
        refraction = nullptr;
        for (const RenderCore::FrameView& view : frame.views())
        {
            if (view.kind == RenderCore::ViewKind::Main)
                continue;
            if (view.kind == RenderCore::ViewKind::Reflection && !reflection)
                reflection = &view;
            else if (view.kind == RenderCore::ViewKind::Refraction && !refraction)
                refraction = &view;
            else if (!isGenericAuxiliaryView(view.kind))
                return false;
        }
        const bool water = frame.environment().waterEnabled;
        const bool expectReflection = water && mOptions.water.enabled && mOptions.water.reflection;
        const bool expectRefraction = water && mOptions.water.enabled && mOptions.water.refraction;
        if ((reflection != nullptr) != expectReflection || (refraction != nullptr) != expectRefraction)
            return false;
        const auto validView = [&](const RenderCore::FrameView* view, RenderCore::ViewKind kind,
                                   float lodScale) {
            if (!view)
                return true;
            const auto target = std::find_if(frame.renderTargets().begin(), frame.renderTargets().end(),
                [&](const auto& value) { return value.identity == view->outputTarget; });
            const auto pass = std::find_if(frame.renderPasses().begin(), frame.renderPasses().end(),
                [&](const auto& value) { return value.view && *value.view == view->identity; });
            const auto present = std::find_if(frame.renderPasses().begin(), frame.renderPasses().end(),
                [](const auto& value) { return value.present; });
            return view->kind == kind && view->clipPlane && !view->temporal && !view->historyValid
                && view->extent == RenderCore::Extent2D{ mOptions.water.targetSize, mOptions.water.targetSize }
                && view->lodScale == lodScale && target != frame.renderTargets().end()
                && target->kind == RenderCore::RenderTargetKind::Offscreen
                && target->colorFormat == RenderCore::RenderTargetFormat::Rgba16Float
                && target->depthFormat == RenderCore::RenderTargetFormat::Depth32Float && !target->transient
                && pass != frame.renderPasses().end() && !pass->present && pass->output == view->outputTarget
                && present != frame.renderPasses().end()
                && std::find(present->inputs.begin(), present->inputs.end(), view->outputTarget)
                    != present->inputs.end()
                && std::find(present->dependencies.begin(), present->dependencies.end(), pass->identity)
                    != present->dependencies.end();
        };
        return validView(reflection, RenderCore::ViewKind::Reflection, mOptions.water.reflectionLodScale)
            && validView(refraction, RenderCore::ViewKind::Refraction, mOptions.water.refractionLodScale)
            && frame.renderTargets().size() == frame.views().size()
            && frame.renderPasses().size() == frame.views().size();
    }

    bool VsgRuntimeHost::shadowViewFamilyCompatible(const RenderCore::FrameRenderState& frame) const noexcept
    {
        const bool expected = mOptions.shadows.enabled && frame.environment().shadowsEnabled;
        if (!expected)
            return frame.derivedViewFamilies().empty();
        if (frame.derivedViewFamilies().size() != 1)
            return false;
        const RenderCore::DerivedViewFamilyDesc& family = frame.derivedViewFamilies().front();
        const RenderCore::FrameView* const mainView = selectMainView(frame);
        return mainView && family.kind == RenderCore::ViewKind::Shadow && family.sourceView == mainView->identity
            && family.viewCount == mOptions.shadows.cascadeCount
            && family.extent.width == mOptions.shadows.mapResolution
            && family.extent.height == mOptions.shadows.mapResolution
            && family.maximumDistance == static_cast<float>(mOptions.shadows.maximumDistance)
            && family.semanticIncludeMask
                == RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster)
            && family.semanticExcludeMask == 0 && !family.transient;
    }

    bool VsgRuntimeHost::synchronizeStaticWorld(const RenderCore::RenderWorld& world)
    {
        Debug::GameplayDiagnostics::Stage diagnostic("static_sync");
        if (mStaticSyncState.unchanged(world, mOptions.staticPlan)
            && !std::getenv("OPENMW_V4_REBUILD_STATIC_PLANS"))
            return true;
        const StaticWorldPlan worldPlan = [&] {
            Debug::GameplayDiagnostics::Stage planning("static_plan");
            return buildStaticWorldPlan(world, mOptions.staticPlan);
        }();
        const StaticWorldMutation mutation = mStaticResidency.prepare(world, worldPlan);
        const char* coarsePopulationControl = std::getenv("OPENMW_V4_COARSE_POPULATION_REBUILD_CONTROL");
        const bool coarsePopulationInvalidation = coarsePopulationControl && std::string_view(coarsePopulationControl) == "1";
        const StaticPopulationMutation populationMutation
            = mStaticPopulationResidency.prepare(world, worldPlan, coarsePopulationInvalidation);
        if (!mutation.valid || !populationMutation.valid || mutation.simpleMeshInstancesDeferred != 0)
        {
            mLastDiagnostic = "static world contains invalid or not-yet-supported instance populations";
            return false;
        }
        if (mutation.upserts.empty() && mutation.removals.empty() && populationMutation.upserts.empty()
            && populationMutation.removals.empty())
        {
            mStaticSyncState.synchronized();
            return true;
        }

        // This cache owns descriptors/configurators, not just cheap lookup keys.
        // Prune only cache-exclusive objects; live and fence-retired graphs keep
        // their own references. Never clear the live graph to recover memory.
        mSharedObjects->prune();

        // CompileManager::compile records transfers and waits for completion.
        // Loading N placements must not perform N synchronous upload round trips.
        // Keep new roots private until the complete batch has compiled successfully.
        // Default-off until representative load-time testing demonstrates a
        // benefit without a staging-memory regression. Same-executable control.
        const char* batchMode = std::getenv("OPENMW_V4_BATCH_STATIC_UPLOADS");
        const bool individualCompile = !batchMode || std::string_view(batchMode) != "1";
        auto pendingCompile = vsg::Group::create();
        auto prepareGraph = [&](vsg::ref_ptr<vsg::Node> graph, const std::string& subject) {
            if (!individualCompile)
            {
                pendingCompile->addChild(std::move(graph));
                return true;
            }
            Debug::GameplayDiagnostics::Stage compiling("static_compile");
            const auto compileResult = compileForViewer(*mViewer, graph);
            if (!compileResult)
                mLastDiagnostic = compileFailureDiagnostic(subject, compileResult);
            return static_cast<bool>(compileResult);
        };
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("static_mutation", {
                {"instances", std::to_string(mutation.upserts.size())},
                {"populations", std::to_string(populationMutation.upserts.size())},
                {"population_new", std::to_string(populationMutation.newGroups)},
                {"population_chunk_changed", std::to_string(populationMutation.changedChunks)},
                {"population_options_changed", std::to_string(populationMutation.changedOptions)},
                {"population_stale_dependencies", std::to_string(populationMutation.staleDependencies)},
                {"population_coarse_control", std::to_string(coarsePopulationInvalidation)},
                {"individual_compile", std::to_string(individualCompile)} });

        std::vector<StaticResident> replacements;
        replacements.reserve(mutation.upserts.size());
        for (const StaticInstancePlan& plan : mutation.upserts)
        {
            if (!plan.lightingEnabled)
            {
                mLastDiagnostic = "per-instance disabled lighting is not implemented by the CP3C host";
                return false;
            }
            StaticRealizationResult realized
                = realizeStaticAssetConformant(world, plan.model, plan.asset, mTextureResolver, mSharedObjects);
            if (!realized.valid() || realized.stats.runtimeContextEffects != 0
                || realized.stats.unsupportedTextureBindings != 0)
            {
                mLastDiagnostic = realized.diagnostics.empty()
                    ? "static realization requires a not-yet-supported compatibility effect"
                    : realized.diagnostics.front();
                return false;
            }
            auto placed = vsg::MatrixTransform::create(toVsgMatrix(staticInstancePlacementMatrix(plan.placement)));
            const bool terrain = hasSemanticFlag(plan.semanticFlags, RenderCore::InstanceSemanticFlag::Terrain);
            const bool castsShadow = hasSemanticFlag(plan.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster)
                && (terrain ? mOptions.shadows.terrainCasters : mOptions.shadows.objectCasters);
            placed->addChild(realized.root);
            const RenderCore::ModelRecord* model = world.get(plan.model);
            const std::string subject = model && !model->sourceIdentity.empty()
                ? "incremental VSG model '" + model->sourceIdentity + "'" : "incremental VSG model";
            if (!prepareGraph(placed, subject))
            {
                return false;
            }
            replacements.push_back(maskedNode(placementMask(castsShadow, plan.semanticFlags), std::move(placed)));
        }

        std::vector<StaticPopulationResident> populationReplacements;
        populationReplacements.reserve(populationMutation.upserts.size());
        for (const StaticPopulationPlan& plan : populationMutation.upserts)
        {
            if (std::ranges::any_of(plan.placements,
                    [](const RenderCore::PopulationInstanceRecord& placement) { return !placement.lightingEnabled; }))
            {
                mLastDiagnostic = "per-placement disabled lighting is not implemented by the CP4C host";
                return false;
            }
            const auto castsShadow = [&](const RenderCore::PopulationInstanceRecord& placement) {
                const bool terrain
                    = hasSemanticFlag(placement.semanticFlags, RenderCore::InstanceSemanticFlag::Terrain);
                return hasSemanticFlag(placement.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster)
                    && (terrain ? mOptions.shadows.terrainCasters : mOptions.shadows.objectCasters);
            };
            const vsg::Mask firstPlacementMask
                = placementMask(castsShadow(plan.placements.front()), plan.placements.front().semanticFlags);
            const bool mixedTraversalMasks = std::ranges::any_of(plan.placements,
                [&](const RenderCore::PopulationInstanceRecord& placement) {
                    return placementMask(castsShadow(placement), placement.semanticFlags) != firstPlacementMask;
                });
            const bool requiresIndividualPlacement = mixedTraversalMasks || std::ranges::any_of(plan.asset.draws,
                [&](const StaticDrawPlan& draw) {
                    const RenderCore::MaterialRecord* material = world.get(draw.material);
                    return !material || material->transparentSort == RenderCore::TransparentSortPolicy::Sorted
                        || draw.billboard.has_value();
                });
            StaticRealizationResult realized = requiresIndividualPlacement
                ? realizeStaticAssetConformant(world, plan.model, plan.asset, mTextureResolver, mSharedObjects)
                : realizeStaticAssetConformant(world, plan.model, plan.asset, mTextureResolver, mSharedObjects, {},
                    plan.placements, plan.coordinateOrigin);
            if (!realized.valid() || realized.stats.runtimeContextEffects != 0
                || realized.stats.unsupportedTextureBindings != 0)
            {
                mLastDiagnostic = realized.diagnostics.empty()
                    ? "population realization requires a not-yet-supported compatibility effect"
                    : realized.diagnostics.front();
                return false;
            }
            auto group = vsg::Group::create();
            if (!requiresIndividualPlacement)
            {
                auto origin = vsg::MatrixTransform::create(
                    vsg::translate(plan.coordinateOrigin.x, plan.coordinateOrigin.y, plan.coordinateOrigin.z));
                origin->addChild(realized.root);
                group->addChild(maskedNode(firstPlacementMask, std::move(origin)));
            }
            else
            {
                group->children.reserve(plan.placements.size());
                for (const RenderCore::PopulationInstanceRecord& placement : plan.placements)
                {
                    auto placed
                        = vsg::MatrixTransform::create(toVsgMatrix(staticInstancePlacementMatrix(placement.transform)));
                    placed->addChild(realized.root);
                    group->addChild(maskedNode(
                        placementMask(castsShadow(placement), placement.semanticFlags), std::move(placed)));
                }
            }
            const RenderCore::ModelRecord* model = world.get(plan.model);
            std::string subject = model && !model->sourceIdentity.empty()
                ? "incremental VSG population model '" + model->sourceIdentity + "'"
                : "incremental VSG population model";
            subject += " with " + std::to_string(plan.placements.size()) + " placements";
            if (!prepareGraph(group, subject))
            {
                return false;
            }
            auto visibility = vsg::Switch::create();
            visibility->addChild(vsg::MASK_ALL, std::move(group));
            populationReplacements.push_back({ std::move(visibility) });
        }

        if (!pendingCompile->children.empty())
        {
            Debug::GameplayDiagnostics::Stage compiling("static_compile");
            const auto result = compileForViewer(*mViewer, pendingCompile);
            if (!result)
            {
                mLastDiagnostic = compileFailureDiagnostic("incremental VSG static batch ("
                    + std::to_string(mutation.upserts.size()) + " instances, "
                    + std::to_string(populationMutation.upserts.size()) + " populations)", result);
                return false;
            }
        }

        std::unordered_map<std::uint64_t, std::size_t> replacementIndices;
        replacementIndices.reserve(mutation.upserts.size());
        for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
            replacementIndices.emplace(staticInstanceKey(mutation.upserts[i].instance), i);

        // Allocate the next traversal list before changing logical residency or
        // the live root. Residency performs its own pre-publication allocation;
        // the final scene-root publication is an allocation-free swap.
        vsg::Group::Children nextChildren;
        nextChildren.reserve(mutation.orderedInstances.size() + populationMutation.orderedPopulations.size());
        for (const RenderCore::InstanceHandle handle : mutation.orderedInstances)
        {
            const auto replacement = replacementIndices.find(staticInstanceKey(handle));
            if (replacement != replacementIndices.end())
            {
                nextChildren.push_back(replacements[replacement->second]);
                continue;
            }
            const StaticResident* resident = mStaticResidency.residentObject(handle);
            if (!resident)
            {
                mLastDiagnostic = "static traversal order could not resolve a resident instance";
                return false;
            }
            nextChildren.push_back(*resident);
        }
        for (const StaticPopulationIdentity identity : populationMutation.orderedPopulations)
        {
            const auto replacement = std::find_if(populationMutation.upserts.begin(), populationMutation.upserts.end(),
                [&](const StaticPopulationPlan& plan) { return populationIdentity(plan) == identity; });
            if (replacement != populationMutation.upserts.end())
            {
                const std::size_t index
                    = static_cast<std::size_t>(replacement - populationMutation.upserts.begin());
                nextChildren.push_back(populationReplacements[index].visibility);
                continue;
            }
            const StaticPopulationResident* resident = mStaticPopulationResidency.residentObject(identity);
            if (!resident)
            {
                mLastDiagnostic = "static traversal order could not resolve a resident population";
                return false;
            }
            nextChildren.push_back(resident->visibility);
        }

        if ((!populationMutation.upserts.empty() || !populationMutation.removals.empty())
            && !mStaticPopulationResidency.commit(world, populationMutation, std::move(populationReplacements)).committed)
        {
            mLastDiagnostic = "static population changed while its replacement graph was being realized";
            return false;
        }
        if ((!mutation.upserts.empty() || !mutation.removals.empty())
            && !mStaticResidency.commit(world, mutation, std::move(replacements)).committed)
        {
            mLastDiagnostic = "static world changed while its replacement graph was being realized";
            return false;
        }
        mStaticRoot->children.swap(nextChildren);
        nextChildren.clear(); // release the old traversal references before pruning
        mSharedObjects->prune();
        mStaticSyncState.synchronized();
        return true;
    }

    bool VsgRuntimeHost::synchronizePopulationVisibility(
        const RenderCore::RenderWorld& world, const RenderCore::FrameView& mainView)
    {
        bool valid = true;
        mStaticPopulationResidency.forEachResident(
            [&](const StaticPopulationPlan& plan, StaticPopulationResident& resident) {
                if (!resident.visibility || resident.visibility->children.size() != 1)
                {
                    valid = false;
                    return;
                }
                resident.visibility->setAllChildren(
                    populationWithinMaximumDistance(world, plan, mainView.current.worldPosition));
            });
        if (!valid)
            mLastDiagnostic = "resident population visibility graph is invalid";
        return valid;
    }

    bool VsgRuntimeHost::synchronizeDynamicActors(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame)
    {
        Debug::GameplayDiagnostics::Stage diagnostic("dynamic_realization");
        unsigned diagnosticActors = 0;
        unsigned reusedActors = 0, rebuiltActors = 0;
        const DynamicActorWorldPlan plan = [&] {
            Debug::GameplayDiagnostics::Stage planningDiagnostic("actor_plan");
            return mActorPlanCache.prepare(world, mOptions.staticPlan,
                std::getenv("OPENMW_V4_REBUILD_ACTOR_PLANS_CONTROL") != nullptr);
        }();
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("actor_plan_cache", {{"reused", std::to_string(mActorPlanCache.reused)},
                {"rebuilt", std::to_string(mActorPlanCache.rebuilt)}});
        if (!plan.valid())
        {
            mLastDiagnostic = plan.diagnostic.empty()
                ? "dynamic actor world contains an invalid model/skeleton dependency"
                : "dynamic actor world contains an invalid model/skeleton dependency: " + plan.diagnostic;
            return false;
        }

        // New dynamic residents are created from their evaluated CPU state. Their configurators contain frame-owned arrays, so placing
        // them in the host-wide SharedObjects cache retains every old frame and
        // eventually exhausts device memory. Share only within this generation;
        // persistent textures remain shared by mTextureResolver.
        auto frameSharedObjects = vsg::SharedObjects::create();

        // If the GPU has completed the current generation, detach it before
        // reserving the replacement. This avoids a needless full-generation
        // VRAM overlap while preserving retirement for genuinely in-flight work.
        if (mDynamicPublishedRoot && mDynamicLastUse && mCompletedThrough
            && *mDynamicLastUse <= *mCompletedThrough)
        {
            mDynamicRoot->children.clear();
            mDynamicPublishedRoot = {};
            mDynamicLastUse.reset();
        }

        auto nextRoot = vsg::Group::create();
        nextRoot->children.reserve(plan.actors.size());
        auto pendingCompile = vsg::Group::create();
        mLastCompiledDynamicRootCount = 0;
        mDynamicActorResidents.beginFrame(frame.frameId(), mCompletedThrough);
        for (const DynamicActorPlan& actor : plan.actors)
        {
            if (!actor.lightingEnabled)
            {
                mLastDiagnostic = "per-actor disabled lighting is not implemented by the CP3D host";
                return false;
            }
            const auto transform = std::find_if(frame.dynamicTransforms().begin(), frame.dynamicTransforms().end(),
                [&](const RenderCore::DynamicTransformState& value) { return value.instance == actor.instance; });
            if (transform == frame.dynamicTransforms().end())
            {
                mLastDiagnostic = "dynamic actor frame is missing its current world transform";
                return false;
            }
            std::vector<glm::mat4> evaluatedModelNodes;
            const std::optional<StaticAssetPlan> evaluatedAsset
                = evaluateDynamicActorAssetPlan(world, frame, actor, &evaluatedModelNodes);
            if (!evaluatedAsset)
            {
                mLastDiagnostic = "dynamic actor draw transforms rejected the evaluated skeleton pose";
                return false;
            }

            std::unordered_map<std::uint32_t, RenderCore::MeshPayload> deformed;
            for (const StaticDrawPlan& draw : evaluatedAsset->draws)
            {
                const RenderCore::MeshRecord* mesh = world.get(draw.mesh);
                if (!mesh || (!mesh->skinned && !mesh->morphed) || deformed.contains(draw.node.value()))
                    continue;
                RenderCore::DeformedMeshPayload result
                    = RenderCore::deformMesh(world, frame, actor.instance, draw.mesh, draw.node, false, &evaluatedModelNodes);
                if (!result.ready() || !mesh->payload)
                {
                    mLastDiagnostic = "dynamic actor CPU deformation rejected its evaluated pose or morph state";
                    return false;
                }
                RenderCore::MeshPayload payload = *mesh->payload;
                payload.positions = std::move(result.positions);
                payload.normals = std::move(result.normals);
                payload.tangents = std::move(result.tangents);
                payload.bitangents = std::move(result.bitangents);
                deformed.emplace(draw.node.value(), std::move(payload));
            }
            const MeshPayloadResolver resolve
                = [&](RenderCore::MeshHandle, RenderCore::ModelNodeIndex node) -> const RenderCore::MeshPayload* {
                const auto found = deformed.find(node.value());
                return found == deformed.end() ? nullptr : &found->second;
            };
            const std::string residentIdentity = std::to_string(world.epoch().value()) + ":"
                + std::to_string(actor.instance.slot()) + ":" + std::to_string(actor.instance.generation());
            auto& resident = mDynamicActorResidents.acquire(residentIdentity);
            const bool canReuse = !std::getenv("OPENMW_V4_REBUILD_ACTORS") && resident.published && resident.contract
                && resident.epoch == world.epoch() && resident.opacity == transform->opacity
                && dynamicActorPlanCurrent(world, *resident.contract);
            if (!canReuse || !updateDeformedAssetRealization(world, *evaluatedAsset, resolve, resident.mutableDraws))
            {
                ++rebuiltActors;
                StaticRealizationResult realized = realizeStaticAssetConformant(
                    world, actor.model, *evaluatedAsset, mTextureResolver, frameSharedObjects, resolve, {}, {},
                    transform->opacity, true);
                if (!realized.valid() || realized.stats.runtimeContextEffects != 0
                    || realized.stats.unsupportedTextureBindings != 0)
                {
                    mLastDiagnostic = realized.diagnostics.empty()
                        ? "dynamic actor realization requires a not-yet-supported compatibility effect"
                        : realized.diagnostics.front();
                    return false;
                }
                auto placed = vsg::MatrixTransform::create();
                placed->addChild(realized.root);
                const bool castsShadow = mOptions.shadows.actorCasters
                    && hasSemanticFlag(actor.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster);
                resident.published = maskedNode(placementMask(castsShadow, actor.semanticFlags), placed);
                resident.placement = std::move(placed);
                resident.mutableDraws = std::move(realized.mutableDraws);
                resident.contract = actor;
                resident.epoch = world.epoch();
                resident.opacity = transform->opacity;
                pendingCompile->addChild(resident.published);
            }
            else ++reusedActors;
            if (!dynamicActorPlanCurrent(world, actor))
            {
                mLastDiagnostic = "dynamic actor changed while its frame graph was being realized";
                return false;
            }
            resident.placement->matrix = toVsgMatrix(staticInstancePlacementMatrix(transform->current));
            if (Debug::GameplayDiagnostics::detailedSampling() && diagnosticActors++ < 16)
            {
                // Compare the CPU deformation with its actual resident streams.
                // Bounded vertex samples, not a pixel or full mesh parity claim.
                std::uint64_t hash = 14695981039346656037ull;
                std::size_t vertices = 0, sampled = 0, mismatches = 0, nonfinite = 0;
                double maxAbs = 0;
                for (std::size_t d = 0; d < evaluatedAsset->draws.size() && d < 64; ++d)
                {
                    const auto& draw = evaluatedAsset->draws[d];
                    const auto* mesh = world.get(draw.mesh);
                    const auto* payload = resolve(draw.mesh, draw.node);
                    if (!payload && mesh) payload = mesh->payload.get();
                    if (!payload) continue;
                    vertices += payload->positions.size();
                    const auto* target = d < resident.mutableDraws.size() ? resident.mutableDraws[d].positions.get() : nullptr;
                    if (!target || target->size() != payload->positions.size()) ++mismatches;
                    const std::size_t step = std::max<std::size_t>(1, (payload->positions.size() + 31) / 32);
                    for (std::size_t v = 0; v < payload->positions.size(); v += step)
                    {
                        const auto& p = payload->positions[v];
                        hash = (hash ^ Debug::GameplayDiagnostics::fingerprint(&p, sizeof(p))) * 1099511628211ull;
                        ++sampled;
                        for (int c = 0; c < 3; ++c)
                        {
                            if (!std::isfinite(p[c])) ++nonfinite;
                            else maxAbs = std::max(maxAbs, std::abs(double(p[c])));
                            if (target && v < target->size() && (*target)[v][c] != p[c]) ++mismatches;
                        }
                    }
                }
                const auto placement = staticInstancePlacementMatrix(transform->current);
                Debug::GameplayDiagnostics::recordEvent("actor_geometry", {{"actor", residentIdentity},
                    {"sample_hash", std::to_string(hash)}, {"vertices_in_checked_draws", std::to_string(vertices)},
                    {"sampled_vertices", std::to_string(sampled)}, {"stream_mismatches", std::to_string(mismatches)},
                    {"nonfinite", std::to_string(nonfinite)}, {"sample_max_abs", std::to_string(maxAbs)},
                    {"draws", std::to_string(evaluatedAsset->draws.size())},
                    {"placement_hash", std::to_string(Debug::GameplayDiagnostics::fingerprint(&placement, sizeof(placement)))}});
            }
            nextRoot->addChild(resident.published);
        }

        std::size_t reusedEffects = 0;
        std::size_t rebuiltEffects = 0;
        mImmediateEffectResidents.beginFrame(frame.frameId(), mCompletedThrough);
        for (const RenderCore::ImmediateEffectDraw& effect : frame.immediateEffectDraws())
        {
            // Acquire only a fence-completed version. Retaining an old graph
            // alone does not make overwriting its shared arrays/placement safe.
            auto& resident = mImmediateEffectResidents.acquire(effect.identity);
            const auto mismatch = resident.published
                ? immediateEffectLayoutMismatch(resident.contract, effect,
                    std::getenv("OPENMW_V4_IMMUTABLE_EFFECT_BOUNDS") != nullptr)
                : ImmediateEffectMismatch::NoCompletedResident;
            if (mismatch == ImmediateEffectMismatch::None
                && updateImmediateEffectRealization(effect, resident.mutableDraws))
            {
                // The reuse predicate already proved the immutable contract
                // unchanged. Do not copy every mesh/texture snapshot again just
                // to retain a pose that is represented by the mutable arrays.
                resident.placement->matrix = toVsgMatrix(effect.worldTransform);
                nextRoot->addChild(resident.published);
                ++reusedEffects;
                continue;
            }

            if (Debug::RuntimeDiagnostics::enabled())
            {
                const auto reason = mismatch == ImmediateEffectMismatch::None ? ImmediateEffectMismatch::StreamUpdate : mismatch;
                ++mEffectRebuildReasons[static_cast<std::size_t>(reason)];
                if (mEffectDiagnosticExamples)
                {
                    --mEffectDiagnosticExamples;
                    Debug::RuntimeDiagnostics::recordEvent("effect_rebuild", immediateEffectMismatchName(reason), effect.identity, {
                        {"old_vertices", resident.contract.mesh.positions.size()}, {"new_vertices", effect.mesh.positions.size()},
                        {"old_indices", resident.contract.mesh.indices.size()}, {"new_indices", effect.mesh.indices.size()},
                        {"old_textures", resident.contract.textures.size()}, {"new_textures", effect.textures.size()},
                        {"completed_frame", mCompletedThrough ? mCompletedThrough->value() : 0} });
                    if (reason == ImmediateEffectMismatch::Material)
                        Debug::RuntimeDiagnostics::recordEvent("effect_material_delta", "float32_bit_patterns", effect.identity, {
                            {"old_alpha", std::bit_cast<std::uint32_t>(resident.contract.material.alpha)},
                            {"new_alpha", std::bit_cast<std::uint32_t>(effect.material.alpha)},
                            {"diffuse_changed", resident.contract.material.diffuse != effect.material.diffuse},
                            {"emission_changed", resident.contract.material.emission != effect.material.emission},
                            {"specular_changed", resident.contract.material.specular != effect.material.specular},
                            {"ambient_changed", resident.contract.material.ambient != effect.material.ambient},
                            {"source_identity_changed", resident.contract.material.sourceIdentity != effect.material.sourceIdentity},
                            {"texture_bindings_changed", resident.contract.material.textures != effect.material.textures}});
                    if (!effect.textures.empty())
                        Debug::RuntimeDiagnostics::recordEvent("effect_texture", "first_stage_example", effect.textures.front().texture.sourceIdentity,
                            {{"role", static_cast<std::uint64_t>(effect.textures.front().binding.role)},
                                {"uv_set", effect.textures.front().binding.transform.uvSet},
                                {"color_space", static_cast<std::uint64_t>(effect.textures.front().binding.colorSpace)},
                                {"width", effect.textures.front().texture.width}, {"height", effect.textures.front().texture.height}});
                }
            }
            ImmediateEffectRealization realized
                = realizeImmediateEffectDraw(effect, mTextureResolver, frameSharedObjects);
            if (!realized.valid())
            {
                mLastDiagnostic = realized.diagnostic.empty()
                    ? "evaluated gameplay effect could not be realized by the VSG compatibility path"
                    : realized.diagnostic;
                return false;
            }
            auto placed = vsg::MatrixTransform::create(toVsgMatrix(effect.worldTransform));
            placed->addChild(realized.root);
            const bool castsShadow
                = hasSemanticFlag(effect.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster);
            auto published = maskedNode(placementMask(castsShadow, effect.semanticFlags), placed);
            nextRoot->addChild(published);
            resident = ImmediateEffectResident{ effect, std::move(placed), std::move(published),
                std::move(realized.mutableDraws) };
            pendingCompile->addChild(resident.published);
            ++rebuiltEffects;
        }
        if (strictQcEnabled() && !frame.immediateEffectDraws().empty())
            Log(Debug::Info) << "V4 strict QC effect residency reused=" << reusedEffects
                             << " rebuilt=" << rebuiltEffects
                             << " retained=" << mImmediateEffectResidents.size();
        // Keep graph censuses sparse even when a busy exterior rebuilds many
        // effects every frame. Failures always get a separate final census.
        const bool allocationDiagnostic = Debug::GameplayDiagnostics::detailedSampling();
        if (allocationDiagnostic)
            Debug::GameplayDiagnostics::recordEvent("dynamic_allocation", {{"phase", "before"},
                {"summary", allocationSummary(*nextRoot, *mWindow->getOrCreateDevice())}}, true);
        const vsg::CompileResult compileResult = [&] {
            // Reused residents already own compiled resources in every live
            // context. TransferTask retains their dynamic buffer registrations
            // and observes dirty() writes without another CompileManager walk.
            // A newly registered view compiles its complete root separately.
            auto compileRoot = std::getenv("OPENMW_V4_RECOMPILE_DYNAMIC_CONTROL") ? nextRoot : pendingCompile;
            if (compileRoot->children.empty())
            {
                vsg::CompileResult empty;
                empty.result = VK_SUCCESS;
                return empty;
            }
            mLastCompiledDynamicRootCount = compileRoot->children.size();
            Debug::GameplayDiagnostics::Stage compileDiagnostic("dynamic_compile");
            return compileForViewer(*mViewer, compileRoot);
        }();
        if (!compileResult)
        {
            mLastDiagnostic = compileFailureDiagnostic("incremental VSG actor/effect graph", compileResult);
            const std::string allocations = allocationSummary(*nextRoot, *mWindow->getOrCreateDevice());
            Log(Debug::Error) << "V4 failed dynamic allocation: " << allocations;
            Debug::GameplayDiagnostics::recordEvent("dynamic_allocation", {{"phase", "failed"},
                {"summary", allocations}}, true);
            return false;
        }
        if (allocationDiagnostic)
            Debug::GameplayDiagnostics::recordEvent("dynamic_allocation", {{"phase", "after"},
                {"summary", allocationSummary(*nextRoot, *mWindow->getOrCreateDevice())}}, true);

        if (mDynamicLastUse && mDynamicPublishedRoot)
        {
            mDynamicRetirements.reserveAdditional(1);
            if (!mDynamicRetirements.queue(*mDynamicLastUse, mDynamicPublishedRoot))
            {
                mLastDiagnostic = "previous dynamic actor graph could not be retained through GPU completion";
                return false;
            }
        }
        if (!mDynamicRoot)
        {
            mLastDiagnostic = "stable dynamic publication holder is missing";
            return false;
        }
        mDynamicRoot->children.clear();
        mDynamicRoot->addChild(nextRoot);
        mDynamicPublishedRoot = std::move(nextRoot);
        mDynamicLastUse.reset();
        mImmediateEffectResidents.collectUnused();
        mDynamicActorResidents.collectUnused();
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("residency", {{"actors_reused", std::to_string(reusedActors)},
                {"actors_rebuilt", std::to_string(rebuiltActors)}, {"actor_versions", std::to_string(mDynamicActorResidents.size())},
                {"effects_reused", std::to_string(reusedEffects)}, {"effects_rebuilt", std::to_string(rebuiltEffects)},
                {"effect_versions", std::to_string(mImmediateEffectResidents.size())}});
        return true;
    }

    bool VsgRuntimeHost::synchronizeLocalLights(const RenderCore::RenderWorld& world)
    {
        bool allCurrent = mOpenMwViewState->localLightsCurrent(world)
            && (!mReflectionView || mReflectionView->state->localLightsCurrent(world))
            && (!mRefractionView || mRefractionView->state->localLightsCurrent(world));
        for (const AuxiliaryViewRuntime& auxiliary : mAuxiliaryViews)
        {
            if (auxiliary.active && auxiliary.kind != RenderCore::ViewKind::Map
                && !auxiliary.state->localLightsCurrent(world))
                allCurrent = false;
        }
        if (allCurrent)
            return true;

        LocalLightBufferPlan plan = buildLocalLightBufferPlan(buildLocalLightWorldPlan(world), glm::dvec3(0.0));
        if (!plan.ready())
        {
            switch (plan.status)
            {
                case LocalLightBufferStatus::UnsupportedModulation:
                    mLastDiagnostic = "modulated local lights require the temporal light compatibility facet";
                    break;
                case LocalLightBufferStatus::UnsupportedSpotLight:
                    mLastDiagnostic = "spot lights require the directional cone compatibility facet";
                    break;
                case LocalLightBufferStatus::RelativePositionOutOfRange:
                    mLastDiagnostic = "local light position cannot be represented in the render coordinate frame";
                    break;
                case LocalLightBufferStatus::CapacityExceeded:
                    mLastDiagnostic = "active local light count exceeds the bounded compatibility buffer";
                    break;
                case LocalLightBufferStatus::InvalidWorldPlan:
                case LocalLightBufferStatus::Ready:
                    mLastDiagnostic = "local light world plan is invalid";
                    break;
            }
            return false;
        }
        if (!mOpenMwViewState->setLocalLights(plan)
            || (mReflectionView && !mReflectionView->state->setLocalLights(plan))
            || (mRefractionView && !mRefractionView->state->setLocalLights(plan)))
        {
            mLastDiagnostic = "OpenMW view state rejected its prepared local light buffer";
            return false;
        }
        for (AuxiliaryViewRuntime& auxiliary : mAuxiliaryViews)
        {
            if (auxiliary.active && auxiliary.kind != RenderCore::ViewKind::Map
                && !auxiliary.state->setLocalLights(plan))
            {
                mLastDiagnostic = "auxiliary OpenMW view state rejected its prepared local light buffer";
                return false;
            }
        }
        return true;
    }

    bool VsgRuntimeHost::synchronizeAuxiliaryViews(const RenderCore::FrameRenderState& frame)
    {
        for (AuxiliaryViewRuntime& runtime : mAuxiliaryViews)
        {
            runtime.active = false;
            if (runtime.commandVisibility)
                runtime.commandVisibility->setAllChildren(false);
            if (runtime.view)
                runtime.view->mask = vsg::MASK_OFF;
        }

        std::size_t requestedCount = 0;
        std::uint64_t requestedPixels = 0;
        const RenderCore::RenderPassDesc* const present = [&]() -> const RenderCore::RenderPassDesc* {
            const auto found = std::find_if(frame.renderPasses().begin(), frame.renderPasses().end(),
                [](const RenderCore::RenderPassDesc& pass) { return pass.present; });
            return found == frame.renderPasses().end() ? nullptr : &*found;
        }();
        if (!present)
        {
            mLastDiagnostic = "auxiliary view validation could not resolve the main present pass";
            return false;
        }

        for (const RenderCore::FrameView& view : frame.views())
        {
            if (!isGenericAuxiliaryView(view.kind))
                continue;
            if (++requestedCount > MaximumAuxiliaryViews)
            {
                mLastDiagnostic = "auxiliary view count exceeds the bounded CP4F target pool";
                return false;
            }
            const std::uint64_t pixels = static_cast<std::uint64_t>(view.extent.width) * view.extent.height;
            if (pixels == 0 || pixels > MaximumAuxiliaryPixels
                || requestedPixels > MaximumAuxiliaryPixels - pixels)
            {
                mLastDiagnostic = "auxiliary view pixels exceed the bounded CP4F target budget";
                return false;
            }
            requestedPixels += pixels;

            const auto target = std::find_if(frame.renderTargets().begin(), frame.renderTargets().end(),
                [&](const RenderCore::RenderTargetDesc& value) { return value.identity == view.outputTarget; });
            const auto pass = std::find_if(frame.renderPasses().begin(), frame.renderPasses().end(),
                [&](const RenderCore::RenderPassDesc& value) { return value.view && *value.view == view.identity; });
            if (target == frame.renderTargets().end() || pass == frame.renderPasses().end()
                || target->kind != RenderCore::RenderTargetKind::Offscreen || target->extent != view.extent
                || target->sampleCount != 1 || target->transient || target->historyValid || view.temporal
                || view.historyValid || view.semanticIncludeMask != ~std::uint64_t{ 0 }
                || view.semanticExcludeMask != 0 || pass->present || pass->output != view.outputTarget
                || !pass->inputs.empty() || !pass->dependencies.empty()
                || !vsgProjectionCompatible(view.current.projection)
                || !vsgProjectionCompatible(view.previous.projection)
                || (target->colorFormat != RenderCore::RenderTargetFormat::Rgba8Srgb
                    && target->colorFormat != RenderCore::RenderTargetFormat::Rgba16Float)
                || (target->depthFormat && *target->depthFormat != RenderCore::RenderTargetFormat::Depth32Float))
            {
                mLastDiagnostic = "generic auxiliary view is outside the bounded CP4F Vulkan contract";
                return false;
            }
            const bool inputListed = std::find(present->inputs.begin(), present->inputs.end(), view.outputTarget)
                != present->inputs.end();
            const bool dependencyListed = std::find(present->dependencies.begin(), present->dependencies.end(), pass->identity)
                != present->dependencies.end();
            if (inputListed != dependencyListed)
            {
                mLastDiagnostic = "auxiliary target sampling dependency is incomplete";
                return false;
            }

            auto runtime = std::find_if(mAuxiliaryViews.begin(), mAuxiliaryViews.end(),
                [&](const AuxiliaryViewRuntime& value) { return value.identity == view.identity; });
            if (runtime == mAuxiliaryViews.end())
            {
                if (mAuxiliaryViews.size() >= MaximumAuxiliaryViews)
                {
                    mLastDiagnostic = "persistent auxiliary target pool is exhausted";
                    return false;
                }
                std::uint64_t residentPixels = 0;
                for (const AuxiliaryViewRuntime& resident : mAuxiliaryViews)
                {
                    const std::uint64_t residentSurfacePixels
                        = static_cast<std::uint64_t>(resident.target.extent.width) * resident.target.extent.height;
                    if (residentSurfacePixels > MaximumAuxiliaryPixels
                        || residentPixels > MaximumAuxiliaryPixels - residentSurfacePixels)
                    {
                        mLastDiagnostic = "persistent auxiliary target residency exceeds the bounded CP4F pixel budget";
                        return false;
                    }
                    residentPixels += residentSurfacePixels;
                }
                if (residentPixels > MaximumAuxiliaryPixels - pixels)
                {
                    mLastDiagnostic = "persistent auxiliary target pixel budget is exhausted";
                    return false;
                }

                waitIdle();
                AuxiliaryViewRuntime created;
                created.identity = view.identity;
                created.targetIdentity = view.outputTarget;
                created.kind = view.kind;
                created.colorFormat = target->colorFormat;
                created.depthFormat = target->depthFormat;
                created.target = createOffscreenRenderTarget(mWindow->getOrCreateDevice(), view.extent,
                    target->colorFormat, target->depthFormat);
                if (!created.target)
                {
                    mLastDiagnostic = "Vulkan auxiliary offscreen target allocation failed";
                    return false;
                }
                created.commandVisibility = vsg::Switch::create();
                if (!created.commandVisibility)
                {
                    mLastDiagnostic = "Vulkan auxiliary view could not create its command visibility switch";
                    return false;
                }
                created.commandVisibility->addChild(false, created.target.renderGraph);
                created.camera = FrameCameraObjects::create(view);
                created.view = vsg::View::create(
                    created.camera.camera, vsg::ref_ptr<vsg::Node>{}, vsg::RECORD_LIGHTS);
                created.state = OpenMwViewDependentState::create(created.view.get());
                created.state->shaderSet = createLegacyCompatibilityShaderSet();
                if (!created.state->shaderSet)
                {
                    mLastDiagnostic = "Vulkan auxiliary view could not create the OpenMW shader contract";
                    return false;
                }
                created.view->viewDependentState = created.state;
                if (view.kind == RenderCore::ViewKind::Map)
                {
                    // Match LocalMapRenderToTexture::setDefaults(): one fixed
                    // 0.3 ambient plus a fixed 0.7 directional light, no local
                    // lights or shadows, and the static/simple-water mask only.
                    created.ambientLight = vsg::AmbientLight::create();
                    created.sunLight = vsg::DirectionalLight::create();
                    if (!created.ambientLight || !created.sunLight)
                    {
                        mLastDiagnostic = "Vulkan map view could not create its legacy fixed lights";
                        return false;
                    }
                    created.ambientLight->name = "OpenMW local-map ambient";
                    created.ambientLight->color.set(0.3f, 0.3f, 0.3f);
                    created.ambientLight->intensity = 1.0f;
                    created.sunLight->name = "OpenMW local-map sun";
                    created.sunLight->color.set(0.7f, 0.7f, 0.7f);
                    created.sunLight->intensity = 1.0f;
                    // VSG stores the ray direction; the compatibility shader
                    // negates it to obtain the surface-to-light vector used by
                    // the legacy OSG light at (-0.3,-0.3,+0.7,0).
                    created.sunLight->direction.set(0.3f, 0.3f, -0.7f);
                    created.view->addChild(created.ambientLight);
                    created.view->addChild(created.sunLight);
                    created.view->addChild(mStaticRoot);
                    if (mOptions.water.enabled)
                    {
                        created.waterSurface = WaterSurface::create({}, {});
                        if (!created.waterSurface)
                        {
                            mLastDiagnostic = "Vulkan map view could not create its simple-water compatibility surface";
                            return false;
                        }
                        created.view->addChild(created.waterSurface.node());
                    }
                }
                else
                {
                    created.view->addChild(mAmbientLight);
                    created.view->addChild(mSunLight);
                    created.view->addChild(mSceneRoot);
                }
                created.view->bins = createStaticConformanceBins();
                created.target.renderGraph->addChild(created.view);
                if (!created.target.renderGraph->framebuffer)
                {
                    mLastDiagnostic = "Vulkan auxiliary view has no framebuffer compilation context";
                    return false;
                }
                // The registration owns exactly the contexts appended by VSG,
                // including any nested shadow contexts. Its destructor removes
                // them on retirement AND every unsuccessful publication path.
                // Keep the old lifetime as an explicit, guarded regression control.
                const bool retainContextControl = std::getenv("OPENMW_V4_RETAIN_AUXILIARY_CONTEXTS") != nullptr;
                if (!retainContextControl)
                    created.compilation = static_cast<ViewCompileManager&>(*mViewer->compileManager)
                        .registerFramebufferView(*created.target.renderGraph->framebuffer, created.view);
                const vsg::CompileResult auxiliaryCompile = retainContextControl
                    ? compileForNewFramebufferView(*mViewer, *created.target.renderGraph->framebuffer,
                        created.view, created.target.renderGraph)
                    : compileForViewerView(*mViewer, *created.view, created.target.renderGraph);
                if (!auxiliaryCompile)
                {
                    mLastDiagnostic = compileFailureDiagnostic(
                        "incremental Vulkan auxiliary framebuffer/view graph", auxiliaryCompile);
                    return false;
                }
                if (!graphicsPipelinesRealizedForView(*created.target.renderGraph, *created.view))
                {
                    mLastDiagnostic
                        = "Vulkan auxiliary view compilation left a graphics pipeline unrealized for its view id";
                    return false;
                }
                // Water targets are already before the swapchain graph. Insert
                // generic sampled surfaces immediately before the exact main graph so
                // their final shader-read layout is established before MyGUI without
                // relying on incidental command-graph child ordering.
                const auto mainGraph = std::find(
                    mCommandGraph->children.begin(), mCommandGraph->children.end(), mRenderGraph);
                if (mainGraph == mCommandGraph->children.end())
                {
                    mLastDiagnostic = "Vulkan auxiliary view could not resolve the main command-graph slot";
                    return false;
                }
                mCommandGraph->children.insert(mainGraph, created.commandVisibility);
                mAuxiliaryViews.push_back(std::move(created));
                runtime = std::prev(mAuxiliaryViews.end());
            }
            else if (runtime->targetIdentity != view.outputTarget || runtime->kind != view.kind
                || runtime->target.extent != view.extent || runtime->colorFormat != target->colorFormat
                || runtime->depthFormat != target->depthFormat)
            {
                mLastDiagnostic = "auxiliary view identity changed target shape; restart is required to preserve sampled-image lifetime";
                return false;
            }

            if (!runtime->commandVisibility)
            {
                mLastDiagnostic = "persistent auxiliary view lost its command visibility switch";
                return false;
            }
            runtime->active = true;
            runtime->commandVisibility->setAllChildren(true);
            runtime->view->mask = vsg::MASK_ALL;
            runtime->camera.update(view);
            runtime->view->LODScale = view.lodScale;
            if (strictQcEnabled())
            {
                const glm::dmat4 authoredView(view.current.view);
                const glm::dvec3 matrixCamera(glm::inverse(authoredView)[3]);
                const glm::dvec3 x(authoredView[0]);
                const glm::dvec3 y(authoredView[1]);
                const glm::dvec3 z(authoredView[2]);
                const double orthogonalityError = std::max(
                    { std::abs(glm::dot(x, y)), std::abs(glm::dot(x, z)), std::abs(glm::dot(y, z)),
                        std::abs(glm::length(x) - 1.0), std::abs(glm::length(y) - 1.0),
                        std::abs(glm::length(z) - 1.0) });
                Log(Debug::Info) << "V4 strict QC auxiliary kind=" << viewKindName(view.kind)
                                 << " semantic=" << view.identity.slot() << ':' << view.identity.generation()
                                 << " vsg=" << runtime->view->viewID << " target=" << view.outputTarget.slot()
                                 << ':' << view.outputTarget.generation() << " extent=" << view.extent.width << 'x'
                                 << view.extent.height << " cameraAuthored=(" << view.current.worldPosition.x << ','
                                 << view.current.worldPosition.y << ',' << view.current.worldPosition.z
                                 << ") cameraFromView=(" << matrixCamera.x << ',' << matrixCamera.y << ','
                                 << matrixCamera.z << ") cameraDelta="
                                 << glm::length(matrixCamera - view.current.worldPosition) << " viewDet="
                                 << glm::determinant(glm::dmat3(authoredView)) << " viewOrthoError="
                                 << orthogonalityError << " staticChildren="
                                 << (mStaticRoot ? mStaticRoot->children.size() : 0u);
            }
            RenderCore::FrameEnvironmentState auxiliaryEnvironment = frame.environment();
            if (view.kind == RenderCore::ViewKind::Map)
            {
                auxiliaryEnvironment.ambient = { 0.3f, 0.3f, 0.3f, 1.0f };
                auxiliaryEnvironment.fogEnabled = false;
                auxiliaryEnvironment.sunDiffuse = { 0.7f, 0.7f, 0.7f, 1.0f };
                auxiliaryEnvironment.sunSpecular = { 0.0f, 0.0f, 0.0f, 0.0f };
                auxiliaryEnvironment.sunLightEnabled = true;
                auxiliaryEnvironment.sunVisible = false;
                auxiliaryEnvironment.shadowsEnabled = false;
                auxiliaryEnvironment.clusteredLocalLighting = false;
                auxiliaryEnvironment.skyEnabled = false;
                // The legacy map cull mask includes Mask_SimpleWater. Preserve
                // the cell's water enable/height, but never treat the overhead
                // map camera as underwater and never sample gameplay RTTs.
                auxiliaryEnvironment.underwater = false;
                if (runtime->waterSurface)
                    runtime->waterSurface.update(auxiliaryEnvironment, view, frame.simulationTime());
            }
            runtime->state->setRadiusFadeEnabled(auxiliaryEnvironment.localLightRadiusFade);
            runtime->state->setEnvironment(auxiliaryEnvironment, view.current.projection);
            runtime->state->setClipPlane(view.clipPlane, view.current);
            const RenderCore::Color clear = view.kind == RenderCore::ViewKind::Map
                ? RenderCore::Color{ 0.0f, 0.0f, 0.0f, 1.0f }
                : (auxiliaryEnvironment.skyEnabled ? auxiliaryEnvironment.skyColor : auxiliaryEnvironment.fogColor);
            runtime->target.renderGraph->setClearValues(
                { { clear.r, clear.g, clear.b, clear.a } }, { 0.0f, 0 });
        }
        return true;
    }

    bool VsgRuntimeHost::prepareGui()
    {
        mGuiPreparedRoot = {};
        mGuiPrepared = false;
        if (!mGuiRenderer)
        {
            mGuiPrepared = true;
            return true;
        }
        mGuiRenderer->collect();
        vsg::ref_ptr<vsg::Node> overlay = mGuiRenderer->buildPersistentOverlay();
        auto nextRoot = vsg::Group::create();
        if (overlay)
            nextRoot->addChild(overlay);
        if (!mView || !compileForViewerView(*mViewer, *mView, nextRoot))
        {
            mLastDiagnostic = "incremental VSG MyGUI main-view compilation failed before overlay publication";
            return false;
        }
        if (overlay && mGuiRenderer->batchCount() != 0 && !mUiPipeline.realizedForView(mView->viewID))
        {
            mLastDiagnostic = "VSG MyGUI main-view graphics pipeline was not realized before overlay publication";
            return false;
        }
        mGuiPreparedRoot = std::move(nextRoot);
        mGuiPrepared = true;
        return true;
    }

    bool VsgRuntimeHost::synchronizeGui()
    {
        // GUI-only transition frames do not pass through the gameplay coordinator,
        // so retain a safe synchronous fallback. Ordinary gameplay frames arrive
        // with an immutable generation prepared before Lua was released.
        if (!mGuiPrepared && !prepareGui())
            return false;
        mGuiPrepared = false;
        vsg::ref_ptr<vsg::Group> nextRoot = std::move(mGuiPreparedRoot);
        if (!mGuiRenderer)
            return true;
        if (!nextRoot)
        {
            mLastDiagnostic = "prepared MyGUI generation is missing";
            return false;
        }
        if (mGuiLastUse && mGuiPublishedRoot)
        {
            mGuiRetirements.reserveAdditional(1);
            if (!mGuiRetirements.queue(*mGuiLastUse, mGuiPublishedRoot))
            {
                mLastDiagnostic = "previous MyGUI graph could not be retained through GPU completion";
                return false;
            }
        }
        if (!mGuiRoot)
        {
            mLastDiagnostic = "stable MyGUI publication holder is missing";
            return false;
        }
        mGuiRoot->children.clear();
        mGuiRoot->addChild(nextRoot);
        mGuiPublishedRoot = std::move(nextRoot);
        mGuiLastUse.reset();
        return true;
    }

    bool VsgRuntimeHost::ensureActiveGraphicsPipelinesRealized()
    {
        struct ActiveView
        {
            std::string_view family;
            vsg::ref_ptr<vsg::View> view;
        };

        std::vector<ActiveView> views;
        views.push_back({ "main", mView });
        if (mReflectionView && mReflectionView->view->mask != vsg::MASK_OFF)
            views.push_back({ "reflection", mReflectionView->view });
        if (mRefractionView && mRefractionView->view->mask != vsg::MASK_OFF)
            views.push_back({ "refraction", mRefractionView->view });
        for (const AuxiliaryViewRuntime& auxiliary : mAuxiliaryViews)
        {
            if (auxiliary.active && auxiliary.view && auxiliary.view->mask != vsg::MASK_OFF)
                views.push_back({ "auxiliary", auxiliary.view });
        }
        // ViewDependentState records cascaded shadows through a hidden
        // pre-render CommandGraph. Those Views are not children of
        // mCommandGraph, but they record the same scene with a distinct shared
        // viewID and therefore require their own pipeline implementations.
        if (mOpenMwViewState && !mOpenMwViewState->shadowMaps.empty())
        {
            const auto& shadow = mOpenMwViewState->shadowMaps.front();
            if (shadow.view && shadow.view->mask != vsg::MASK_OFF)
                views.push_back({ "shadow", shadow.view });
        }

        if (strictQcEnabled())
        {
            std::uint64_t signature = 0;
            std::ostringstream inventory;
            inventory << "V4 strict QC active views=" << views.size();
            for (const ActiveView& active : views)
            {
                if (!active.view)
                    continue;
                hashCombine(signature, active.view->viewID);
                hashCombine(signature, std::hash<std::string_view>{}(active.family));
                inventory << " [" << active.family << " vsg=" << active.view->viewID << ']';
            }
            if (!mStrictQcLastViewSignature || *mStrictQcLastViewSignature != signature)
            {
                Log(Debug::Info) << inventory.str();
                mStrictQcLastViewSignature = signature;
            }
        }

        std::vector<std::string> unresolved;
        for (const ActiveView& active : views)
        {
            if (!active.view)
                continue;
            GraphicsPipelineAudit audit = auditGraphicsPipelinesForView(*active.view, *active.view);
            if (audit.valid())
                continue;

            if (strictQcEnabled())
                Log(Debug::Warning) << "V4 strict QC repairing " << audit.unrealized.size()
                                    << " unrealized pipeline(s) for " << active.family << " view "
                                    << active.view->viewID;

            // A graph can acquire new immutable draws after a view's context was
            // introduced. Compile the exact active view once, then census it
            // again before record traversal reaches VSG's unchecked vk(viewID).
            const vsg::CompileResult repair = compileForViewerView(*mViewer, *active.view, active.view);
            if (repair)
                audit = auditGraphicsPipelinesForView(*active.view, *active.view);
            if (audit.valid())
                continue;

            for (const std::string& issue : audit.unrealized)
            {
                std::ostringstream message;
                message << active.family << ": " << issue;
                if (!repair)
                    message << " (compile result " << repair.result << ": " << repair.message << ')';
                unresolved.push_back(message.str());
            }
        }

        if (unresolved.empty())
            return true;
        std::ostringstream diagnostic;
        diagnostic << "Vulkan pre-submit pipeline census found " << unresolved.size()
                   << " unrealized active pipeline(s)";
        // Keep the fatal dialog usable for a large exterior. The count remains
        // exact, while a bounded representative set identifies the failed view.
        const auto displayed = std::min<std::size_t>(unresolved.size(), 8);
        for (std::size_t i = 0; i < displayed; ++i)
            diagnostic << "; " << unresolved[i];
        if (displayed < unresolved.size())
            diagnostic << "; " << (unresolved.size() - displayed) << " additional pipeline(s) omitted";
        mLastDiagnostic = diagnostic.str();
        return false;
    }

    void VsgRuntimeHost::reportRuntimeMemory(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame) noexcept
    {
        if (!mRuntimeDiagnosticSampler.due()) return;
        const auto start = Debug::RuntimeDiagnostics::nowUs();
        mEffectDiagnosticExamples = 8;
        try
        {
            reportNeutralMemory(world);
            reportVulkanMemory(*mWindow->getOrCreatePhysicalDevice(), *mWindow->getOrCreateDevice());
            std::uint64_t effectBytes = 0, effectWritable = 0, effectInFlight = 0, selected = 0;
            mImmediateEffectResidents.inspect([&](const auto&, const ImmediateEffectResident& resident,
                bool chosen, bool writable, const auto&) {
                effectBytes += meshCapacityBytes(resident.contract.mesh);
                selected += chosen;
                effectWritable += writable;
                effectInFlight += !writable;
            });
            std::uint64_t actorWritable = 0, actorInFlight = 0;
            mDynamicActorResidents.inspect([&](const auto&, const auto&, bool, bool writable, const auto&) {
                actorWritable += writable; actorInFlight += !writable;
            });
            std::uint64_t frameBytes = 0;
            for (const auto& draw : frame.immediateEffectDraws()) frameBytes += meshCapacityBytes(draw.mesh);
            Debug::RuntimeDiagnostics::recordEvent("resident_versions", "dynamic", "Before frame sync; completed slots retained for bounded reuse are normal", {
                {"effect_versions", mImmediateEffectResidents.size()}, {"effect_writable", effectWritable},
                {"effect_in_flight", effectInFlight}, {"effect_selected_previous", selected},
                {"effect_contract_mesh_capacity_bytes", effectBytes}, {"frame_effect_mesh_capacity_bytes", frameBytes},
                {"actor_versions", mDynamicActorResidents.size()}, {"actor_writable", actorWritable},
                {"actor_in_flight", actorInFlight}, {"completed_frame", mCompletedThrough ? mCompletedThrough->value() : 0},
                {"static_instances", residentStaticInstanceCount()} });
            const auto retirement = [&](const char* name, const auto& queue, std::uint64_t released) {
                std::uint64_t oldest = 0, completed = 0;
                queue.inspect([&](RenderCore::FrameId lastUse, const auto&) {
                    if (!oldest || lastUse.value() < oldest) oldest = lastUse.value();
                    completed += mCompletedThrough && lastUse <= *mCompletedThrough;
                });
                Debug::RuntimeDiagnostics::recordEvent("retirement", name, "Before completion collection; graph roots, not unique allocation bytes", {
                    {"pending_roots", queue.size()}, {"oldest_last_use_frame", oldest},
                    {"completed_roots_pending_collection", completed}, {"released_roots", released},
                    {"completed_frame", mCompletedThrough ? mCompletedThrough->value() : 0} });
            };
            retirement("dynamic", mDynamicRetirements, mDiagnosticReleasedDynamic);
            retirement("gui", mGuiRetirements, mDiagnosticReleasedGui);
            for (std::size_t i = 0; i < mEffectRebuildReasons.size(); ++i)
                if (mEffectRebuildReasons[i])
                    Debug::RuntimeDiagnostics::recordEvent("effect_rebuild_count", immediateEffectMismatchName(static_cast<ImmediateEffectMismatch>(i)), {},
                        {{"count", mEffectRebuildReasons[i]}});
            for (const auto& auxiliary : mAuxiliaryViews)
                Debug::RuntimeDiagnostics::recordEvent("auxiliary", "vsg_target", {}, {
                    {"target_slot", auxiliary.targetIdentity.slot()}, {"target_generation", auxiliary.targetIdentity.generation()},
                    {"kind", static_cast<std::uint64_t>(auxiliary.kind)}, {"active", auxiliary.active},
                    {"width", auxiliary.target.extent.width}, {"height", auxiliary.target.extent.height},
                    {"semantic_color_format", static_cast<std::uint64_t>(auxiliary.colorFormat)},
                    {"image_present", static_cast<bool>(auxiliary.target.color)}});
            Debug::RuntimeDiagnostics::recordEvent("probe_cost", "vsg_memory", "Counts and payload capacity, not GPU execution time",
                {{"elapsed_us", Debug::RuntimeDiagnostics::nowUs() - start}});
        }
        catch (...) { Debug::RuntimeDiagnostics::recordEvent("coverage", "vsg_memory", "snapshot unavailable", {{"available", 0}}); }
    }

    void VsgRuntimeHost::reportStrictFrameDiagnostics(
        const RenderCore::FrameRenderState& frame, const RenderCore::FrameView& mainView)
    {
        if (!strictQcEnabled())
            return;

        std::uint64_t signature = 0;
        hashCombine(signature, frame.worldEpoch().value());
        hashCombine(signature, frame.views().size());
        hashCombine(signature, frame.renderTargets().size());
        hashCombine(signature, frame.renderPasses().size());
        hashCombine(signature, frame.dynamicTransforms().size());
        hashCombine(signature, frame.skeletonPoses().size());
        hashCombine(signature, frame.morphWeights().size());
        hashCombine(signature, frame.immediateEffectDraws().size());
        hashCombine(signature, frame.environment().interior ? 1 : 0);
        hashCombine(signature, glm::length(mainView.current.worldPosition) < 1e-3 ? 0 : 1);
        const bool initialSample = mStrictQcInitialFramesReported < 12;
        if (!initialSample && mStrictQcLastFrameSignature && *mStrictQcLastFrameSignature == signature)
            return;
        if (initialSample)
            ++mStrictQcInitialFramesReported;
        mStrictQcLastFrameSignature = signature;

        const glm::dmat4 view(mainView.current.view);
        const glm::dmat4 cameraWorld = glm::inverse(view);
        const glm::dvec3 matrixCamera(cameraWorld[3]);
        const double cameraDelta = glm::length(matrixCamera - mainView.current.worldPosition);
        const glm::dvec3 x(view[0]);
        const glm::dvec3 y(view[1]);
        const glm::dvec3 z(view[2]);
        const double orthogonalityError = std::max(
            { std::abs(glm::dot(x, y)), std::abs(glm::dot(x, z)), std::abs(glm::dot(y, z)),
                std::abs(glm::length(x) - 1.0), std::abs(glm::length(y) - 1.0),
                std::abs(glm::length(z) - 1.0) });
        const double determinant = glm::determinant(glm::dmat3(view));

        double nearestDynamic = std::numeric_limits<double>::infinity();
        double farthestDynamic = 0.0;
        for (const RenderCore::DynamicTransformState& dynamic : frame.dynamicTransforms())
        {
            const double distance = glm::length(dynamic.current.translation - mainView.current.worldPosition);
            nearestDynamic = std::min(nearestDynamic, distance);
            farthestDynamic = std::max(farthestDynamic, distance);
        }

        std::ostringstream report;
        report << "V4 strict QC frame=" << frame.frameId().value() << " world=" << frame.worldEpoch().value()
               << '/' << frame.renderWorldRevision().value() << " main={semantic=" << mainView.identity.slot() << ':'
               << mainView.identity.generation() << " vsg=" << (mView ? mView->viewID : 0) << " kind="
               << viewKindName(mainView.kind) << " extent=" << mainView.extent.width << 'x' << mainView.extent.height
               << " cameraAuthored=(" << mainView.current.worldPosition.x << ',' << mainView.current.worldPosition.y
               << ',' << mainView.current.worldPosition.z << ") cameraFromView=(" << matrixCamera.x << ','
               << matrixCamera.y << ',' << matrixCamera.z << ") cameraDelta=" << cameraDelta
               << " viewDet=" << determinant << " viewOrthoError=" << orthogonalityError << "} graph={views="
               << frame.views().size() << " targets=" << frame.renderTargets().size() << " passes="
               << frame.renderPasses().size() << "} dynamic={transforms=" << frame.dynamicTransforms().size()
               << " skeletons=" << frame.skeletonPoses().size() << " morphs=" << frame.morphWeights().size()
               << " effects=" << frame.immediateEffectDraws().size();
        if (!frame.dynamicTransforms().empty())
            report << " nearest=" << nearestDynamic << " farthest=" << farthestDynamic;
        report << "} environment={interior=" << frame.environment().interior << " sky="
               << frame.environment().skyEnabled << " water=" << frame.environment().waterEnabled << "}";
        Log(cameraDelta > 1.0 || orthogonalityError > 1e-3 || std::abs(std::abs(determinant) - 1.0) > 1e-3
                ? Debug::Warning
                : Debug::Info)
            << report.str();
    }

    RenderCore::RenderFrameResult VsgRuntimeHost::renderFrame(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame)
    {
        return renderFrameImpl(world, frame, false);
    }

    RenderCore::RenderFrameResult VsgRuntimeHost::renderGuiFrame(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame)
    {
        return renderFrameImpl(world, frame, true);
    }

    RenderCore::RenderFrameResult VsgRuntimeHost::renderFrameImpl(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame, bool guiOnly)
    {
        mLastDiagnostic.clear();
        if (guiOnly && (frame.environment().skyEnabled || frame.environment().waterEnabled
                || frame.environment().sunLightEnabled || frame.environment().shadowsEnabled
                || frame.views().size() != 1 || !frame.skeletonPoses().empty()
                || !frame.immediateEffectDraws().empty()))
            return finish(RenderCore::RenderFrameResult::Failed, "GUI-only frame contains world rendering state");
        if (!frame.valid() || !RenderCore::frameCompatibleWithWorld(world, frame))
            return finish(RenderCore::RenderFrameResult::Failed, "invalid or stale semantic frame state");
        if (frame.dynamicMaterials().empty() == false)
            return finish(
                RenderCore::RenderFrameResult::Failed, "dynamic materials require a later compatibility facet");
        if (frame.environment().waterEnabled && !mOptions.water.enabled)
            return finish(RenderCore::RenderFrameResult::Failed,
                "water is present but the CP4E Vulkan water route was disabled at bootstrap");
        if (frame.environment().clusteredLocalLighting)
            return finish(RenderCore::RenderFrameResult::Failed,
                "clustered local-light selection and far-plane fading require the clustered compatibility facet");
        const RenderCore::FrameView* mainView = selectMainView(frame);
        if (!mainView || mainView->extent != frame.renderExtent() || frame.renderExtent() != frame.outputExtent())
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP3C host requires one unmasked main view with equal render and output extents");
        const RenderCore::FrameView* reflectionView = nullptr;
        const RenderCore::FrameView* refractionView = nullptr;
        if (!waterViewsCompatible(frame, reflectionView, refractionView))
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP4E water views and persistent target policy do not match the semantic frame");
        if (!shadowViewFamilyCompatible(frame))
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP4D native shadow resources do not match the semantic derived-view family");
        if (!vsgProjectionCompatible(mainView->current.projection)
            || !vsgProjectionCompatible(mainView->previous.projection) || frame.jitter() != glm::vec2(0.0f)
            || frame.projectionOffset() != glm::vec2(0.0f))
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP3C host requires explicit reversed zero-to-one/down-Y projection and no temporal jitter");

        reportStrictFrameDiagnostics(frame, *mainView);
        reportRuntimeMemory(world, frame);

        // VSG skips swapchain acquisition for invisible windows, but its record/submit
        // task still owns the window and can otherwise reuse the previous image's
        // image-available semaphore. Never submit a frame while SDL reports the
        // Vulkan window hidden or minimized.
        if (!mWindow->visible())
            return finish(RenderCore::RenderFrameResult::Skipped, "SDL Vulkan window is hidden or minimized");

        if (!synchronizeAuxiliaryViews(frame))
            return finish(RenderCore::RenderFrameResult::Failed, mLastDiagnostic);
        const VkExtent2D desiredExtent{ frame.outputExtent().width, frame.outputExtent().height };
        const auto extentMatches = [&] {
            const VkExtent2D actual = mWindow->extent2D();
            return actual.width == desiredExtent.width && actual.height == desiredExtent.height;
        };
        if (!extentMatches())
            mWindow->resize();
        if (!extentMatches())
            return finish(
                RenderCore::RenderFrameResult::Skipped, "SDL pixel extent is not ready for the requested output");
        if (!mViewer->advanceToNextFrame(frame.simulationTime()))
            return finish(RenderCore::RenderFrameResult::Skipped, "VSG could not acquire the next swapchain frame");
        // advanceToNextFrame() polls SDL/VSG events before acquisition. A minimize
        // event can therefore make the window invisible after the pre-check and
        // cause VSG to skip acquisition while still returning a valid frame. Stop
        // here before RecordAndSubmitTask can consume a stale acquire semaphore.
        if (!mWindow->visible())
            return finish(RenderCore::RenderFrameResult::Skipped, "SDL Vulkan window became hidden or minimized");

        const VsgCompletionPoll completion = mCompletion.pollBeforeRecordAndSubmit(*mViewer);
        if (completion.result != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan completion polling failed with VkResult " + std::to_string(completion.result));
        if (completion.completedThrough)
        {
            mCompletedThrough = completion.completedThrough;
            bool releasedStatic = false;
            {
                auto instances = mStaticResidency.collect(*completion.completedThrough);
                auto populations = mStaticPopulationResidency.collect(*completion.completedThrough);
                releasedStatic = !instances.empty() || !populations.empty();
            } // destruction releases fence-completed graphs before cache pruning
            if (releasedStatic)
                mSharedObjects->prune();
            const auto releasedDynamic = mDynamicRetirements.collect(*completion.completedThrough).size();
            const auto releasedGui = mGuiRetirements.collect(*completion.completedThrough).size();
            if (Debug::RuntimeDiagnostics::enabled())
            {
                mDiagnosticReleasedDynamic += releasedDynamic;
                mDiagnosticReleasedGui += releasedGui;
            }
        }
        if (!mCompletion.canRegisterSubmission(frame.frameId()))
            return finish(RenderCore::RenderFrameResult::Failed, "semantic frame id is not submit-safe");

        mCamera.update(*mainView);
        mView->LODScale = mainView->lodScale;
        const RenderCore::FrameEnvironmentState& environment = frame.environment();
        mOpenMwViewState->setRadiusFadeEnabled(environment.localLightRadiusFade);
        mOpenMwViewState->setEnvironment(environment, mainView->current.projection);
        mOpenMwViewState->setClipPlane(mainView->clipPlane, mainView->current);
        const auto updateWaterView = [&](std::optional<WaterViewRuntime>& runtime,
                                         const RenderCore::FrameView* view) {
            if (!runtime)
                return;
            runtime->view->mask = view
                ? (view->kind == RenderCore::ViewKind::Reflection ? ReflectionTraversalMask
                                                                  : RefractionTraversalMask)
                : vsg::MASK_OFF;
            if (!view)
                return;
            runtime->camera.update(*view);
            runtime->view->LODScale = view->lodScale;
            runtime->state->setRadiusFadeEnabled(environment.localLightRadiusFade);
            runtime->state->setEnvironment(environment, view->current.projection);
            runtime->state->setClipPlane(view->clipPlane, view->current);
            const RenderCore::Color& auxiliaryClear
                = environment.skyEnabled && !environment.underwater ? environment.skyColor : environment.fogColor;
            runtime->target.renderGraph->setClearValues(
                { { auxiliaryClear.r, auxiliaryClear.g, auxiliaryClear.b, auxiliaryClear.a } }, { 0.0f, 0 });
        };
        updateWaterView(mReflectionView, reflectionView);
        updateWaterView(mRefractionView, refractionView);
        mAmbientLight->color.set(environment.ambient.r, environment.ambient.g, environment.ambient.b);
        mAmbientLight->intensity = 1.0f;
        mSunLight->color.set(environment.sunDiffuse.r, environment.sunDiffuse.g, environment.sunDiffuse.b);
        mSunLight->intensity = environment.sunLightEnabled ? 1.0f : 0.0f;
        mSunLight->direction.set(environment.sunDirection.x, environment.sunDirection.y, environment.sunDirection.z);
        mSkyBackdrop.update(environment, *mainView);
        mWaterSurface.update(environment, *mainView, frame.simulationTime());
        if (mOptions.shadows.enabled)
        {
            if (environment.shadowsEnabled)
                mOpenMwViewState->shadowSettingsOverride.erase(mSunLight);
            else
                mOpenMwViewState->shadowSettingsOverride[mSunLight] = {};
        }
        // CP4D's first visible sky facet is deliberately resource-free: the
        // weather system's current sky colour owns uncovered background pixels.
        // The legacy fog clear remains the control for interiors and disabled
        // skies, while later atmosphere/cloud geometry can layer over this.
        const RenderCore::Color& clear = environment.skyEnabled ? environment.skyColor : environment.fogColor;
        mRenderGraph->setClearValues({ { clear.r, clear.g, clear.b, clear.a } });
        // Preserve existing resident ownership across GUI-only loading frames.
        // Hiding the world must not retire/rebuild it or evaluate incomplete actors.
        mSceneVisibility->setAllChildren(!guiOnly);
        if ((!guiOnly && (!synchronizeLocalLights(world) || !synchronizeStaticWorld(world)
                || !synchronizePopulationVisibility(world, *mainView) || !synchronizeDynamicActors(world, frame)))
            || !synchronizeGui())
            return finish(RenderCore::RenderFrameResult::Failed, mLastDiagnostic);

        const bool pipelinesReady = [&] {
            Debug::GameplayDiagnostics::Stage pipelineDiagnostic("pipeline_audit");
            return ensureActiveGraphicsPipelinesRealized();
        }();
        if (!pipelinesReady)
            return finish(RenderCore::RenderFrameResult::Failed, mLastDiagnostic);
        mViewer->update();
        const VsgSubmitPresentResult submission = [&] {
            Debug::GameplayDiagnostics::Stage submitDiagnostic("submit_present");
            return submitAndPresentChecked(*mViewer);
        }();
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("submission", {{"submit", std::to_string(submission.submit)},
                {"present", std::to_string(submission.present)}, {"frame_id", std::to_string(frame.frameId().value())}});
        if (submission.submit != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan record/submit failed with VkResult " + std::to_string(submission.submit));
        mWaitedIdle = false;
        if (!mCompletion.registerSubmission(*mViewer, frame.frameId())
            || (!guiOnly && (!mImmediateEffectResidents.markSubmitted(frame.frameId())
            || !mDynamicActorResidents.markSubmitted(frame.frameId())
            || !mStaticResidency.markSubmitted(frame.frameId())
            || !mStaticPopulationResidency.markSubmitted(frame.frameId()))))
        {
            // Submission has already happened. Synchronize before returning so
            // an untracked in-flight frame can never make later destruction or
            // replacement unsafe, even if an internal invariant is violated.
            waitIdle();
            return finish(RenderCore::RenderFrameResult::Failed,
                "submitted frame could not be registered; device was synchronized for safety");
        }
        if (!guiOnly)
            mDynamicLastUse = frame.frameId();
        mGuiLastUse = frame.frameId();
        if (!submission.success())
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan presentation failed with VkResult " + std::to_string(submission.present));
        return finish(RenderCore::RenderFrameResult::Presented);
    }

    RenderCore::RenderFrameResult VsgRuntimeHost::finish(RenderCore::RenderFrameResult result, std::string diagnostic)
    {
        mLastDiagnostic = std::move(diagnostic);
        return result;
    }

    void VsgRuntimeHost::waitIdle()
    {
        if (mViewer && !mWaitedIdle)
        {
            mViewer->deviceWaitIdle();
            mWaitedIdle = true;
        }
    }

    std::size_t VsgRuntimeHost::residentStaticInstanceCount() const noexcept
    {
        return mStaticResidency.residentCount();
    }

    std::size_t VsgRuntimeHost::pendingRetirementCount() const noexcept
    {
        return mStaticResidency.pendingRetirementCount() + mStaticPopulationResidency.pendingRetirementCount()
            + mDynamicRetirements.size() + mGuiRetirements.size();
    }
}
