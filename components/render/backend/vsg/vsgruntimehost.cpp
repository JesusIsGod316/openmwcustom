#include "vsgruntimehost.hpp"

#include "legacymaterialshader.hpp"
#include "dynamicactorplan.hpp"
#include "staticassetconformance.hpp"

#include <components/vsgmygui/rendermanager.hpp>

#include <components/rendercore/deformation.hpp>

#include <vsg/app/CommandGraph.h>
#include <vsg/app/RenderGraph.h>
#include <vsg/app/View.h>
#include <vsg/app/Viewer.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/MatrixTransform.h>
#include <vsg/utils/SharedObjects.h>

#include <algorithm>
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
            result.extent = { 1, 1 };
            return result;
        }

        [[nodiscard]] bool vsgProjectionCompatible(const RenderCore::ProjectionState& projection) noexcept
        {
            return projection.depthRange == RenderCore::ClipDepthRange::ZeroToOne
                && projection.depthDirection == RenderCore::DepthDirection::Reversed
                && projection.yDirection == RenderCore::ClipYDirection::Down;
        }
    }

    VsgRuntimeHost::VsgRuntimeHost(vsg::ref_ptr<SdlVulkanWindow> window,
        StaticTextureResolver textureResolver, VsgRuntimeHostOptions options)
        : mOptions(options)
        , mTextureResolver(std::move(textureResolver))
        , mWindow(std::move(window))
        , mSharedObjects(vsg::SharedObjects::create())
        , mViewer(vsg::Viewer::create())
        , mSceneRoot(vsg::Group::create())
        , mStaticRoot(vsg::Group::create())
        , mDynamicRoot(vsg::Group::create())
        , mGuiRoot(vsg::Group::create())
        , mAmbientLight(vsg::AmbientLight::create())
        , mSunLight(vsg::DirectionalLight::create())
        , mCamera(FrameCameraObjects::create(initialView()))
        , mCompletion(options.maximumFramesInFlight)
    {
        if (!mWindow || !mWindow->valid() || !mTextureResolver
            || options.maximumFramesInFlight != VsgRecordAndSubmitRingSize)
            throw std::invalid_argument(
                "VsgRuntimeHost requires a valid window/resolver and the exact VSG 1.1.15 three-frame task ring");

        mViewer->addWindow(mWindow);
        mView = vsg::View::create(mCamera.camera);
        mOpenMwViewState = OpenMwViewDependentState::create(mView.get());
        mOpenMwViewState->shaderSet = createLegacyCompatibilityShaderSet();
        if (!mOpenMwViewState->shaderSet)
            throw std::runtime_error("VsgRuntimeHost could not create its OpenMW view shader contract");
        mView->viewDependentState = mOpenMwViewState;
        mAmbientLight->name = "OpenMW ambient";
        mSunLight->name = "OpenMW sun";
        mView->addChild(mAmbientLight);
        mView->addChild(mSunLight);
        mSceneRoot->addChild(mStaticRoot);
        mSceneRoot->addChild(mDynamicRoot);
        mSceneRoot->addChild(mGuiRoot);
        const VkExtent2D initialExtent = mWindow->extent2D();
        mUiPipeline = createUiPipeline(std::max(1u, initialExtent.width), std::max(1u, initialExtent.height));
        if (!mUiPipeline)
            throw std::runtime_error("VsgRuntimeHost could not create its MyGUI pipeline");
        mView->addChild(mSceneRoot);
        mView->bins = createStaticConformanceBins();
        mRenderGraph = vsg::RenderGraph::create(mWindow);
        mRenderGraph->addChild(mView);
        auto commandGraph = vsg::CommandGraph::create(mWindow);
        commandGraph->addChild(mRenderGraph);
        mViewer->assignRecordAndSubmitTaskAndPresentation({ commandGraph });
        const vsg::CompileResult compile = mViewer->compile();
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
        mRenderGraph = {};
        mOpenMwViewState = {};
        mView = {};
        mViewer = {};
        mWindow = {};
    }

    RenderCore::RenderBackendKind VsgRuntimeHost::backendKind() const noexcept
    {
        return RenderCore::RenderBackendKind::VsgVulkan;
    }

    const RenderCore::FrameView* VsgRuntimeHost::selectMainView(
        const RenderCore::FrameRenderState& frame) const noexcept
    {
        if (frame.views().size() != 1 || frame.views().front().kind != RenderCore::ViewKind::Main
            || frame.views().front().semanticIncludeMask != ~std::uint64_t{ 0 }
            || frame.views().front().semanticExcludeMask != 0)
            return nullptr;
        return &frame.views().front();
    }

    bool VsgRuntimeHost::synchronizeStaticWorld(const RenderCore::RenderWorld& world)
    {
        const StaticWorldMutation mutation = mStaticResidency.prepare(world, mOptions.staticPlan);
        if (!mutation.valid || mutation.simpleMeshInstancesDeferred != 0)
        {
            mLastDiagnostic = "static world contains invalid or not-yet-supported instance populations";
            return false;
        }
        if (mutation.upserts.empty() && mutation.removals.empty())
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
            StaticRealizationResult realized = realizeStaticAssetConformant(
                world, plan.model, plan.asset, mTextureResolver, mSharedObjects);
            if (!realized.valid() || realized.stats.runtimeContextEffects != 0
                || realized.stats.unsupportedTextureBindings != 0)
            {
                mLastDiagnostic = realized.diagnostics.empty()
                    ? "static realization requires a not-yet-supported compatibility effect"
                    : realized.diagnostics.front();
                return false;
            }
            auto placed = vsg::MatrixTransform::create(toVsgMatrix(staticInstancePlacementMatrix(plan.placement)));
            placed->addChild(realized.root);
            if (!compileForViewer(*mViewer, placed))
            {
                mLastDiagnostic = "incremental VSG compilation failed before scene publication";
                return false;
            }
            replacements.push_back(std::move(placed));
        }

        std::unordered_map<std::uint64_t, std::size_t> replacementIndices;
        replacementIndices.reserve(mutation.upserts.size());
        for (std::size_t i = 0; i < mutation.upserts.size(); ++i)
            replacementIndices.emplace(staticInstanceKey(mutation.upserts[i].instance), i);

        // Allocate the next traversal list before changing logical residency or
        // the live root. Residency performs its own pre-publication allocation;
        // the final scene-root publication is an allocation-free swap.
        vsg::Group::Children nextChildren;
        nextChildren.reserve(mutation.orderedInstances.size());
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

        StaticWorldCommitResult<StaticResident> committed
            = mStaticResidency.commit(world, mutation, std::move(replacements));
        if (!committed.committed)
        {
            mLastDiagnostic = "static world changed while its replacement graph was being realized";
            return false;
        }
        mStaticRoot->children.swap(nextChildren);
        return true;
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
            const std::optional<StaticAssetPlan> evaluatedAsset
                = evaluateDynamicActorAssetPlan(world, frame, actor);
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
            const MeshPayloadResolver resolve = [&](RenderCore::MeshHandle, RenderCore::ModelNodeIndex node)
                -> const RenderCore::MeshPayload* {
                const auto found = deformed.find(node.value());
                return found == deformed.end() ? nullptr : &found->second;
            };
            StaticRealizationResult realized = realizeStaticAssetConformant(
                world, actor.model, *evaluatedAsset, mTextureResolver, mSharedObjects, resolve);
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
            auto placed = vsg::MatrixTransform::create(
                toVsgMatrix(staticInstancePlacementMatrix(transform->current)));
            placed->addChild(realized.root);
            nextRoot->addChild(placed);
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
        if (mOpenMwViewState->localLightsCurrent(world))
            return true;

        LocalLightBufferPlan plan
            = buildLocalLightBufferPlan(buildLocalLightWorldPlan(world), glm::dvec3(0.0));
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
        if (!mOpenMwViewState->setLocalLights(std::move(plan)))
        {
            mLastDiagnostic = "OpenMW view state rejected its prepared local light buffer";
            return false;
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
        mSceneRoot->children[2] = mGuiRoot;
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
            return finish(RenderCore::RenderFrameResult::Failed,
                "dynamic materials require a later compatibility facet");
        if (frame.environment().skyEnabled || frame.environment().waterEnabled)
            return finish(RenderCore::RenderFrameResult::Failed,
                "sky and water require the environment compatibility facet");
        if (frame.environment().clusteredLocalLighting)
            return finish(RenderCore::RenderFrameResult::Failed,
                "clustered local-light selection and far-plane fading require the clustered compatibility facet");
        const RenderCore::FrameView* mainView = selectMainView(frame);
        if (!mainView || mainView->extent != frame.renderExtent() || frame.renderExtent() != frame.outputExtent())
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP3C host requires one unmasked main view with equal render and output extents");
        if (!vsgProjectionCompatible(mainView->current.projection)
            || !vsgProjectionCompatible(mainView->previous.projection) || frame.jitter() != glm::vec2(0.0f)
            || frame.projectionOffset() != glm::vec2(0.0f))
            return finish(RenderCore::RenderFrameResult::Failed,
                "CP3C host requires explicit reversed zero-to-one/down-Y projection and no temporal jitter");
        const VkExtent2D desiredExtent{ frame.outputExtent().width, frame.outputExtent().height };
        const auto extentMatches = [&] {
            const VkExtent2D actual = mWindow->extent2D();
            return actual.width == desiredExtent.width && actual.height == desiredExtent.height;
        };
        if (!extentMatches())
            mWindow->resize();
        if (!extentMatches())
            return finish(RenderCore::RenderFrameResult::Skipped, "SDL pixel extent is not ready for the requested output");
        if (!mViewer->advanceToNextFrame(frame.simulationTime()))
            return finish(RenderCore::RenderFrameResult::Skipped, "VSG could not acquire the next swapchain frame");

        const VsgCompletionPoll completion = mCompletion.pollBeforeRecordAndSubmit(*mViewer);
        if (completion.result != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan completion polling failed with VkResult " + std::to_string(completion.result));
        if (completion.completedThrough)
        {
            (void)mStaticResidency.collect(*completion.completedThrough);
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
        mAmbientLight->color.set(environment.ambient.r, environment.ambient.g, environment.ambient.b);
        mAmbientLight->intensity = 1.0f;
        mSunLight->color.set(environment.sunDiffuse.r, environment.sunDiffuse.g, environment.sunDiffuse.b);
        mSunLight->intensity = environment.sunLightEnabled ? 1.0f : 0.0f;
        mSunLight->direction.set(
            environment.sunDirection.x, environment.sunDirection.y, environment.sunDirection.z);
        const RenderCore::Color& clear = environment.fogColor;
        mRenderGraph->setClearValues({ { clear.r, clear.g, clear.b, clear.a } });
        if (!synchronizeLocalLights(world) || !synchronizeStaticWorld(world)
            || !synchronizeDynamicActors(world, frame) || !synchronizeGui())
            return finish(RenderCore::RenderFrameResult::Failed, mLastDiagnostic);

        mViewer->update();
        const VsgSubmitPresentResult submission = submitAndPresentChecked(*mViewer);
        if (submission.submit != VK_SUCCESS)
            return finish(RenderCore::RenderFrameResult::Failed,
                "Vulkan record/submit failed with VkResult " + std::to_string(submission.submit));
        mWaitedIdle = false;
        if (!mCompletion.registerSubmission(*mViewer, frame.frameId())
            || !mStaticResidency.markSubmitted(frame.frameId()))
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

    RenderCore::RenderFrameResult VsgRuntimeHost::finish(
        RenderCore::RenderFrameResult result, std::string diagnostic)
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
        return mStaticResidency.pendingRetirementCount() + mDynamicRetirements.size() + mGuiRetirements.size();
    }
}
