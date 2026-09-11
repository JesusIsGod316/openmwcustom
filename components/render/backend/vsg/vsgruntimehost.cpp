#include "vsgruntimehost.hpp"

#include "dynamicactorplan.hpp"
#include "immediateeffectrealizer.hpp"
#include "legacymaterialshader.hpp"
#include "populationvisibility.hpp"
#include "staticassetconformance.hpp"

#include <components/vsgmygui/rendermanager.hpp>

#include <components/rendercore/deformation.hpp>

#include <vsg/app/CommandGraph.h>
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
#include <limits>
#include <stdexcept>
#include <string>
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
        mView->addChild(mSceneRoot);
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
                mRefractionView ? mRefractionView->target.color : nullptr);
        if (options.water.enabled && !mWaterSurface)
            throw std::runtime_error("VsgRuntimeHost could not create its CP4E water surface");
        if (mWaterSurface)
            mMainOnlyRoot->addChild(mWaterSurface.node());
        mMainOnlyRoot->addChild(mGuiRoot);
        mView->addChild(mMainOnlyRoot);
        mView->bins = createStaticConformanceBins();
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
        if (!mCommandGraph || !found->target.renderGraph)
        {
            mLastDiagnostic = "persistent auxiliary target retirement found an invalid command graph";
            return false;
        }

        waitIdle();
        const auto graph = std::find(mCommandGraph->children.begin(), mCommandGraph->children.end(), found->target.renderGraph);
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
        const StaticWorldPlan worldPlan = buildStaticWorldPlan(world, mOptions.staticPlan);
        const StaticWorldMutation mutation = mStaticResidency.prepare(world, worldPlan);
        const StaticPopulationMutation populationMutation = mStaticPopulationResidency.prepare(world, worldPlan);
        if (!mutation.valid || !populationMutation.valid || mutation.simpleMeshInstancesDeferred != 0)
        {
            mLastDiagnostic = "static world contains invalid or not-yet-supported instance populations";
            return false;
        }
        if (mutation.upserts.empty() && mutation.removals.empty() && populationMutation.upserts.empty()
            && populationMutation.removals.empty())
            return true;

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
            if (!compileForViewer(*mViewer, placed))
            {
                mLastDiagnostic = "incremental VSG compilation failed before scene publication";
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
            if (!compileForViewer(*mViewer, group))
            {
                mLastDiagnostic = "incremental VSG population compilation failed before scene publication";
                return false;
            }
            auto visibility = vsg::Switch::create();
            visibility->addChild(vsg::MASK_ALL, std::move(group));
            populationReplacements.push_back({ std::move(visibility) });
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
        const DynamicActorWorldPlan plan = buildDynamicActorWorldPlan(world, mOptions.staticPlan);
        if (!plan.valid())
        {
            mLastDiagnostic = "dynamic actor world contains an invalid model/skeleton dependency";
            return false;
        }

        auto nextRoot = vsg::Group::create();
        nextRoot->children.reserve(plan.actors.size());
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
            const std::optional<StaticAssetPlan> evaluatedAsset = evaluateDynamicActorAssetPlan(world, frame, actor);
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
                    = RenderCore::deformMesh(world, frame, actor.instance, draw.mesh, draw.node);
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
            StaticRealizationResult realized = realizeStaticAssetConformant(
                world, actor.model, *evaluatedAsset, mTextureResolver, mSharedObjects, resolve, {}, {},
                transform->opacity);
            if (!realized.valid() || realized.stats.runtimeContextEffects != 0
                || realized.stats.unsupportedTextureBindings != 0)
            {
                mLastDiagnostic = realized.diagnostics.empty()
                    ? "dynamic actor realization requires a not-yet-supported compatibility effect"
                    : realized.diagnostics.front();
                return false;
            }
            if (!dynamicActorPlanCurrent(world, actor))
            {
                mLastDiagnostic = "dynamic actor changed while its frame graph was being realized";
                return false;
            }
            auto placed = vsg::MatrixTransform::create(toVsgMatrix(staticInstancePlacementMatrix(transform->current)));
            placed->addChild(realized.root);
            const bool castsShadow = mOptions.shadows.actorCasters
                && hasSemanticFlag(actor.semanticFlags, RenderCore::InstanceSemanticFlag::ShadowCaster);
            nextRoot->addChild(maskedNode(placementMask(castsShadow, actor.semanticFlags), std::move(placed)));
        }

        for (const RenderCore::ImmediateEffectDraw& effect : frame.immediateEffectDraws())
        {
            ImmediateEffectRealization realized
                = realizeImmediateEffectDraw(effect, mTextureResolver, mSharedObjects);
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
            nextRoot->addChild(maskedNode(placementMask(castsShadow, effect.semanticFlags), std::move(placed)));
        }
        if (!compileForViewer(*mViewer, nextRoot))
        {
            mLastDiagnostic = "incremental VSG actor compilation failed before scene publication";
            return false;
        }

        if (mDynamicLastUse)
        {
            mDynamicRetirements.reserveAdditional(1);
            if (!mDynamicRetirements.queue(*mDynamicLastUse, mDynamicRoot))
            {
                mLastDiagnostic = "previous dynamic actor graph could not be retained through GPU completion";
                return false;
            }
        }
        mDynamicRoot = std::move(nextRoot);
        mSceneRoot->children[1] = mDynamicRoot;
        mDynamicLastUse.reset();
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
            if (runtime.target.renderGraph)
                runtime.target.renderGraph->mask = vsg::MASK_OFF;
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
                if (!compileForViewer(*mViewer, created.target.renderGraph))
                {
                    mLastDiagnostic = "incremental Vulkan auxiliary view compilation failed";
                    return false;
                }
                // Water targets are already before the swapchain graph. Insert
                // generic sampled surfaces immediately before the main graph so
                // their final shader-read layout is established before MyGUI.
                mCommandGraph->children.insert(mCommandGraph->children.end() - 1, created.target.renderGraph);
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

            runtime->active = true;
            runtime->target.renderGraph->mask = vsg::MASK_ALL;
            runtime->view->mask = vsg::MASK_ALL;
            runtime->camera.update(view);
            runtime->view->LODScale = view.lodScale;
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

    bool VsgRuntimeHost::synchronizeGui()
    {
        if (!mGuiRenderer)
            return true;
        mGuiRenderer->collect();
        if (!mGuiRenderer->overlayStructureChanged())
        {
            mGuiRenderer->updatePersistentOverlay();
            return true;
        }

        vsg::ref_ptr<vsg::Node> overlay = mGuiRenderer->buildPersistentOverlay();
        auto nextRoot = vsg::Group::create();
        if (overlay)
            nextRoot->addChild(overlay);
        if (!compileForViewer(*mViewer, nextRoot))
        {
            mLastDiagnostic = "incremental VSG MyGUI compilation failed before overlay publication";
            return false;
        }
        if (mGuiLastUse)
        {
            mGuiRetirements.reserveAdditional(1);
            if (!mGuiRetirements.queue(*mGuiLastUse, mGuiRoot))
            {
                mLastDiagnostic = "previous MyGUI graph could not be retained through GPU completion";
                return false;
            }
        }
        mGuiRoot = std::move(nextRoot);
        mMainOnlyRoot->children.back() = mGuiRoot;
        mGuiLastUse.reset();
        return true;
    }

    RenderCore::RenderFrameResult VsgRuntimeHost::renderFrame(
        const RenderCore::RenderWorld& world, const RenderCore::FrameRenderState& frame)
    {
        mLastDiagnostic.clear();
        if (!frame.valid() || !RenderCore::frameCompatibleWithWorld(world, frame))
            return finish(RenderCore::RenderFrameResult::Failed, "invalid or stale semantic frame state");
        if (!frame.dynamicMaterials().empty())
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

        const VsgCompletionPoll completion = mCompletion.pollBeforeRecordAndSubmit(*mViewer);
        if (completion.result != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan completion polling failed with VkResult " + std::to_string(completion.result));
        if (completion.completedThrough)
        {
            (void)mStaticResidency.collect(*completion.completedThrough);
            (void)mStaticPopulationResidency.collect(*completion.completedThrough);
            (void)mDynamicRetirements.collect(*completion.completedThrough);
            (void)mGuiRetirements.collect(*completion.completedThrough);
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
        if (!synchronizeLocalLights(world) || !synchronizeStaticWorld(world)
            || !synchronizePopulationVisibility(world, *mainView) || !synchronizeDynamicActors(world, frame)
            || !synchronizeGui())
            return finish(RenderCore::RenderFrameResult::Failed, mLastDiagnostic);

        mViewer->update();
        const VsgSubmitPresentResult submission = submitAndPresentChecked(*mViewer);
        if (submission.submit != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan record/submit failed with VkResult " + std::to_string(submission.submit));
        mWaitedIdle = false;
        if (!mCompletion.registerSubmission(*mViewer, frame.frameId())
            || !mStaticResidency.markSubmitted(frame.frameId())
            || !mStaticPopulationResidency.markSubmitted(frame.frameId()))
        {
            // Submission has already happened. Synchronize before returning so
            // an untracked in-flight frame can never make later destruction or
            // replacement unsafe, even if an internal invariant is violated.
            waitIdle();
            return finish(RenderCore::RenderFrameResult::Failed,
                "submitted frame could not be registered; device was synchronized for safety");
        }
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
