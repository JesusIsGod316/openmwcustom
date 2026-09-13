#include "v4enginerenderbridge.hpp"

#include "v4runtimeoptions.hpp"
#include "v4effectcapture.hpp"
#include "v4scenerenderlifecycle.hpp"
#include "v4semanticsource.hpp"
#include "v4terrainsource.hpp"

#include "animation.hpp"
#include "groundcover.hpp"
#include "npcanimation.hpp"
#include "renderingmanager.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/inventorystore.hpp"

#include <components/nif/niffile.hpp>
#include <components/nifrender/actormodelcomposer.hpp>
#include <components/nifrender/enchantedglow.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/sceneutil/morphgeometry.hpp>
#include <components/sceneutil/skeleton.hpp>

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
#include <components/render/backend/vsg/vsgsemanticsession.hpp>

#include <SDL3/SDL.h>

#include <cmath>
#include <cstddef>
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
            RenderVsg::VsgSemanticSession& session, const VFS::Manager& vfs, VFS::Path::NormalizedView path)
        {
            if (path.empty())
                return std::nullopt;
            if (const std::optional<RenderCore::ModelHandle> model = session.models().find(path.value()))
            {
                return NifRender::StaticModelCacheResult{ NifRender::StaticModelCacheStatus::Reused,
                    NifRender::TranslationPublishStatus::Applied, *model, session.models().findSkeleton(path.value()) };
            }
            const VFS::Path::Normalized normalized(path);
            if (!vfs.exists(normalized))
                return std::nullopt;
            Nif::NIFFile nifFile(normalized);
            Nif::Reader reader(nifFile, nullptr);
            reader.parse(vfs.get(normalized));
            const NifRender::TranslationBundle bundle = NifRender::translateStaticNif(Nif::FileView(nifFile), vfs);
            const NifRender::StaticModelCacheResult published = session.models().publish(bundle);
            return published.available() ? std::optional<NifRender::StaticModelCacheResult>(published) : std::nullopt;
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

            void apply(osg::Geode& geode) override
            {
                for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
                {
                    if (auto* morph = dynamic_cast<SceneUtil::MorphGeometry*>(geode.getDrawable(i)))
                        morphs.push_back(morph);
                }
                traverse(geode);
            }

            std::vector<SceneUtil::MorphGeometry*> morphs;
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
        , mSession(std::move(session))
        , mRouteStatus(std::make_shared<V4RenderRouteStatus>())
        , mTerrain(std::make_unique<RenderCore::TerrainChunkProducer>(mSession->world(), mSession->publisher()))
    {
    }

    bool V4EngineRenderBridge::synchronizeExteriorTerrain(const RenderingManager& rendering, const MWWorld::Cell& cell)
    {
        mLastDiagnostic.clear();
        if (!cell.isExterior())
        {
            if (!synchronizeGroundcover(rendering, {}, {}))
                return false;
            mTerrainResidencyPlanner.reset();
            mPendingTerrainPublication.clear();
            if (mTerrainPreparation)
            {
                static_cast<void>(
                    mTerrainPreparation->request(std::span<const RenderCore::TerrainPreparationRequest>{}));
                static_cast<void>(mTerrainPreparation->takeReady());
            }
            const RenderCore::TerrainChunkPublishStatus status = mTerrain->synchronize(std::nullopt);
            return status == RenderCore::TerrainChunkPublishStatus::Applied
                || status == RenderCore::TerrainChunkPublishStatus::AlreadyPresent;
        }

        TerrainStorage* const storage = rendering.getTerrainStorage();
        if (!storage)
        {
            mLastDiagnostic = "authoritative exterior terrain storage is unavailable";
            return false;
        }

        const RenderCore::TerrainPreparationRequest current
            = makeV4TerrainChunkRequest(cell, cell.getGridX(), cell.getGridY(), true);
        if (!mTerrain->contains(current.identity))
        {
            const std::optional<RenderCore::TerrainChunkSource> source = makeV4TerrainChunkSource(*storage, current);
            if (!source)
            {
                mLastDiagnostic = "authoritative exterior LAND data could not produce the required terrain chunk";
                return false;
            }
            const RenderCore::TerrainChunkPublishStatus status = mTerrain->synchronize(source);
            if (status != RenderCore::TerrainChunkPublishStatus::Applied
                && status != RenderCore::TerrainChunkPublishStatus::AlreadyPresent)
            {
                mLastDiagnostic = "required terrain chunk publication failed with status "
                    + std::to_string(static_cast<unsigned int>(status));
                return false;
            }
        }

        if (!mTerrainPreparation)
        {
            mTerrainPreparation = std::make_unique<RenderCore::TerrainPreparationService>(
                [storage](const RenderCore::TerrainPreparationRequest& request, std::stop_token stop) {
                    if (stop.stop_requested())
                        return std::optional<RenderCore::TerrainChunkSource>{};
                    return makeV4TerrainChunkSource(*storage, request);
                });
        }

        const std::vector<RenderCore::TerrainResidencyCell> residency
            = mTerrainResidencyPlanner.update(current.worldspaceIdentity, current.gridX, current.gridY);
        if (!synchronizeGroundcover(rendering, current.worldspaceIdentity, residency))
            return false;
        std::vector<RenderCore::TerrainPreparationRequest> desired;
        desired.reserve(residency.size());
        for (const RenderCore::TerrainResidencyCell& resident : residency)
            desired.push_back(makeV4TerrainChunkRequest(
                cell, resident.gridX, resident.gridY, resident.required, resident.lodLevel, resident.stitchMask));
        const RenderCore::TerrainPreparationRequestStatus requested
            = mTerrainPreparation->request(std::span<const RenderCore::TerrainPreparationRequest>(desired));
        if (requested == RenderCore::TerrainPreparationRequestStatus::Invalid
            || requested == RenderCore::TerrainPreparationRequestStatus::GenerationExhausted)
        {
            mLastDiagnostic = "terrain preparation rejected the desired resident cell set";
            return false;
        }
        if (requested == RenderCore::TerrainPreparationRequestStatus::Accepted)
            mPendingTerrainPublication.clear();

        if (std::optional<RenderCore::PreparedTerrainSet> ready = mTerrainPreparation->takeReady())
        {
            if (!ready->requiredChunksReady)
            {
                mLastDiagnostic = "background terrain preparation failed for the required current cell";
                return false;
            }
            mPendingTerrainPublication = std::move(ready->chunks);
        }
        if (!mPendingTerrainPublication.empty())
        {
            constexpr RenderCore::TerrainPublicationLimits limits{
                .maxNewChunks = 4,
                .maxNewMeshBytes = 8u * 1024u * 1024u,
            };
            const RenderCore::TerrainChunkPublishStatus status = mTerrain->synchronize(
                std::span<const RenderCore::TerrainChunkSource>(mPendingTerrainPublication), limits);
            if (status != RenderCore::TerrainChunkPublishStatus::Applied
                && status != RenderCore::TerrainChunkPublishStatus::PartiallyApplied
                && status != RenderCore::TerrainChunkPublishStatus::AlreadyPresent)
            {
                mLastDiagnostic = "prepared terrain set publication failed with status "
                    + std::to_string(static_cast<unsigned int>(status));
                return false;
            }
            if (status != RenderCore::TerrainChunkPublishStatus::PartiallyApplied)
                mPendingTerrainPublication.clear();
        }
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
        const Groundcover* groundcover = rendering.getGroundcover();
        if (groundcover && !worldspaceIdentity.empty())
        {
            for (const RenderCore::TerrainResidencyCell& resident : residency)
            {
                if (resident.lodLevel == 0)
                    desired.insert("groundcover:" + std::string(worldspaceIdentity) + ":"
                        + std::to_string(resident.gridX) + "," + std::to_string(resident.gridY));
            }

            // File merge/density filtering is deterministic but currently
            // synchronous. Bound new work per frame until it moves onto the
            // CP4 preparation service, preventing a nine-cell entry spike.
            constexpr std::size_t maxNewCellsPerFrame = 1;
            std::size_t newCells = 0;
            for (const RenderCore::TerrainResidencyCell& resident : residency)
            {
                if (resident.lodLevel != 0)
                    continue;
                const std::string cellIdentity = "groundcover:" + std::string(worldspaceIdentity) + ":"
                    + std::to_string(resident.gridX) + "," + std::to_string(resident.gridY);
                if (mGroundcoverCells.contains(cellIdentity))
                    continue;
                if (newCells >= maxNewCellsPerFrame)
                    continue;

                RenderCore::StaticPopulationCellSource cellSource;
                cellSource.identity = cellIdentity;
                cellSource.worldspaceIdentity = worldspaceIdentity;
                cellSource.gridX = resident.gridX;
                cellSource.gridY = resident.gridY;
                cellSource.groundcover = true;
                const RenderCore::StaticPopulationPublishStatus added
                    = mSession->populations().addCell(std::move(cellSource));
                if (added != RenderCore::StaticPopulationPublishStatus::Applied
                    && added != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
                {
                    mLastDiagnostic = "groundcover population cell staging failed";
                    return false;
                }

                const osg::Vec2f center(
                    static_cast<float>(resident.gridX) + 0.5f, static_cast<float>(resident.gridY) + 0.5f);
                Groundcover::InstanceMap instances = groundcover->collectInstances(1.0f, center);
                for (const auto& [modelPath, entries] : instances)
                {
                    const std::optional<NifRender::StaticModelCacheResult> published
                        = ensureModelPublished(*mSession, mVfs, modelPath);
                    if (!published)
                    {
                        mLastDiagnostic = "groundcover model publication failed for " + modelPath.value();
                        return false;
                    }
                    const RenderCore::ModelRecord* model = mSession->world().get(published->model);
                    if (!model)
                    {
                        mLastDiagnostic = "groundcover model cache returned a stale handle";
                        return false;
                    }
                    for (const Groundcover::GroundcoverEntry& entry : entries)
                    {
                        RenderCore::StaticPopulationInstanceSource source;
                        source.identity = "groundcover:" + entry.mRefNum.toString();
                        source.cellIdentity = cellIdentity;
                        source.model = published->model;
                        source.transform.translation
                            = { entry.mPos.pos[0], entry.mPos.pos[1], entry.mPos.pos[2] };
                        const osg::Quat rotation = Misc::Convert::makeOsgQuat(entry.mPos);
                        source.transform.rotation = { static_cast<float>(rotation.w()), static_cast<float>(rotation.x()),
                            static_cast<float>(rotation.y()), static_cast<float>(rotation.z()) };
                        source.transform.scale = { entry.mScale, entry.mScale, entry.mScale };
                        source.localBounds = model->bounds;
                        source.lod.maximumDistance = std::max(0.0f, Settings::groundcover().mRenderingDistance.get());
                        source.semanticFlags &= ~RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster);
                        const RenderCore::StaticPopulationPublishStatus status
                            = mSession->populations().upsert(std::move(source));
                        if (status != RenderCore::StaticPopulationPublishStatus::Applied
                            && status != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
                        {
                            mLastDiagnostic = "groundcover placement staging failed";
                            return false;
                        }
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
            const RenderCore::StaticPopulationPublishStatus status = mSession->populations().removeCell(*current);
            if (status != RenderCore::StaticPopulationPublishStatus::Applied
                && status != RenderCore::StaticPopulationPublishStatus::AlreadyPresent)
            {
                mLastDiagnostic = "groundcover population retirement failed";
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
        mTerrainPreparation.reset();
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
        mLastDiagnostic.clear();
        if (mComposedActorEpoch != mSession->world().epoch())
        {
            mComposedActors.clear();
            mComposedActorEpoch = mSession->world().epoch();
        }
        if (mActorLightEpoch != mSession->world().epoch())
        {
            mActorLights.clear();
            mActorLightEpoch = mSession->world().epoch();
        }
        std::set<std::string, std::less<>> currentActorLights;
        bool compatible = true;
        if (++mPoseTraversal == 0u)
            ++mPoseTraversal;

        rendering.forEachAnimation([&](Animation& animation) {
            if (!compatible)
                return;
            const MWWorld::Ptr ptr = animation.getPtr();
            if (ptr.isEmpty() || !ptr.getRefData().isEnabled())
                return;

            if (!ptr.getClass().isActor())
            {
                const VFS::Path::Normalized modelPath = ptr.getClass().getCorrectedModel(ptr);
                if (modelPath.empty() || Misc::ResourceHelpers::isHiddenMarker(ptr.getCellRef().getRefId()))
                    return;

                bool needsEvaluatedCapture = ptr.getClass().useAnim();
                if (!needsEvaluatedCapture)
                {
                    const std::optional<NifRender::StaticModelCacheResult> published
                        = ensureModelPublished(*mSession, mVfs, modelPath);
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
                }
                if (!needsEvaluatedCapture)
                    return;

                const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
                if (!identity)
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated non-actor object has no stable content identity";
                    return;
                }
                osg::Group* const evaluatedRoot = animation.getV4EffectRoot();
                if (!evaluatedRoot)
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated non-actor object has no authoritative OpenMW animation root: "
                        + *identity;
                    return;
                }

                // Whole-object compatibility capture deliberately follows only
                // active children. OpenMW's evaluated Switch/node-mask state is
                // observable animation behavior; traversing all children would
                // resurrect hidden controller branches and mod-authored states.
                v4_effect_detail::CaptureVisitor visitor("animated-object:" + *identity, true, mVfs);
                visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN);
                evaluatedRoot->accept(visitor);
                V4EffectCaptureResult captured = visitor.take();
                if (!captured.valid())
                {
                    compatible = false;
                    mLastDiagnostic = captured.diagnostic.empty()
                        ? "evaluated non-actor object could not produce native Vulkan compatibility draws: " + *identity
                        : "evaluated non-actor object " + *identity + ": " + captured.diagnostic;
                    return;
                }

                constexpr std::uint64_t worldObjectFlags
                    = RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::OrdinaryWorld)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ReflectionEligible)
                    | RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::RefractionEligible);
                for (RenderCore::ImmediateEffectDraw& draw : captured.draws)
                {
                    draw.semanticFlags = worldObjectFlags;
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
                    = captureV4AttachedEffects(*effectRoot, "actor-effect:" + *identity, mVfs);
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
                = ensureModelPublished(*mSession, mVfs, animation.getV4SourceModel());
            if (!base || !base->skeleton)
            {
                compatible = false;
                mLastDiagnostic = "active actor source model has no publishable canonical skeleton";
                return;
            }
            RenderCore::ModelHandle actorModel = base->model;
            if (auto* npc = dynamic_cast<NpcAnimation*>(&animation))
            {
                std::vector<NifRender::ActorPartModelSource> parts;
                std::string signature(animation.getV4SourceModel().value());
                for (const NpcAnimation::V4PartSource& part : npc->getV4PartSources())
                {
                    const std::optional<NifRender::StaticModelCacheResult> published
                        = ensureModelPublished(*mSession, mVfs, part.model);
                    if (!published)
                    {
                        compatible = false;
                        mLastDiagnostic = "NPC part is missing from the winning VFS or failed translation";
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
                        const RenderCore::Color color{
                            sourceColor.r(), sourceColor.g(), sourceColor.b(), sourceColor.a() };
                        const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(
                            mSession->world(), mSession->publisher(), mVfs, partModel, color,
                            Settings::shaders().mApplyLightingToEnvironmentMaps);
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

                    parts.push_back({ partModel, part.boneName, part.visible });
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
                        mLastDiagnostic = "NPC attached ammunition cannot resolve its authoritative item or attachment bone";
                        return;
                    }

                    const VFS::Path::Normalized ammoModel = ammo->getClass().getCorrectedModel(*ammo);
                    const std::optional<NifRender::StaticModelCacheResult> publishedAmmo
                        = ensureModelPublished(*mSession, mVfs, ammoModel);
                    if (!publishedAmmo)
                    {
                        compatible = false;
                        mLastDiagnostic = "NPC attached ammunition is missing from the winning VFS or failed translation";
                        return;
                    }

                    RenderCore::ModelHandle ammoModelHandle = publishedAmmo->model;
                    std::string ammoGlowSignature;
                    if (!ammo->getClass().getEnchantment(*ammo).empty())
                    {
                        const osg::Vec4f sourceColor = ammo->getClass().getEnchantmentColor(*ammo);
                        const RenderCore::Color color{
                            sourceColor.r(), sourceColor.g(), sourceColor.b(), sourceColor.a() };
                        const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(
                            mSession->world(), mSession->publisher(), mVfs, ammoModelHandle, color,
                            Settings::shaders().mApplyLightingToEnvironmentMaps);
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
                    parts.push_back({ ammoModelHandle, arrowBone->getName(), ammoVisible });
                    signature += "\nammunition:" + std::string(ammoModel.value()) + ":" + arrowBone->getName() + ":"
                        + (ammoVisible ? "1" : "0") + ammoGlowSignature;
                }

                auto entry = mComposedActors.find(*identity);
                if (entry == mComposedActors.end() || entry->second.signature != signature
                    || !mSession->world().get(entry->second.model))
                {
                    NifRender::ComposedActorModel composed = NifRender::composeActorModel(
                        mSession->world(), base->model, *base->skeleton, parts, "runtime:npc:" + *identity);
                    if (!composed.valid())
                    {
                        compatible = false;
                        mLastDiagnostic = composed.diagnostic;
                        return;
                    }

                    RenderCore::ModelHandle composedHandle;
                    RenderCore::RenderWorldUpdateBatch batch(
                        mSession->world().epoch(), mSession->publisher().nextSequence(), "runtime:npc:" + *identity);
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
                const RenderCore::Color color{
                    sourceColor->r(), sourceColor->g(), sourceColor->b(), sourceColor->a() };
                const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(
                    mSession->world(), mSession->publisher(), mVfs, actorModel, color,
                    Settings::shaders().mApplyLightingToEnvironmentMaps, true);
                if (!glow.available())
                {
                    compatible = false;
                    mLastDiagnostic = "actor spell-cast glow publication failed: " + enchantedGlowDiagnostic(glow.status);
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
            if (!bound || bound->model != actorModel || bound->skeleton != base->skeleton)
            {
                const RenderCore::ModelRecord* modelRecord = mSession->world().get(actorModel);
                const std::optional<RenderCore::DynamicInstanceSource> dynamic = modelRecord
                    ? makeV4DynamicInstanceSource(ptr, actorModel, *base->skeleton, modelRecord->bounds)
                    : std::nullopt;
                if (!dynamic)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor could not produce a dynamic instance source";
                    return;
                }
                const RenderCore::ActiveCellPublishResult published = mSession->cells().upsertDynamicInstance(*dynamic);
                if (published.status != RenderCore::ActiveCellPublishStatus::Applied
                    && published.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)
                {
                    compatible = false;
                    mLastDiagnostic = "active actor instance publication failed";
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

            const RenderCore::ModelRecord* model = instance->model ? mSession->world().get(*instance->model) : nullptr;
            if (!model || !model->payload)
            {
                compatible = false;
                mLastDiagnostic = "active actor has no current V4 model payload";
                return;
            }
            if (model->dynamicRequirements != 0)
            {
                compatible = false;
                mLastDiagnostic = modelDynamicRequirementDiagnostic(model->dynamicRequirements);
                return;
            }
            std::vector<std::pair<RenderCore::ModelNodeIndex, const RenderCore::MeshRecord*>> morphNodes;
            for (std::size_t nodeIndex = 0; nodeIndex < model->payload->nodes.size(); ++nodeIndex)
            {
                const RenderCore::ModelNodeRecord& node = model->payload->nodes[nodeIndex];
                const RenderCore::MeshRecord* mesh = node.mesh ? mSession->world().get(*node.mesh) : nullptr;
                if (mesh && mesh->morphed)
                    morphNodes.emplace_back(RenderCore::ModelNodeIndex{ static_cast<std::uint32_t>(nodeIndex) }, mesh);
            }

            SceneUtil::Skeleton* evaluated = animation.getSkeleton();
            if (!evaluated)
            {
                compatible = false;
                mLastDiagnostic = "active actor has no evaluated OpenMW skeleton";
                return;
            }
            std::vector<SceneUtil::Bone*> evaluatedBones;
            evaluatedBones.reserve(skeleton->payload->bones.size());
            for (const RenderCore::BoneRecord& bone : skeleton->payload->bones)
            {
                SceneUtil::Bone* sourceBone = evaluated->getBone(bone.name);
                if (!sourceBone)
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated actor skeleton is missing required bone " + bone.name;
                    return;
                }
                evaluatedBones.push_back(sourceBone);
            }
            if (!compatible)
                return;
            evaluated->updateBoneMatrices(mPoseTraversal);
            std::vector<glm::mat4> global;
            global.reserve(evaluatedBones.size());
            for (const SceneUtil::Bone* bone : evaluatedBones)
                global.push_back(toGlm(bone->mMatrixInSkeletonSpace));

            RenderCore::SkeletonPoseInput pose;
            pose.instance = *handle;
            pose.skeleton = *instance->skeleton;
            pose.localTransforms.resize(global.size());
            for (std::size_t i = 0; i < global.size(); ++i)
            {
                const std::int32_t parent = skeleton->payload->bones[i].parent;
                pose.localTransforms[i]
                    = parent < 0 ? global[i] : glm::inverse(global[static_cast<std::size_t>(parent)]) * global[i];
                if (!finite(pose.localTransforms[i]))
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated actor pose contains a non-finite local transform";
                    return;
                }
            }
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
                animation.getObjectRoot()->accept(collector);
                if (collector.morphs.size() != morphNodes.size())
                {
                    compatible = false;
                    mLastDiagnostic = "evaluated actor morph geometry does not match its translated model";
                    return;
                }
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
                    const std::string foldedName = Misc::StringUtils::lowerCase(node.name);
                    const auto named = evaluatedByName.find(foldedName);
                    const std::size_t occurrence = nameCursor[foldedName]++;
                    if (node.name.empty() || named == evaluatedByName.end() || occurrence >= named->second.size())
                    {
                        compatible = false;
                        mLastDiagnostic = "evaluated actor morph nodes do not match translated node " + node.name;
                        return;
                    }
                    const SceneUtil::MorphGeometry& evaluatedMorph = *named->second[occurrence];
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
                        if (target.sourceIndex >= evaluatedMorph.getMorphTargetList().size())
                        {
                            compatible = false;
                            mLastDiagnostic = "evaluated actor morph target count is incompatible with translated data";
                            return;
                        }
                        weights.weights.push_back(evaluatedMorph.getMorphTarget(target.sourceIndex).getWeight());
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
        return compatible;
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
        input.immediateEffectDraws = source.immediateEffectDraws;
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
        const RenderCore::RenderFrameResult result = mSession->renderFrame(input);
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
