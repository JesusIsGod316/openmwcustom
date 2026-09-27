#include "v4enginerenderbridge.hpp"
#include <components/debug/gameplaydiagnostics.hpp>

#include "v4runtimeoptions.hpp"
#include "v4effectcapture.hpp"
#include "v4objectcaptureplan.hpp"
#include "v4persistentobject.hpp"
#include "v4animatedobjectcapture.hpp"
#include "v4rigidactorpose.hpp"
#include "v4scenerenderlifecycle.hpp"
#include "v4semanticsource.hpp"
#include "v4terrainsource.hpp"
#include "vulkanmw/groundcoversource.hpp"
#include "vulkanmw/nativeanimationruntime.hpp"

#include "animation.hpp"
#include "groundcover.hpp"
#include "npcanimation.hpp"
#include "renderingmanager.hpp"
#include "postprocessor.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

#include <components/nif/niffile.hpp>
#include <components/nifrender/actormodelcomposer.hpp>
#include <components/nifrender/enchantedglow.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

#include <components/debug/debuglog.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadacti.hpp>
#include <components/esm3/loadstat.hpp>

#include <components/misc/strings/lower.hpp>
#include <components/misc/convert.hpp>
#include <components/misc/resourcehelpers.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>
#include <components/vsgmygui/platform.hpp>
#include <components/vsgmygui/rendermanager.hpp>
#include <components/vsgmygui/vfsimagedecoder.hpp>

#include <osg/Geode>
#include <osg/NodeVisitor>

#include <components/render/backend/vsg/vfstextureresolver.hpp>
#include <components/nifrender/vfsidentity.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_inverse.hpp>

namespace MWRender
{
    namespace
    {
        [[nodiscard]] glm::mat4 toGlm(const osg::Matrixf& source) noexcept
        {
            glm::mat4 result(1.0f);
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 4; ++row)
                    result[static_cast<glm::length_t>(column)][static_cast<glm::length_t>(row)] = source(column, row);
            return result;
        }

        [[nodiscard]] bool finite(const glm::mat4& value) noexcept
        {
            for (glm::length_t column = 0; column < 4; ++column)
                for (glm::length_t row = 0; row < 4; ++row)
                    if (!std::isfinite(value[column][row]))
                        return false;
            return true;
        }

        [[nodiscard]] std::optional<NifRender::StaticModelCacheResult> ensureModelPublished(
            RenderVsg::VsgSemanticSession& session, const VFS::Manager& vfs, VFS::Path::NormalizedView path,
            NifRender::TextureIdentityCache* textureIdentities, std::string* failureDiagnostic = nullptr)
        {
            const auto fail = [&](std::string message) -> std::optional<NifRender::StaticModelCacheResult> {
                if (failureDiagnostic)
                    *failureDiagnostic = std::move(message);
                return std::nullopt;
            };
            if (path.empty())
                return fail("model path is empty");
            if (const std::optional<RenderCore::ModelHandle> model = session.models().find(path.value()))
            {
                return NifRender::StaticModelCacheResult{ NifRender::StaticModelCacheStatus::Reused,
                    NifRender::TranslationPublishStatus::Applied, *model, session.models().findSkeleton(path.value()) };
            }
            const VFS::Path::Normalized normalized(path);
            if (!vfs.exists(normalized))
                return fail("winning VFS has no file for '" + std::string(path.value()) + "'");
            try
            {
                Debug::GameplayDiagnostics::Operation modelDiagnostic("v4_model_load", std::string(path.value()));
                Nif::NIFFile nifFile(normalized);
                Nif::Reader reader(nifFile, nullptr);
                reader.parse(vfs.get(normalized));
                const NifRender::TranslationBundle bundle
                    = NifRender::translateStaticNif(Nif::FileView(nifFile), vfs, {}, textureIdentities);
                const NifRender::StaticModelCacheResult published = session.models().publish(bundle);
                if (published.available())
                    return published;

                std::string detail = "model '" + std::string(path.value()) + "' was rejected";
                for (const NifRender::TranslationDiagnostic& diagnostic : bundle.diagnostics)
                {
                    if (diagnostic.severity != NifRender::DiagnosticSeverity::Error)
                        continue;
                    detail += " by translation [" + diagnostic.code + "]";
                    if (diagnostic.sourceRecordId)
                        detail += " record " + std::to_string(*diagnostic.sourceRecordId);
                    if (!diagnostic.sourceRecordType.empty())
                        detail += " (" + diagnostic.sourceRecordType + ")";
                    if (!diagnostic.message.empty())
                        detail += ": " + diagnostic.message;
                    return fail(std::move(detail));
                }
                detail += " during cache publication (cache status "
                    + std::to_string(static_cast<unsigned int>(published.status)) + ", publish status "
                    + std::to_string(static_cast<unsigned int>(published.publishStatus)) + ")";
                return fail(std::move(detail));
            }
            catch (const std::exception& error)
            {
                return fail("model '" + std::string(path.value()) + "' could not be parsed or translated: "
                    + error.what());
            }
        }

        [[nodiscard]] bool requiresModelPlayback(const RenderCore::ModelRecord& model) noexcept
        {
            if (!RenderCore::validModelDynamicRequirements(model.dynamicRequirements)
                || model.dynamicRequirements != 0 || !model.payload)
                return true;
            return std::any_of(model.payload->nodes.begin(), model.payload->nodes.end(),
                [](const RenderCore::ModelNodeRecord& node) { return node.controllerFlags != 0; });
        }

        [[nodiscard]] std::string enchantedGlowDiagnostic(NifRender::EnchantedGlowPublishStatus status)
        {
            using Status = NifRender::EnchantedGlowPublishStatus;
            switch (status)
            {
                case Status::MissingTexture:
                    return "NPC enchanted equipment is missing one or more canonical caustic texture frames";
                case Status::ExistingEnvironmentBinding:
                    return "NPC enchanted equipment also owns an authored environment map; combined semantics remain fail-closed";
                case Status::UnsupportedLightingOrder:
                    return "NPC enchanted equipment returned the reserved unsupported lighting-order status";
                case Status::ReservationFailed:
                    return "NPC enchanted equipment glow resource reservation failed";
                case Status::BatchBuildFailed:
                    return "NPC enchanted equipment glow update batch could not be built";
                case Status::PublishRejected:
                    return "NPC enchanted equipment glow publication was rejected";
                case Status::InvalidSource:
                    return "NPC enchanted equipment glow source is invalid";
                case Status::Published:
                case Status::Reused:
                    break;
            }
            return "NPC enchanted equipment glow publication returned an invalid status";
        }

        class MorphCollector final : public osg::NodeVisitor
        {
        public:
            MorphCollector()
                : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
            {
            }

            void apply(osg::Drawable& drawable) override
            {
                // Modern OpenMW's NIF loader publishes geometry directly as
                // drawable nodes. Restricting this visitor to osg::Geode silently
                // missed those MorphGeometry instances and made a valid one-morph
                // NPC part look as if its evaluated attachment had no morphs.
                if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(&drawable))
                    morphs.push_back(morph);
                traverse(drawable);
            }

            std::vector<SceneUtil::MorphGeometry*> morphs;
        };

        struct EvaluatedMorphSource
        {
            RenderCore::MeshHandle mesh;
            SceneUtil::MorphGeometry* geometry = nullptr;
            bool consumed = false;
        };

        [[nodiscard]] std::string modelDynamicRequirementDiagnostic(std::uint32_t requirements)
        {
            using RenderCore::ModelDynamicRequirement;
            using RenderCore::modelDynamicRequirement;
            if ((requirements & modelDynamicRequirement(ModelDynamicRequirement::ParticleSystem)) != 0)
                return "actor model requires particle-system playback and realization";
            if ((requirements & modelDynamicRequirement(ModelDynamicRequirement::NodeEffect)) != 0)
                return "actor model requires dynamic node-effect realization";
            if ((requirements & modelDynamicRequirement(ModelDynamicRequirement::SequencePlayback)) != 0)
                return "actor model requires NiFltAnimationNode sequence playback";
            if ((requirements & modelDynamicRequirement(ModelDynamicRequirement::DynamicVertexData)) != 0)
                return "actor model requires dynamic source-vertex playback";
            return "actor model declares an unknown dynamic realization requirement";
        }
    }
    bool V4EngineRenderBridge::linkedRuntimeAvailable() noexcept
    {
        return true;
    }

    std::unique_ptr<V4EngineRenderBridge> V4EngineRenderBridge::create(
        const VFS::Manager& vfs, RenderVsg::VsgRuntimeBootstrapOptions options)
    {
        // Use the same winning VFS asset as native OpenMW water, decoded as
        // normal data (never sRGB). The procedural path stays available for A/B.
        if (options.host.water.enabled && !std::getenv("OPENMW_V4_PROCEDURAL_WATER_CONTROL"))
        {
            const auto identity = NifRender::resolveTextureVfsIdentity(
                VFS::Path::NormalizedView("textures/omw/water_nm.png"), vfs);
            if (identity.valid())
            {
                RenderCore::TextureRecord record;
                record.sourceIdentity = std::string(identity.canonicalPath.value());
                record.contentIdentity = identity.contentIdentity;
                const RenderCore::TextureRealizationKey key{
                    { RenderCore::TextureHandle::fromParts(0, 1), RenderCore::TextureColorSpace::Data,
                        RenderCore::TextureFormatClass::Normal }, record.revision };
                auto report = std::make_shared<RenderVsg::StaticTextureDecodeReport>();
                RenderVsg::StaticTextureDecoder decoder({}, report);
                auto data = decoder.decode(record, key, [&vfs](std::string_view path) {
                    return vfs.find(VFS::Path::toNormalized(path));
                });
                if (data && report->warningFallbacks == 0)
                    options.host.water.normalMap = std::move(data);
            }
            if (!options.host.water.normalMap)
                Log(Debug::Warning) << "V4 water normal map unavailable; retaining procedural water";
        }
        std::shared_ptr<RenderVsg::VsgSemanticSession> session(
            RenderVsg::VsgSemanticSession::create(RenderVsg::makeVfsStaticTextureResolver(vfs), std::move(options)));
        if (!session)
            throw std::runtime_error("V4 engine render bridge received no semantic session");
        return std::unique_ptr<V4EngineRenderBridge>(new V4EngineRenderBridge(vfs, std::move(session)));
    }

    std::unique_ptr<V4EngineRenderBridge> V4EngineRenderBridge::createConfigured(const VFS::Manager& vfs)
    {
        return create(vfs, makeV4RuntimeBootstrapOptions());
    }

    V4EngineRenderBridge::V4EngineRenderBridge(
        const VFS::Manager& vfs, std::shared_ptr<RenderVsg::VsgSemanticSession> session)
        : mVfs(vfs)
        , mTextureIdentities(vfs, std::getenv("OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES") ? 0u : 4096u)
        , mSession(std::move(session))
        , mNativeAssets(std::make_unique<RenderNative::NifAssetService>(
              mVfs, &mTextureIdentities, mSession->models()))
        , mNativeStaticWorld(std::make_unique<RenderNative::StaticWorldService>(
              mSession->cells(), mSession->populations()))
        , mRouteStatus(std::make_shared<V4RenderRouteStatus>())
        , mNativeTerrain(std::make_unique<RenderNative::TerrainWorldService>(
              mSession->world(), mSession->publisher()))
        , mNativeAnimation(std::make_unique<V4NativeAnimationRuntime>(mVfs))
    {
    }

    bool V4EngineRenderBridge::synchronizeExteriorTerrain(const RenderingManager& rendering, const MWWorld::Cell& cell)
    {
        mLastDiagnostic.clear();
        if (!cell.isExterior())
        {
            if (!synchronizeGroundcover(rendering, {}, {}))
                return false;
            const RenderNative::TerrainWorldSyncResult cleared = mNativeTerrain->clear();
            if (!cleared.accepted())
            {
                mLastDiagnostic = cleared.diagnostic.empty()
                    ? "native terrain clear failed"
                    : cleared.diagnostic;
                return false;
            }
            return true;
        }

        TerrainStorage* const storage = rendering.getTerrainStorage();
        if (!storage)
        {
            mLastDiagnostic = "authoritative exterior terrain storage is unavailable";
            return false;
        }

        const RenderCore::TerrainPreparationRequest current
            = makeV4TerrainChunkRequest(cell, cell.getGridX(), cell.getGridY(), true);
        const std::vector<RenderCore::TerrainResidencyCell> residency
            = mNativeTerrain->updateResidency(current.worldspaceIdentity, current.gridX, current.gridY);

        std::vector<RenderCore::TerrainPreparationRequest> desired;
        desired.reserve(residency.size());
        for (const RenderCore::TerrainResidencyCell& resident : residency)
            desired.push_back(makeV4TerrainChunkRequest(
                cell, resident.gridX, resident.gridY, resident.required, resident.lodLevel, resident.stitchMask));

        const RenderNative::TerrainWorldSyncResult terrain = mNativeTerrain->synchronize(
            std::span<const RenderCore::TerrainPreparationRequest>(desired),
            [storage](const RenderCore::TerrainPreparationRequest& request, std::stop_token stop) {
                if (stop.stop_requested())
                    return std::optional<RenderCore::TerrainChunkSource>{};
                return makeV4TerrainChunkSource(*storage, request);
            });
        if (!terrain.accepted())
        {
            mLastDiagnostic = terrain.diagnostic.empty()
                ? "native terrain synchronization failed"
                : terrain.diagnostic;
            return false;
        }

        if (!synchronizeGroundcover(rendering, current.worldspaceIdentity, residency))
            return false;
        return true;
    }

    bool V4EngineRenderBridge::synchronizeGroundcover(const RenderingManager& rendering,
        std::string_view worldspaceIdentity, std::span<const RenderCore::TerrainResidencyCell> residency)
    {
        if (mGroundcoverEpoch != mSession->world().epoch())
        {
            mGroundcoverCells.clear();
            mGroundcoverEpoch = mSession->world().epoch();
        }

        std::set<std::string, std::less<>> desired;
        const bool groundcoverEnabled = Settings::groundcover().mEnabled && !worldspaceIdentity.empty()
            && ESM::RefId::deserializeText(worldspaceIdentity) == ESM::Cell::sDefaultWorldspaceId;
        if (groundcoverEnabled)
        {
            for (const RenderCore::TerrainResidencyCell& resident : residency)
            {
                if (resident.lodLevel == 0)
                    desired.insert("groundcover:" + std::string(worldspaceIdentity) + ":"
                        + std::to_string(resident.gridX) + "," + std::to_string(resident.gridY));
            }

            constexpr std::size_t maxNewCellsPerFrame = 1;
            std::size_t newCells = 0;
            for (const RenderCore::TerrainResidencyCell& resident : residency)
            {
                if (resident.lodLevel != 0)
                    continue;
                const std::string cellIdentity = "groundcover:" + std::string(worldspaceIdentity) + ":"
                    + std::to_string(resident.gridX) + "," + std::to_string(resident.gridY);
                if (mGroundcoverCells.contains(cellIdentity) || newCells >= maxNewCellsPerFrame)
                    continue;

                VulkanMW::GroundcoverPopulationSource source = VulkanMW::makeGroundcoverPopulationSource(
                    rendering.getGroundcoverStore(), Settings::groundcover().mDensity.get(), *mNativeAssets,
                    mSession->world(), worldspaceIdentity, resident.gridX, resident.gridY,
                    Settings::groundcover().mRenderingDistance.get());
                if (!source.valid())
                {
                    mLastDiagnostic = source.diagnostic.empty()
                        ? "native groundcover source rejected the resident cell"
                        : source.diagnostic;
                    return false;
                }

                const RenderNative::StaticWorldMutationResult activated
                    = mNativeStaticWorld->activatePopulationCell(std::move(source.cell));
                if (!activated.accepted())
                {
                    mLastDiagnostic = "native groundcover population cell publication failed";
                    return false;
                }

                for (RenderCore::StaticPopulationInstanceSource& placement : source.instances)
                {
                    const RenderNative::StaticWorldMutationResult published
                        = mNativeStaticWorld->upsertPopulation(std::move(placement));
                    if (!published.accepted())
                    {
                        static_cast<void>(mNativeStaticWorld->deactivatePopulationCell(cellIdentity));
                        mLastDiagnostic = "native groundcover placement publication failed";
                        return false;
                    }
                }

                mGroundcoverCells.insert(cellIdentity);
                ++newCells;
            }
        }

        for (auto current = mGroundcoverCells.begin(); current != mGroundcoverCells.end();)
        {
            if (desired.contains(*current))
            {
                ++current;
                continue;
            }
            const RenderNative::StaticWorldMutationResult retired
                = mNativeStaticWorld->deactivatePopulationCell(*current);
            if (!retired.accepted())
            {
                mLastDiagnostic = "native groundcover population retirement failed";
                return false;
            }
            current = mGroundcoverCells.erase(current);
        }
        return true;
    }

    V4EngineRenderBridge::~V4EngineRenderBridge()
    {
        stopBackgroundPreparation();
        waitIdle();
    }

    void V4EngineRenderBridge::stopBackgroundPreparation()
    {
        if (mNativeTerrain)
            mNativeTerrain->stopBackgroundPreparation();
    }

    std::unique_ptr<MWWorld::SceneRenderLifecycle> V4EngineRenderBridge::takeSceneRenderLifecycle()
    {
        if (mLifecycleTaken)
            throw std::logic_error("V4 scene render lifecycle was already taken");
        std::unique_ptr<MWWorld::SceneRenderLifecycle> result
            = std::make_unique<V4SceneRenderLifecycle>(mSession, mVfs, mRouteStatus);
        mLifecycleTaken = true;
        return result;
    }

    std::optional<RenderCore::Extent2D> V4EngineRenderBridge::outputExtent() const noexcept
    {
        if (!mSession)
            return std::nullopt;
        SDL_Window* const window = mSession->bootstrap().sdlWindow();
        if (!window)
            return std::nullopt;
        const SDL_WindowFlags flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_HIDDEN) != 0 || (flags & SDL_WINDOW_MINIMIZED) != 0)
            return std::nullopt;
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(window, &width, &height) || width <= 0 || height <= 0)
            return std::nullopt;
        const RenderCore::Extent2D result{ static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height) };
        if (!result.valid())
            return std::nullopt;
        return result;
    }

    SDL_Window* V4EngineRenderBridge::sdlWindow() const noexcept
    {
        return mSession ? mSession->bootstrap().sdlWindow() : nullptr;
    }

    std::unique_ptr<MyGUIPlatform::PlatformBase> V4EngineRenderBridge::createGuiPlatform(
        const std::filesystem::path& logName)
    {
        const std::optional<RenderCore::Extent2D> extent = outputExtent();
        if (!extent)
            throw std::runtime_error("V4 MyGUI platform requires a visible Vulkan drawable extent");
        RenderVsg::VsgRuntimeHost& host = mSession->bootstrap().renderer();
        constexpr VFS::Path::NormalizedView resourcePath("mygui");
        auto platform = std::make_unique<VsgMyGui::Platform>(host.uiPipeline(), VsgMyGui::makeVfsImageDecoder(mVfs),
            &mVfs, static_cast<int>(extent->width), static_cast<int>(extent->height), resourcePath, logName,
            [session = mSession](
                VsgMyGui::RenderManager* renderer) { session->bootstrap().renderer().detachGuiRenderer(renderer); });
        host.attachGuiRenderer(platform->getRenderManagerPtr());
        return platform;
    }

    bool V4EngineRenderBridge::captureDynamicFrameState(const RenderingManager& rendering, V4MainFrameSource& source)
    {
        Debug::GameplayDiagnostics::Stage diagnostic("dynamic_capture");
        // Snapshot the parser/UI/Lua-owned effect state on the gameplay thread.
        // The Vulkan draw path never reads mutable Fx or OSG state concurrently.
        if (auto* post = rendering.getPostProcessor())
            mSession->bootstrap().renderer().setPostProcessingFrame(
                post->prepareNativeFrame(source.simulationTime, source.frameDelta));
        auto textureSnapshot = [&] {
            Debug::GameplayDiagnostics::Stage stage("texture_identity_prepare");
            return NifRender::TextureIdentityCache::CaptureScope(mTextureIdentities);
        }();
        mLastDiagnostic.clear();
        if (!mSession->healthy() || !mRouteStatus->healthy())
        {
            // A loading-frame error can be caught by a delayed Lua action. Do
            // not mutate the partially transitioned world on the next capture
            // or replace the original GPU error with a missing-actor error.
            mLastDiagnostic = !mSession->healthy() ? mSession->lastDiagnostic() : mRouteStatus->firstDiagnostic();
            return false;
        }
        const RenderCore::WorldEpoch worldEpoch = mSession->world().epoch();
        const bool persistentDraws = Misc::environmentFlag<"OPENMW_V4_PERSISTENT_DRAW_STREAM">();
        const bool cachedObjectAdmission = Misc::environmentFlag<"OPENMW_VK_CACHED_OBJECT_ADMISSION">();
        unsigned reusedObjectAdmissions = 0, skippedObjectAdmissions = 0;
        std::uint64_t cleanObjectPublications = 0, objectBindingInspections = 0;
        if (persistentDraws) mPersistentDraws.begin(worldEpoch.value());
        if (mEvaluatedObjectPlaybackEpoch != worldEpoch)
        {
            if (mObjectCapturePlans) mObjectCapturePlans->clear();
            mEvaluatedObjectPlayback.clear();
            mNativeSkeletalAssets.clear();
            mNativeSkeletalObjects.clear();
            mEvaluatedObjectPlaybackEpoch = worldEpoch;
        }
        if (mComposedActorEpoch != worldEpoch)
        {
            mForcedActorSkeletons.clear();
            mComposedActors.clear();
            mComposedActorEpoch = worldEpoch;
        }
        if (mActorLightEpoch != worldEpoch)
        {
            mActorLights.clear();
            mActorLightEpoch = worldEpoch;
        }
        std::set<std::string, std::less<>> currentActorLights;
        std::map<std::string, NativeSkeletalInstance, std::less<>> currentSkeletalObjects;
        unsigned skeletalObjectFallbacks = 0;
        unsigned skeletalPosesValidated = 0;
        float skeletalPoseMaximumError = 0;
        std::uint32_t nativeActorPoses = 0;
        std::uint32_t legacyActorPoses = 0;
        std::uint32_t nativeSampledTracks = 0;
        std::uint32_t nativeFallbackEvents = 0;
        std::uint32_t nativeSeedFailures = 0;
        bool compatible = true;
        if (++mPoseTraversal == 0u)
            ++mPoseTraversal;
        if (mNativeAnimation)
            mNativeAnimation->beginFrame();

        rendering.forEachAnimation([&](Animation& animation) {
            if (!compatible)
                return;
            const MWWorld::Ptr ptr = animation.getPtr();
            if (ptr.isEmpty() || !ptr.getRefData().isEnabled())
                return;

            Debug::GameplayDiagnostics::CaptureWork captureWork(ptr.getClass().isActor());
            if (!ptr.getClass().isActor())
            {
                // The animation root is replaced through setObjectRoot(), which
                // invalidates this engine-owned decision. A new RenderWorld
                // epoch likewise requires a fresh model/playback lookup. This
                // avoids repeating it for unchanged objects, including the
                // numerous non-playing objects in a modded exterior.
                const auto* admission = cachedObjectAdmission
                    ? animation.getV4ObjectCaptureAdmission(worldEpoch.value()) : nullptr;
                if (admission)
                {
                    ++reusedObjectAdmissions;
                    if (!admission->needsEvaluatedCapture)
                    {
                        ++skippedObjectAdmissions;
                        return;
                    }
                }
                VFS::Path::Normalized uncachedModelPath;
                if (!admission) uncachedModelPath = ptr.getClass().getCorrectedModel(ptr);
                const VFS::Path::Normalized& modelPath
                    = admission ? admission->correctedModel : uncachedModelPath;
                if (modelPath.empty() || Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
                    return;

                bool needsEvaluatedCapture = admission ? admission->needsEvaluatedCapture : ptr.getClass().useAnim();
                if (Debug::GameplayDiagnostics::sampling() && ptr.getClass().useAnim())
                    ++Debug::GameplayDiagnostics::context.useAnimObjects;
                if (!needsEvaluatedCapture)
                {
                    const auto cached = mEvaluatedObjectPlayback.find(modelPath.value());
                    if (cached != mEvaluatedObjectPlayback.end())
                        needsEvaluatedCapture = cached->second;
                    else
                    {
                        const std::optional<NifRender::StaticModelCacheResult> published
                            = ensureModelPublished(*mSession, mVfs, modelPath, &mTextureIdentities);
                        const RenderCore::ModelRecord* model
                            = published ? mSession->world().get(published->model) : nullptr;
                        if (!published || !model)
                        {
                            compatible = false;
                            mLastDiagnostic = "evaluated non-actor model could not resolve its canonical V4 model state: "
                                + modelPath.value();
                            return;
                        }
                        needsEvaluatedCapture = requiresModelPlayback(*model);
                        mEvaluatedObjectPlayback.emplace(std::string(modelPath.value()), needsEvaluatedCapture);
                    }
                }
                if (cachedObjectAdmission && !admission)
                    animation.setV4ObjectCaptureAdmission(worldEpoch.value(), modelPath, needsEvaluatedCapture);
                if (!needsEvaluatedCapture)
                    return;

                const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
                if (!identity)
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated non-actor object has no stable content identity";
                    return;
                }
                // Native skeletal props bypass binding-plan and evaluated mesh
                // capture. Engine state gates admission; source coverage binds once.
                const auto nativeModelPath = animation.getV4SourceModel();
                if (Misc::environmentFlag<"OPENMW_VK_NATIVE_SKELETAL_OBJECTS">()
                    && (ptr.getType() == ESM::Activator::sRecordId || ptr.getType() == ESM::Static::sRecordId)
                    && nativeModelPath.value().ends_with(".nif"))
                {
                    auto entry = mNativeSkeletalAssets.find(nativeModelPath.value());
                    const bool inserted = entry == mNativeSkeletalAssets.end();
                    if (inserted) entry = mNativeSkeletalAssets.try_emplace(std::string(nativeModelPath.value())).first;
                    auto& native = entry->second;
                    if (inserted)
                    {
                        native.asset = mNativeAssets->resolve(nativeModelPath);
                        const auto* model = native.asset.available() ? mSession->world().get(native.asset.model) : nullptr;
                        const auto* skeleton = native.asset.skeleton ? mSession->world().get(*native.asset.skeleton) : nullptr;
                        if (model && skeleton && native.asset.controllers && native.asset.namedVisualCapabilities == 0)
                            native.program = RenderNative::SkeletalObjectProgram::bind(
                                mSession->world(), *model, *skeleton, *native.asset.controllers);
                        if (Debug::GameplayDiagnostics::sampling())
                            Debug::GameplayDiagnostics::recordEvent("native_skeletal_asset",
                                {{"model", std::string(nativeModelPath.value())}, {"reason", native.program.diagnostic()},
                                    {"skeleton", skeleton ? "1" : "0"},
                                    {"named_visuals", std::to_string(native.asset.namedVisualCapabilities)},
                                    {"asset", native.asset.diagnostic}});
                    }
                    std::vector<glm::mat4> pose;
                    const auto* model = native.asset.available() ? mSession->world().get(native.asset.model) : nullptr;
                    const auto* skeleton = native.asset.skeleton ? mSession->world().get(*native.asset.skeleton) : nullptr;
                    V4NativeAnimationPoseResult playback;
                    if (model && skeleton && mNativeAnimation)
                        playback = mNativeAnimation->captureSkeletalObjectPose(*identity, animation,
                            native.program, static_cast<float>(source.simulationTime), pose);
                    if (playback.applied())
                    {
                        skeletalPosesValidated += playback.poseValidated ? 1u : 0u;
                        skeletalPoseMaximumError = std::max(skeletalPoseMaximumError, playback.poseMaximumError);
                        auto dynamic = makeV4DynamicInstanceSource(ptr, native.asset.model,
                            *native.asset.skeleton, model->bounds, true);
                        if (!dynamic)
                        {
                            compatible = false;
                            mLastDiagnostic = "native skeletal prop has no placement: " + *identity;
                            return;
                        }
                        auto handle = mSession->cells().findInstance(*identity);
                        const auto* bound = handle ? mSession->world().get(*handle) : nullptr;
                        if (!bound || bound->model != native.asset.model || bound->skeleton != native.asset.skeleton)
                        {
                            const auto publication = mSession->cells().upsertDynamicInstance(*dynamic);
                            if (publication.status != RenderCore::ActiveCellPublishStatus::Applied
                                && publication.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)
                            {
                                compatible = false;
                                mLastDiagnostic = "native skeletal prop publication failed: " + *identity;
                                return;
                            }
                            handle = mSession->cells().findInstance(*identity);
                        }
                        if (!handle)
                        {
                            compatible = false;
                            mLastDiagnostic = "native skeletal prop lost its instance: " + *identity;
                            return;
                        }
                        RenderCore::SkeletonPoseInput input;
                        input.instance = *handle;
                        input.skeleton = *native.asset.skeleton;
                        input.localTransforms = std::move(pose);
                        source.skeletonPoses.push_back(std::move(input));
                        RenderCore::DynamicTransformInput placement;
                        placement.instance = *handle;
                        placement.transform = dynamic->transform;
                        source.dynamicTransforms.push_back(std::move(placement));
                        currentSkeletalObjects.emplace(*identity,
                            NativeSkeletalInstance{*handle, native.asset.model, *native.asset.skeleton});
                        return;
                    }
                    if (Debug::GameplayDiagnostics::sampling() && (++skeletalObjectFallbacks <= 4 || skeleton))
                        Debug::GameplayDiagnostics::recordEvent("native_skeletal_object_fallback",
                            {{"model", std::string(nativeModelPath.value())}, {"reason", native.program.diagnostic()},
                                {"playback", playback.diagnostic}, {"asset", native.asset.diagnostic}});
                }
                osg::Group* const evaluatedRoot = animation.getV4EffectRoot();
                if (!evaluatedRoot)
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated non-actor object has no authoritative OpenMW animation root: "
                        + *identity;
                    return;
                }

                std::optional<V4EffectCaptureResult> planned;
                V4PersistentObject* objectProducer = nullptr;
                if (Misc::environmentFlag<"OPENMW_V4_NATIVE_OBJECT_PRODUCERS">())
                {
                    if (auto* producer = animation.prepareV4PersistentObject())
                    {
                        objectProducer = producer;
                        const auto builds = producer->geometryBuilds;
                        const auto updates = producer->materialUpdates;
                        const auto reuses = producer->reusedDraws;
                        const auto clean = producer->cleanPublications;
                        const auto inspections = producer->bindingInspections;
                        planned = producer->publish("animated-object:" + *identity, mVfs, mTextureIdentities,
                            persistentDraws ? &mPersistentDraws : nullptr,
                            Settings::shaders().mApplyLightingToEnvironmentMaps);
                        cleanObjectPublications += producer->cleanPublications - clean;
                        objectBindingInspections += producer->bindingInspections - inspections;
                        if (Debug::GameplayDiagnostics::sampling())
                        {
                            auto& c = Debug::GameplayDiagnostics::context;
                            c.nativeObjectBuilds += static_cast<unsigned>(producer->geometryBuilds - builds);
                            c.nativeObjectUpdates += static_cast<unsigned>(producer->materialUpdates - updates);
                            c.nativeObjectReuses += static_cast<unsigned>(producer->reusedDraws - reuses);
                            if (!planned && c.nativeObjectFallbacks++ < 4)
                                Debug::GameplayDiagnostics::recordEvent("native_object_fallback", {
                                    {"model", std::string(modelPath.value())}, {"reason", producer->fallbackReason()} });
                        }
                    }
                }
                if (planned && objectProducer && objectProducer->hasIntrinsicParticles())
                {
                    V4AnimatedObjectCaptureVisitor particlesVisitor("animated-object:" + *identity, mVfs,
                        &mTextureIdentities, V4AnimatedObjectCaptureVisitor::Mode::ParticlesOnly);
                    particlesVisitor.setTraversalNumber(mPoseTraversal);
                    evaluatedRoot->accept(particlesVisitor);
                    auto particles = particlesVisitor.take();
                    if (particles.valid())
                    {
                        for (auto& draw : particles.draws)
                            planned->draws.push_back(std::move(draw));
                    }
                    else
                    {
                        // The known static body and the live particle path are
                        // atomic from the caller's perspective. Retire any
                        // retained slots before the whole-object fallback.
                        objectProducer->invalidate("intrinsic particle capture: " + particles.diagnostic);
                        planned.reset();
                    }
                }
                if (!planned && Misc::environmentFlag<"OPENMW_V4_OBJECT_BINDING_PLANS">())
                {
                    if (!mObjectCapturePlans) mObjectCapturePlans = std::make_unique<V4ObjectCapturePlans>();
                    planned = mObjectCapturePlans->capture(*evaluatedRoot, "animated-object:" + *identity,
                        mVfs, mTextureIdentities);
                }
                V4EffectCaptureResult capturedObject;
                if (planned) capturedObject = std::move(*planned);
                else
                {
                    V4AnimatedObjectCaptureVisitor objectVisitor("animated-object:" + *identity, mVfs, &mTextureIdentities);
                    objectVisitor.setTraversalNumber(mPoseTraversal);
                    evaluatedRoot->accept(objectVisitor);
                    capturedObject = objectVisitor.take();
                }
                if (!capturedObject.valid())
                {
                    compatible = false;
                    mLastDiagnostic = capturedObject.diagnostic.empty()
                        ? "evaluated non-actor object could not produce native Vulkan compatibility draws: " + *identity
                        : "evaluated non-actor object " + *identity + ": " + capturedObject.diagnostic;
                    mLastDiagnostic += " [model=" + std::string(modelPath.value()) + "]";
                    return;
                }
                if (capturedObject.draws.empty() && !persistentDraws)
                {
                    const char* strictQc = std::getenv("OPENMW_V4_STRICT_QC");
                    if (strictQc && strictQc[0] != '\0' && strictQc[0] != '0')
                        Log(Debug::Warning) << "V4 strict QC evaluated object produced no draws identity="
                                            << *identity << " model=" << modelPath.value();
                }

                std::optional<V4EffectCaptureResult> capturedEffects;
                if (animation.hasV4UpdateVfxAttachments())
                {
                    v4_effect_detail::CaptureVisitor effectVisitor("animated-object-effect:" + *identity, false, mVfs, &mTextureIdentities);
                    effectVisitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
                    effectVisitor.setTraversalNumber(mPoseTraversal);
                    evaluatedRoot->accept(effectVisitor);
                    capturedEffects.emplace(effectVisitor.take());
                    if (!capturedEffects->valid())
                    {
                        compatible = false;
                        mLastDiagnostic = capturedEffects->diagnostic.empty()
                            ? "evaluated non-actor attached effect could not produce native Vulkan compatibility draws: "
                                + *identity
                            : "evaluated non-actor attached effect " + *identity + ": " + capturedEffects->diagnostic;
                        return;
                    }
                }

                constexpr std::uint64_t worldObjectFlags
                    = RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OrdinaryWorld)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ReflectionEligible)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::RefractionEligible);
                for (RenderCore::ImmediateEffectDraw& draw : capturedObject.draws)
                {
                    draw.semanticFlags = worldObjectFlags;
                    source.immediateEffectDraws.push_back(std::move(draw));
                }
                if (capturedEffects)
                {
                    for (RenderCore::ImmediateEffectDraw& draw : capturedEffects->draws)
                        source.immediateEffectDraws.push_back(std::move(draw));
                }
                return;
            }

            const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
            if (!identity)
            {
                compatible = false;
                mLastDiagnostic = "active actor has no stable content identity";
                return;
            }
            if (animation.hasV4UpdateVfxAttachments())
            {
                osg::Group* const effectRoot = animation.getV4EffectRoot();
                if (!effectRoot)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor effect attachment has no evaluated source root";
                    return;
                }
                V4EffectCaptureResult captured
                    = captureV4AttachedEffects(*effectRoot, "actor-effect:" + *identity, mVfs, &mTextureIdentities, mPoseTraversal);
                if (!captured.valid())
                {
                    compatible = false;
                    mLastDiagnostic = captured.diagnostic.empty()
                        ? "active actor effect attachment could not produce evaluated V4 draws"
                        : captured.diagnostic;
                    return;
                }
                for (RenderCore::ImmediateEffectDraw& draw : captured.draws)
                    source.immediateEffectDraws.push_back(std::move(draw));
            }
            const std::optional<NifRender::StaticModelCacheResult> base
                = ensureModelPublished(*mSession, mVfs, animation.getV4SourceModel(), &mTextureIdentities);
            if (!base)
            {
                compatible = false;
                mLastDiagnostic = "active actor source model could not be published";
                return;
            }
            const RenderCore::ModelRecord* const baseRecord = mSession->world().get(base->model);
            if (!baseRecord || !baseRecord->payload)
            {
                compatible = false;
                mLastDiagnostic = "active actor source model has no current canonical model payload";
                return;
            }

            const bool particleActor = baseRecord->dynamicRequirements
                == RenderCore::modelDynamicRequirement(RenderCore::ModelDynamicRequirement::ParticleSystem)
                && std::getenv("OPENMW_V4_REJECT_PARTICLE_ACTORS") == nullptr;
            if (particleActor)
            {
                osg::Node* root = animation.getV4EffectRoot();
                if (!root)
                {
                    compatible = false;
                    mLastDiagnostic = "particle actor has no evaluated root: " + *identity;
                    return;
                }
                V4EffectCaptureResult captured = captureV4ParticleActor(
                    *root, "particle-actor:" + *identity, mVfs, &mTextureIdentities, mPoseTraversal);
                if (!captured.valid())
                {
                    compatible = false;
                    mLastDiagnostic = "particle actor " + *identity + " ("
                        + std::string(animation.getV4SourceModel().value()) + "): " + captured.diagnostic;
                    return;
                }
                // A model change can move an existing actor into this route.
                // Retire its persistent body before publishing evaluated draws.
                if (mSession->cells().findInstance(*identity)
                    && mSession->cells().removeInstance(*identity).status
                        != RenderCore::ActiveCellPublishStatus::Applied)
                {
                    compatible = false;
                    mLastDiagnostic = "particle actor persistent body retirement failed: " + *identity;
                    return;
                }
                for (auto& draw : captured.draws)
                    source.immediateEffectDraws.push_back(std::move(draw));
            }
            else
            {
                std::optional<RenderCore::SkeletonHandle> actorSkeleton = base->skeleton;
                // A translated skin palette only includes skin-required bones. NPC
                // equipment and body-part attachment nodes may not influence that
                // base skin at all. Like OpenMW's base-only actor skeleton, retain
                // all canonical transform bones and capture their evaluated poses.
                // Non-NPC producers retain their existing translated-skeleton path.
                if (!actorSkeleton || dynamic_cast<NpcAnimation*>(&animation) != nullptr)
                {
                    const std::string skeletonIdentity(animation.getV4SourceModel().value());
                    const auto cached = mForcedActorSkeletons.find(skeletonIdentity);
                    if (cached != mForcedActorSkeletons.end() && mSession->world().get(cached->second))
                        actorSkeleton = cached->second;
                    else
                    {
                        NifRender::ForcedActorSkeleton forced = NifRender::buildForcedActorSkeleton(
                            *baseRecord, "runtime:forced-actor-skeleton:" + skeletonIdentity);
                        if (!forced.valid())
                        {
                            compatible = false;
                            mLastDiagnostic = "active actor source model cannot reproduce OpenMW's forced skeleton: "
                                + forced.diagnostic;
                            return;
                        }
                        const std::optional<RenderCore::SkeletonHandle> reserved = mSession->world().reserveSkeleton();
                        if (!reserved)
                        {
                            compatible = false;
                            mLastDiagnostic = "forced actor skeleton handle reservation failed";
                            return;
                        }
                        RenderCore::RenderWorldUpdateBatch batch(mSession->world().epoch(),
                            mSession->publisher().nextSequence(), forced.record.sourceIdentity);
                        if (!batch.add(RenderCore::CreateSkeleton{ *reserved, std::move(forced.record) })
                            || !batch.seal()
                            || mSession->publisher().apply(batch) != RenderCore::PublishStatus::Applied)
                        {
                            mSession->world().cancel(*reserved);
                            compatible = false;
                            mLastDiagnostic = "forced actor skeleton publication failed";
                            return;
                        }
                        mForcedActorSkeletons.insert_or_assign(skeletonIdentity, *reserved);
                        actorSkeleton = *reserved;
                    }
                }
                RenderCore::ModelHandle actorModel = base->model;
                std::vector<EvaluatedMorphSource> evaluatedPartMorphs;
                if (auto* npc = dynamic_cast<NpcAnimation*>(&animation))
                {
                    std::vector<NifRender::ActorPartModelSource> parts;
                    std::string signature(animation.getV4SourceModel().value());
                    const auto bindEvaluatedMorphs = [&](RenderCore::ModelHandle partModel, osg::Node* evaluatedRoot,
                                                         std::string_view diagnosticIdentity) {
                        const RenderCore::ModelRecord* const partRecord = mSession->world().get(partModel);
                        if (!partRecord || !partRecord->payload)
                        {
                            compatible = false;
                            mLastDiagnostic = std::string(diagnosticIdentity) + " has no current model payload";
                            return;
                        }

                        std::vector<RenderCore::MeshHandle> neutralMorphMeshes;
                        bool hasSkinnedGeometry = false;
                        for (const RenderCore::ModelNodeRecord& node : partRecord->payload->nodes)
                        {
                            const RenderCore::MeshRecord* const mesh
                                = node.mesh ? mSession->world().get(*node.mesh) : nullptr;
                            if (mesh && mesh->skin)
                                hasSkinnedGeometry = true;
                            if (mesh && mesh->morphed)
                                neutralMorphMeshes.push_back(*node.mesh);
                        }
                        // SceneUtil::attach treats any template containing RigGeometry as a
                        // skeleton and CopyRigVisitor copies only matching RigGeometry into
                        // the actor. Standalone MorphGeometry siblings are deliberately not
                        // part of that evaluated attachment. composeActorModel mirrors the
                        // same selection, so those uncomposed morph meshes require no live
                        // weight binding.
                        if (hasSkinnedGeometry)
                            return;
                        if (neutralMorphMeshes.empty())
                            return;
                        if (!evaluatedRoot)
                        {
                            compatible = false;
                            mLastDiagnostic = std::string(diagnosticIdentity) + " has no evaluated morph root";
                            return;
                        }

                        MorphCollector collector;
                        evaluatedRoot->accept(collector);
                        if (collector.morphs.size() != neutralMorphMeshes.size())
                        {
                            compatible = false;
                            mLastDiagnostic = std::string(diagnosticIdentity)
                                + " evaluated morph topology does not match its published model";
                            return;
                        }
                        for (std::size_t i = 0; i < neutralMorphMeshes.size(); ++i)
                            evaluatedPartMorphs.push_back({ neutralMorphMeshes[i], collector.morphs[i], false });
                    };
                    for (const NpcAnimation::V4PartSource& part : npc->getV4PartSources())
                    {
                        std::string partFailure;
                        const std::optional<NifRender::StaticModelCacheResult> published
                            = ensureModelPublished(*mSession, mVfs, part.model, &mTextureIdentities, &partFailure);
                        if (!published)
                        {
                            compatible = false;
                            mLastDiagnostic
                                = "NPC part '" + std::string(part.model.value()) + "' failed: " + partFailure;
                            return;
                        }

                        RenderCore::ModelHandle partModel = published->model;
                        std::string glowSignature;
                        if (part.enchantedGlow)
                        {
                            const int slot = npc->getV4PartSlot(part.type);
                            MWWorld::InventoryStore& inventory = ptr.getClass().getInventoryStore(ptr);
                            const auto item = slot >= 0 ? inventory.getSlot(slot) : inventory.end();
                            if (item == inventory.end() || item->getClass().getEnchantment(*item).empty())
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC enchanted part cannot resolve its authoritative equipped item";
                                return;
                            }
                            const osg::Vec4f sourceColor = item->getClass().getEnchantmentColor(*item);
                            const RenderCore::Color color{ sourceColor.r(), sourceColor.g(), sourceColor.b(),
                                sourceColor.a() };
                            const NifRender::EnchantedGlowPublishResult glow
                                = NifRender::publishEnchantedGlowVariant(mSession->world(), mSession->publisher(), mVfs,
                                    partModel, color, Settings::shaders().mApplyLightingToEnvironmentMaps);
                            if (!glow.available())
                            {
                                compatible = false;
                                mLastDiagnostic = enchantedGlowDiagnostic(glow.status);
                                return;
                            }
                            partModel = glow.model;
                            const RenderCore::ModelRecord* variant = mSession->world().get(partModel);
                            if (!variant)
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC enchanted part variant returned a stale model handle";
                                return;
                            }
                            glowSignature = ":glow=" + variant->sourceIdentity;
                        }

                        bindEvaluatedMorphs(
                            partModel, part.evaluatedRoot, "NPC part '" + std::string(part.model.value()) + "'");
                        if (!compatible)
                            return;
                        parts.push_back({ partModel, part.boneName, part.visible, std::string(part.model.value()) });
                        signature += "\n" + std::to_string(static_cast<unsigned int>(part.type)) + ":"
                            + std::string(part.model.value()) + ":" + part.boneName + ":" + (part.visible ? "1" : "0")
                            + glowSignature;
                    }
                    if (!compatible)
                        return;

                    if (osg::Node* attachedAmmunition = npc->getAttachedAmmunitionNode())
                    {
                        MWWorld::InventoryStore& inventory = ptr.getClass().getInventoryStore(ptr);
                        const auto ammo = inventory.getSlot(MWWorld::InventoryStore::Slot_Ammunition);
                        osg::Group* const arrowBone = npc->getArrowBone();
                        if (ammo == inventory.end() || !arrowBone || arrowBone->getName().empty())
                        {
                            compatible = false;
                            mLastDiagnostic
                                = "NPC attached ammunition cannot resolve its authoritative item or attachment bone";
                            return;
                        }

                        const VFS::Path::Normalized ammoModel = ammo->getClass().getCorrectedModel(*ammo);
                        const std::optional<NifRender::StaticModelCacheResult> publishedAmmo
                            = ensureModelPublished(*mSession, mVfs, ammoModel, &mTextureIdentities);
                        if (!publishedAmmo)
                        {
                            compatible = false;
                            mLastDiagnostic
                                = "NPC attached ammunition is missing from the winning VFS or failed translation";
                            return;
                        }

                        RenderCore::ModelHandle ammoModelHandle = publishedAmmo->model;
                        std::string ammoGlowSignature;
                        if (!ammo->getClass().getEnchantment(*ammo).empty())
                        {
                            const osg::Vec4f sourceColor = ammo->getClass().getEnchantmentColor(*ammo);
                            const RenderCore::Color color{ sourceColor.r(), sourceColor.g(), sourceColor.b(),
                                sourceColor.a() };
                            const NifRender::EnchantedGlowPublishResult glow
                                = NifRender::publishEnchantedGlowVariant(mSession->world(), mSession->publisher(), mVfs,
                                    ammoModelHandle, color, Settings::shaders().mApplyLightingToEnvironmentMaps);
                            if (!glow.available())
                            {
                                compatible = false;
                                mLastDiagnostic = enchantedGlowDiagnostic(glow.status);
                                return;
                            }
                            ammoModelHandle = glow.model;
                            const RenderCore::ModelRecord* variant = mSession->world().get(ammoModelHandle);
                            if (!variant)
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC enchanted ammunition variant returned a stale model handle";
                                return;
                            }
                            ammoGlowSignature = ":glow=" + variant->sourceIdentity;
                        }

                        const bool ammoVisible = attachedAmmunition->getNodeMask() != 0u;
                        bindEvaluatedMorphs(ammoModelHandle, attachedAmmunition,
                            "NPC ammunition '" + std::string(ammoModel.value()) + "'");
                        if (!compatible)
                            return;
                        parts.push_back(
                            { ammoModelHandle, arrowBone->getName(), ammoVisible, std::string(ammoModel.value()) });
                        signature += "\nammunition:" + std::string(ammoModel.value()) + ":" + arrowBone->getName() + ":"
                            + (ammoVisible ? "1" : "0") + ammoGlowSignature;
                    }

                    auto entry = mComposedActors.find(*identity);
                    if (entry == mComposedActors.end() || entry->second.signature != signature
                        || !mSession->world().get(entry->second.model))
                    {
                        NifRender::ComposedActorModel composed = NifRender::composeActorModel(
                            mSession->world(), base->model, *actorSkeleton, parts, "runtime:npc:" + *identity);
                        if (!composed.valid())
                        {
                            compatible = false;
                            mLastDiagnostic = "NPC '" + *identity + "' base='"
                                + std::string(animation.getV4SourceModel().value()) + "' cell='"
                                + std::string(ptr.getCell() ? ptr.getCell()->getCell()->getDescription() : "<none>")
                                + "': " + composed.diagnostic;
                            return;
                        }

                        RenderCore::ModelHandle composedHandle;
                        RenderCore::RenderWorldUpdateBatch batch(mSession->world().epoch(),
                            mSession->publisher().nextSequence(), "runtime:npc:" + *identity);
                        if (entry == mComposedActors.end() || !mSession->world().get(entry->second.model))
                        {
                            const std::optional<RenderCore::ModelHandle> reserved = mSession->world().reserveModel();
                            if (!reserved)
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC composite model handle reservation failed";
                                return;
                            }
                            composedHandle = *reserved;
                            if (!batch.add(RenderCore::CreateModel{ composedHandle, std::move(composed.record) })
                                || !batch.seal()
                                || mSession->publisher().apply(batch) != RenderCore::PublishStatus::Applied)
                            {
                                mSession->world().cancel(composedHandle);
                                compatible = false;
                                mLastDiagnostic = "NPC composite model publication failed";
                                return;
                            }
                        }
                        else
                        {
                            composedHandle = entry->second.model;
                            const RenderCore::ModelRecord* current = mSession->world().get(composedHandle);
                            const std::optional<RenderCore::ResourceRevision> revision
                                = current ? RenderCore::advanceMonotonic(current->revision) : std::nullopt;
                            if (!revision)
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC composite model revision exhausted";
                                return;
                            }
                            composed.record.revision = *revision;
                            if (!batch.add(RenderCore::UpdateModel{ composedHandle, std::move(composed.record) })
                                || !batch.seal()
                                || mSession->publisher().apply(batch) != RenderCore::PublishStatus::Applied)
                            {
                                compatible = false;
                                mLastDiagnostic = "NPC equipment model replacement failed atomically";
                                return;
                            }
                        }
                        mComposedActors[*identity] = { composedHandle, std::move(signature) };
                        actorModel = composedHandle;
                    }
                    else
                        actorModel = entry->second.model;
                }

                if (const std::optional<osg::Vec4f> sourceColor = animation.getV4GlowColor())
                {
                    const RenderCore::Color color{ sourceColor->r(), sourceColor->g(), sourceColor->b(),
                        sourceColor->a() };
                    const NifRender::EnchantedGlowPublishResult glow
                        = NifRender::publishEnchantedGlowVariant(mSession->world(), mSession->publisher(), mVfs,
                            actorModel, color, Settings::shaders().mApplyLightingToEnvironmentMaps, true);
                    if (!glow.available())
                    {
                        compatible = false;
                        mLastDiagnostic
                            = "actor spell-cast glow publication failed: " + enchantedGlowDiagnostic(glow.status);
                        return;
                    }
                    actorModel = glow.model;
                    if (!mSession->world().get(actorModel))
                    {
                        compatible = false;
                        mLastDiagnostic = "actor spell-cast glow variant returned a stale model handle";
                        return;
                    }
                }

                std::optional<RenderCore::InstanceHandle> handle = mSession->cells().findInstance(*identity);
                const RenderCore::InstanceRecord* bound = handle ? mSession->world().get(*handle) : nullptr;
                if (!bound || bound->model != actorModel || bound->skeleton != actorSkeleton)
                {
                    const RenderCore::ModelRecord* modelRecord = mSession->world().get(actorModel);
                    const std::optional<RenderCore::DynamicInstanceSource> dynamic = modelRecord
                        ? makeV4DynamicInstanceSource(ptr, actorModel, *actorSkeleton, modelRecord->bounds)
                        : std::nullopt;
                    if (!dynamic)
                    {
                        compatible = false;
                        mLastDiagnostic = "active actor could not produce a dynamic instance source";
                        return;
                    }
                    const RenderCore::ActiveCellPublishResult published
                        = mSession->cells().upsertDynamicInstance(*dynamic);
                    if (published.status != RenderCore::ActiveCellPublishStatus::Applied
                        && published.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)
                    {
                        compatible = false;
                        mLastDiagnostic = "active actor instance publication failed for " + *identity + " in cell "
                            + dynamic->cellIdentity + " (status "
                            + std::to_string(static_cast<unsigned int>(published.status)) + ", world status "
                            + std::to_string(static_cast<unsigned int>(published.worldStatus)) + ")";
                        return;
                    }
                    handle = published.instance.valid() ? std::optional<RenderCore::InstanceHandle>(published.instance)
                                                        : mSession->cells().findInstance(*identity);
                }
                const RenderCore::InstanceRecord* instance = handle ? mSession->world().get(*handle) : nullptr;
                const RenderCore::SkeletonRecord* skeleton
                    = instance && instance->skeleton ? mSession->world().get(*instance->skeleton) : nullptr;
                if (!handle || !instance || !skeleton || !skeleton->payload)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor has no current V4 instance/skeleton binding";
                    return;
                }

                const RenderCore::ModelRecord* model
                    = instance->model ? mSession->world().get(*instance->model) : nullptr;
                if (!model || !model->payload)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor has no current V4 model payload";
                    return;
                }
                if (model->dynamicRequirements != 0)
                {
                    compatible = false;
                    mLastDiagnostic = modelDynamicRequirementDiagnostic(model->dynamicRequirements)
                        + ": " + *identity + " (" + std::string(animation.getV4SourceModel().value())
                        + ", requirements=" + std::to_string(model->dynamicRequirements) + ")";
                    return;
                }
                std::vector<std::pair<RenderCore::ModelNodeIndex, const RenderCore::MeshRecord*>> morphNodes;
                for (std::size_t nodeIndex = 0; nodeIndex < model->payload->nodes.size(); ++nodeIndex)
                {
                    const RenderCore::ModelNodeRecord& node = model->payload->nodes[nodeIndex];
                    const RenderCore::MeshRecord* mesh = node.mesh ? mSession->world().get(*node.mesh) : nullptr;
                    if (mesh && mesh->morphed)
                        morphNodes.emplace_back(
                            RenderCore::ModelNodeIndex{ static_cast<std::uint32_t>(nodeIndex) }, mesh);
                }

                SceneUtil::Skeleton* evaluated = animation.getSkeleton();
                RenderCore::SkeletonPoseInput pose;
                pose.instance = *handle;
                pose.skeleton = *instance->skeleton;

                V4NativeAnimationPoseResult nativePose;
                if (evaluated && mNativeAnimation)
                    nativePose = mNativeAnimation->captureSkeletonPose(
                        *identity, animation, *skeleton, pose.localTransforms);

                std::vector<glm::mat4> global;
                if (nativePose.applied())
                {
                    ++nativeActorPoses;
                    nativeSampledTracks += nativePose.sampledTracks;
                }
                else
                {
                    ++legacyActorPoses;
                    if (Debug::GameplayDiagnostics::sampling() && evaluated
                        && nativeFallbackEvents++ < 4 && !nativePose.diagnostic.empty())
                        Debug::GameplayDiagnostics::recordEvent("native_animation_fallback",
                            { { "identity", *identity }, { "reason", nativePose.diagnostic } });

                    if (!evaluated)
                    {
                        const bool skinned = std::any_of(model->payload->nodes.begin(), model->payload->nodes.end(),
                            [&](const RenderCore::ModelNodeRecord& node) {
                                const auto* mesh = node.mesh ? mSession->world().get(*node.mesh) : nullptr;
                                return mesh && mesh->skinned;
                            });
                        if (skinned || !animation.getObjectRoot()
                            || !captureV4RigidActorPose(
                                *animation.getObjectRoot(), *skeleton->payload, global, mLastDiagnostic))
                        {
                            compatible = false;
                            if (mLastDiagnostic.empty())
                                mLastDiagnostic = "actor cannot supply its evaluated pose";
                            mLastDiagnostic
                                += " actor=" + *identity + " model=" + std::string(animation.getV4SourceModel().value());
                            return;
                        }
                    }
                    else
                    {
                        std::vector<SceneUtil::Bone*> evaluatedBones;
                        evaluatedBones.reserve(skeleton->payload->bones.size());
                        for (const RenderCore::BoneRecord& bone : skeleton->payload->bones)
                        {
                            SceneUtil::Bone* sourceBone = evaluated->getBone(bone.name);
                            if (!sourceBone)
                            {
                                compatible = false;
                                mLastDiagnostic = "evaluated actor skeleton is missing required bone " + bone.name
                                    + " from " + skeleton->sourceIdentity;
                                return;
                            }
                            evaluatedBones.push_back(sourceBone);
                        }
                        evaluated->updateBoneMatrices(mPoseTraversal);
                        global.reserve(evaluatedBones.size());
                        for (const SceneUtil::Bone* bone : evaluatedBones)
                            global.push_back(toGlm(bone->mMatrixInSkeletonSpace));
                    }

                    pose.localTransforms.resize(global.size());
                    for (std::size_t i = 0; i < global.size(); ++i)
                    {
                        const std::int32_t parent = skeleton->payload->bones[i].parent;
                        pose.localTransforms[i]
                            = parent < 0 ? global[i]
                                       : glm::inverse(global[static_cast<std::size_t>(parent)]) * global[i];
                        if (!finite(pose.localTransforms[i]))
                        {
                            compatible = false;
                            mLastDiagnostic = "evaluated actor pose contains a non-finite local transform";
                            return;
                        }
                    }
                }

                if (pose.localTransforms.size() != skeleton->payload->bones.size())
                {
                    compatible = false;
                    mLastDiagnostic = "actor pose does not match the published skeleton bone count";
                    return;
                }
                for (const glm::mat4& local : pose.localTransforms)
                {
                    if (!finite(local))
                    {
                        compatible = false;
                        mLastDiagnostic = "actor pose contains a non-finite local transform";
                        return;
                    }
                }

                // The forced legacy benchmark arm never resumes native posing.
                // Do not charge it for native history decomposition/maintenance.
                // Ordinary compatibility fallback still refreshes the exact seed.
                if (!nativePose.applied() && mNativeAnimation
                    && !std::getenv("OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL"))
                {
                    std::string seedDiagnostic;
                    if (!mNativeAnimation->seedSkeletonPose(
                            *identity, *skeleton, pose.localTransforms, seedDiagnostic))
                    {
                        ++nativeSeedFailures;
                        if (Debug::GameplayDiagnostics::sampling() && nativeFallbackEvents++ < 4
                            && !seedDiagnostic.empty())
                            Debug::GameplayDiagnostics::recordEvent("native_animation_seed_fallback",
                                { { "identity", *identity }, { "reason", seedDiagnostic } });
                    }
                }

                if (Debug::GameplayDiagnostics::detailedSampling() && source.skeletonPoses.size() < 16)
                    Debug::GameplayDiagnostics::recordEvent("actor_pose",
                        { { "actor",
                              std::to_string(mSession->world().epoch().value()) + ":" + std::to_string(handle->slot())
                                  + ":" + std::to_string(handle->generation()) },
                            { "identity", *identity }, { "skeleton", skeleton->sourceIdentity },
                            { "bones", std::to_string(pose.localTransforms.size()) },
                            { "source", nativePose.applied() ? "native_kf" : "legacy_osg" },
                            { "global_hash", nativePose.applied() ? "not_captured"
                                : std::to_string(Debug::GameplayDiagnostics::fingerprint(
                                    global.data(), global.size() * sizeof(glm::mat4))) },
                            { "local_hash",
                                std::to_string(Debug::GameplayDiagnostics::fingerprint(
                                    pose.localTransforms.data(), pose.localTransforms.size() * sizeof(glm::mat4))) },
                            { "pose_traversal", std::to_string(mPoseTraversal) } });
                source.skeletonPoses.push_back(std::move(pose));

                if (!morphNodes.empty())
                {
                    if (!animation.getObjectRoot())
                    {
                        compatible = false;
                        mLastDiagnostic = "active actor morph source has no evaluated object root";
                        return;
                    }
                    MorphCollector collector;
                    const bool npcActor = dynamic_cast<NpcAnimation*>(&animation) != nullptr;
                    if (!npcActor)
                        animation.getObjectRoot()->accept(collector);

                    // NPC composition reuses each exact published part mesh handle,
                    // so that handle is the authoritative correspondence key for its
                    // evaluated clone. Non-NPC actors retain stable name/occurrence
                    // matching against their single evaluated object root.
                    std::unordered_map<std::string, std::vector<SceneUtil::MorphGeometry*>> evaluatedByName;
                    for (SceneUtil::MorphGeometry* morph : collector.morphs)
                    {
                        if (!morph || morph->getName().empty())
                        {
                            compatible = false;
                            mLastDiagnostic = "evaluated actor morph geometry has no stable node name";
                            return;
                        }
                        evaluatedByName[Misc::StringUtils::lowerCase(morph->getName())].push_back(morph);
                    }
                    std::unordered_map<std::string, std::size_t> nameCursor;
                    for (std::size_t i = 0; i < morphNodes.size(); ++i)
                    {
                        const RenderCore::ModelNodeRecord& node = model->payload->nodes[morphNodes[i].first.value()];
                        SceneUtil::MorphGeometry* evaluatedMorph = nullptr;
                        if (npcActor)
                        {
                            const auto binding = std::find_if(evaluatedPartMorphs.begin(), evaluatedPartMorphs.end(),
                                [&](const EvaluatedMorphSource& candidate) {
                                    return !candidate.consumed && node.mesh && candidate.mesh == *node.mesh;
                                });
                            if (binding != evaluatedPartMorphs.end())
                            {
                                binding->consumed = true;
                                evaluatedMorph = binding->geometry;
                            }
                        }
                        else
                        {
                            const std::string foldedName = Misc::StringUtils::lowerCase(node.name);
                            const auto named = evaluatedByName.find(foldedName);
                            const std::size_t occurrence = nameCursor[foldedName]++;
                            if (!node.name.empty() && named != evaluatedByName.end()
                                && occurrence < named->second.size())
                                evaluatedMorph = named->second[occurrence];
                        }
                        if (!evaluatedMorph)
                        {
                            compatible = false;
                            mLastDiagnostic
                                = "evaluated actor morph source does not match translated node " + node.name;
                            return;
                        }
                        if (!morphNodes[i].second->morphs)
                        {
                            compatible = false;
                            mLastDiagnostic = "translated actor morph node has no target payload";
                            return;
                        }
                        RenderCore::MorphWeightInput weights;
                        weights.instance = *handle;
                        weights.mesh = *node.mesh;
                        weights.modelNode = morphNodes[i].first;
                        for (const RenderCore::MorphTargetPayload& target : morphNodes[i].second->morphs->targets)
                        {
                            if (target.sourceIndex >= evaluatedMorph->getMorphTargetList().size())
                            {
                                compatible = false;
                                mLastDiagnostic
                                    = "evaluated actor morph target count is incompatible with translated data";
                                return;
                            }
                            weights.weights.push_back(evaluatedMorph->getMorphTarget(target.sourceIndex).getWeight());
                        }
                        source.morphWeights.push_back(std::move(weights));
                    }
                }

                const std::optional<RenderCore::DynamicInstanceSource> dynamic = makeV4DynamicInstanceSource(
                    animation.getPtr(), *instance->model, *instance->skeleton, instance->localBounds);
                if (!dynamic)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor placement could not be captured";
                    return;
                }
                RenderCore::DynamicTransformInput transform;
                transform.instance = *handle;
                transform.transform = dynamic->transform;
                transform.opacity = animation.getV4Alpha() * animation.getV4ActorFade();
                source.dynamicTransforms.push_back(std::move(transform));
            }

            const std::vector<Animation::V4AttachedLightSource> attachedLights
                = animation.captureV4AttachedLights(mPoseTraversal);
            const std::optional<RenderCore::ActiveCellSource> activeCell = attachedLights.empty()
                ? std::optional<RenderCore::ActiveCellSource>{}
                : (ptr.getCell() ? makeV4ActiveCellSource(*ptr.getCell()) : std::nullopt);
            if (!attachedLights.empty() && !activeCell)
            {
                compatible = false;
                mLastDiagnostic = "active actor-local lights have no active cell identity";
                return;
            }
            for (std::size_t lightIndex = 0; lightIndex < attachedLights.size(); ++lightIndex)
            {
                const Animation::V4AttachedLightSource& attached = attachedLights[lightIndex];
                RenderCore::CellLightSource light;
                light.identity = *identity + ":actor-light:" + std::to_string(lightIndex);
                light.cellIdentity = activeCell->identity;
                light.light.position = { attached.worldPosition.x(), attached.worldPosition.y(),
                    attached.worldPosition.z() };
                light.light.diffuse = { attached.diffuse.r(), attached.diffuse.g(), attached.diffuse.b(),
                    attached.diffuse.a() };
                light.light.specular = { attached.specular.r(), attached.specular.g(), attached.specular.b(),
                    attached.specular.a() };
                light.light.ambient = { attached.ambient.r(), attached.ambient.g(), attached.ambient.b(),
                    attached.ambient.a() };
                light.light.constantAttenuation = attached.constantAttenuation;
                light.light.linearAttenuation = attached.linearAttenuation;
                light.light.quadraticAttenuation = attached.quadraticAttenuation;
                light.light.effectiveRadius = attached.radius;
                light.light.actorFade = attached.actorFade;
                light.light.semanticFlags = RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Dynamic);
                if (attached.carryable)
                    light.light.semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Carryable);
                const RenderCore::ActiveCellPublishResult published = mSession->cells().upsertLight(light);
                if (published.status != RenderCore::ActiveCellPublishStatus::Applied
                    && published.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)
                {
                    compatible = false;
                    mLastDiagnostic = "actor-local light publication failed";
                    return;
                }
                currentActorLights.insert(light.identity);
            }
        });
        if (cachedObjectAdmission && Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("object_admission_cache", {
                {"reused", std::to_string(reusedObjectAdmissions)},
                {"skipped", std::to_string(skippedObjectAdmissions)}});
        if (mNativeAnimation)
            mNativeAnimation->endFrame();
        // Withdraw only this producer's instances on unload/disable or newly
        // unsupported state. Backend retirement preserves in-flight GPU lifetime.
        if (compatible)
        {
            for (const auto& [identity, owned] : mNativeSkeletalObjects)
                if (!currentSkeletalObjects.contains(identity))
                {
                    const auto handle = mSession->cells().findInstance(identity);
                    const auto* bound = handle ? mSession->world().get(*handle) : nullptr;
                    if (!bound || *handle != owned.instance || bound->model != owned.model
                        || bound->skeleton != owned.skeleton) continue;
                    const auto retired = mSession->cells().removeDynamicInstance(identity);
                    if (retired.status != RenderCore::ActiveCellPublishStatus::Applied
                        && retired.status != RenderCore::ActiveCellPublishStatus::NotFound)
                    {
                        compatible = false;
                        mLastDiagnostic = "native skeletal prop retirement failed: " + identity;
                        break;
                    }
                }
            if (compatible) mNativeSkeletalObjects = std::move(currentSkeletalObjects);
        }
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("native_skeletal_objects",
                {{"skeletal_objects_published", std::to_string(mNativeSkeletalObjects.size())},
                    {"skeletal_poses_validated", std::to_string(skeletalPosesValidated)},
                    {"skeletal_pose_max_error", std::to_string(skeletalPoseMaximumError)},
                    {"skeletal_object_fallbacks", std::to_string(skeletalObjectFallbacks)}});
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("native_animation_runtime",
                { { "native_actor_poses", std::to_string(nativeActorPoses) },
                    { "legacy_actor_poses", std::to_string(legacyActorPoses) },
                    { "sampled_tracks", std::to_string(nativeSampledTracks) },
                    { "seed_failures", std::to_string(nativeSeedFailures) },
                    { "legacy_control",
                        std::getenv("OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL") ? "1" : "0" } });
        if (compatible)
        {
            for (const std::string& identity : mActorLights)
            {
                if (currentActorLights.contains(identity))
                    continue;
                const RenderCore::ActiveCellPublishResult removed = mSession->cells().removeLight(identity);
                if (removed.status != RenderCore::ActiveCellPublishStatus::Applied
                    && removed.status != RenderCore::ActiveCellPublishStatus::NotFound)
                {
                    compatible = false;
                    mLastDiagnostic = "stale actor-local light retirement failed";
                    break;
                }
            }
            if (compatible)
                mActorLights = std::move(currentActorLights);
        }
        if (!compatible)
        {
            source.dynamicTransforms.clear();
            source.skeletonPoses.clear();
            source.morphWeights.clear();
            source.immediateEffectDraws.clear();
        }
        if (Debug::GameplayDiagnostics::sampling())
            Debug::GameplayDiagnostics::recordEvent("object_producer_work", {
                {"clean_publications", std::to_string(cleanObjectPublications)},
                {"binding_inspections", std::to_string(objectBindingInspections)}});
        if (compatible && persistentDraws) source.persistentDraws = mPersistentDraws.finish();
        return compatible;
    }

    bool V4EngineRenderBridge::prepareGuiFrame()
    {
        mLastDiagnostic.clear();
        if (!mSession)
        {
            mLastDiagnostic = "V4 GUI preparation has no semantic session";
            return false;
        }
        RenderVsg::VsgRuntimeHost& host = mSession->bootstrap().renderer();
        if (host.prepareGui())
            return true;
        mLastDiagnostic = host.lastDiagnostic().empty()
            ? "Vulkan MyGUI state could not be captured before Lua worker release"
            : host.lastDiagnostic();
        return false;
    }

    void V4EngineRenderBridge::publishImmediateEffects(
        RenderCore::SingleViewFrameInput& input, const V4MainFrameSource& source)
    {
        Debug::GameplayDiagnostics::Stage handoff("effect_snapshot_publish");
        input.persistentDraws = source.persistentDraws;
        if (std::getenv("OPENMW_V4_LEGACY_FRAME_HANDOFF_CONTROL"))
        {
            input.immediateEffectDraws = source.immediateEffectDraws;
            return;
        }
        const bool parallel = std::getenv("OPENMW_V4_PARALLEL_EFFECT_PUBLICATION") != nullptr;
        if (parallel && !mEffectPublicationWorkers)
        {
            const auto hardware = std::thread::hardware_concurrency();
            mEffectPublicationWorkers = std::make_unique<RenderCore::BoundedParallelFor>(
                hardware > 1 ? std::min(hardware - 1, 3u) : 0u);
        }
        input.ownedImmediateEffects = std::make_shared<const RenderCore::OwnedImmediateEffects>(
            source.immediateEffectDraws, parallel ? mEffectPublicationWorkers.get() : nullptr);
    }

    RenderCore::RenderFrameResult V4EngineRenderBridge::renderMainFrame(const V4MainFrameSource& source)
    {
        mLastDiagnostic.clear();
        if (!mRouteStatus->healthy())
        {
            mLastDiagnostic = mRouteStatus->firstDiagnostic();
            if (mLastDiagnostic.empty())
                mLastDiagnostic = "V4 scene publication route is unhealthy";
            return RenderCore::RenderFrameResult::Failed;
        }
        if (!mLifecycleTaken)
        {
            mLastDiagnostic = "scene lifecycle must be attached before rendering a V4 frame";
            return RenderCore::RenderFrameResult::Failed;
        }

        const std::optional<RenderCore::Extent2D> extent = outputExtent();
        if (!extent)
        {
            mLastDiagnostic = "V4 output extent is temporarily unavailable";
            return RenderCore::RenderFrameResult::Skipped;
        }
        RenderCore::SingleViewFrameInput input;
        input.camera = source.camera;
        input.renderExtent = *extent;
        input.outputExtent = *extent;
        input.environment = source.environment;
        input.simulationTime = source.simulationTime;
        input.frameDelta = source.frameDelta;
        input.lodScale = source.lodScale;
        input.dynamicTransforms = source.dynamicTransforms;
        input.skeletonPoses = source.skeletonPoses;
        input.morphWeights = source.morphWeights;
        publishImmediateEffects(input, source);
        input.invalidateHistory = source.invalidateHistory || mGuiOnlyFramePresented;
        const RenderCore::RenderFrameResult result = mSession->renderFrame(input);
        if (result == RenderCore::RenderFrameResult::Presented)
            mGuiOnlyFramePresented = false;
        mLastDiagnostic = mSession->lastDiagnostic();
        return result;
    }

    RenderCore::RenderFrameResult V4EngineRenderBridge::renderGuiFrame(double simulationTime, double frameDelta)
    {
        mLastDiagnostic.clear();
        const std::optional<RenderCore::Extent2D> extent = outputExtent();
        if (!extent)
            return RenderCore::RenderFrameResult::Skipped;

        RenderCore::SingleViewFrameInput input;
        input.renderExtent = *extent;
        input.outputExtent = *extent;
        input.simulationTime = simulationTime;
        input.frameDelta = frameDelta;
        input.invalidateHistory = true;
        input.environment.skyEnabled = false;
        input.environment.sunLightEnabled = false;
        input.environment.sunVisible = false;
        const RenderCore::RenderFrameResult result = std::getenv("OPENMW_V4_GUI_WORLD_CONTROL")
            ? mSession->renderFrame(input) : mSession->renderGuiFrame(input);
        if (result == RenderCore::RenderFrameResult::Presented)
            mGuiOnlyFramePresented = true;
        mLastDiagnostic = mSession->lastDiagnostic();
        return result;
    }

    void V4EngineRenderBridge::waitIdle()
    {
        if (mSession)
            mSession->waitIdle();
    }
}
