#include "v4engineframecoordinator.hpp"

#include "renderingmanager.hpp"
#include "v4enginerenderbridge.hpp"
#include "v4semanticsource.hpp"

#include "../mwbase/environment.hpp"
#include "../mwbase/world.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/projectilemanager.hpp"

#include <components/nif/niffile.hpp>
#include <components/nifrender/enchantedglow.hpp>
#include <components/nifrender/niftranslator.hpp>
#include <components/rendercore/namedvisualsemantics.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/manager.hpp>

#include <optional>
#include <set>
#include <string>
#include <utility>

namespace MWRender
{
    bool V4EngineRenderBridge::synchronizeProjectiles()
    {
        mLastDiagnostic.clear();
        if (mProjectileEpoch != mSession->world().epoch())
        {
            mProjectileInstances.clear();
            mProjectileEpoch = mSession->world().epoch();
        }

        const MWWorld::V4ProjectileFrameSnapshot frame = MWWorld::captureV4ProjectileFrameState();
        std::set<std::string, std::less<>> currentProjectiles;

        std::optional<RenderCore::ActiveCellSource> activeCell;
        if (!frame.physicalProjectiles.empty())
        {
            MWBase::World* const world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world ? world->getPlayerPtr() : MWWorld::Ptr();
            activeCell = !player.isEmpty() && player.getCell() ? makeV4ActiveCellSource(*player.getCell()) : std::nullopt;
            if (!activeCell)
            {
                mLastDiagnostic = "live physical projectiles have no authoritative active cell identity";
                return false;
            }
        }

        for (const MWWorld::V4PhysicalProjectileSnapshot& projectile : frame.physicalProjectiles)
        {
            try
            {
                MWWorld::ManualRef ref(*MWBase::Environment::get().getESMStore(), projectile.projectileId);
                MWWorld::Ptr ptr = ref.getPtr();
                const VFS::Path::Normalized modelPath = ptr.getClass().getCorrectedModel(ptr);
                if (modelPath.empty())
                {
                    mLastDiagnostic = "live physical projectile has no corrected model path";
                    return false;
                }

                std::optional<RenderCore::ModelHandle> model = mSession->models().find(modelPath.value());
                if (!model)
                {
                    if (!mVfs.exists(modelPath))
                    {
                        mLastDiagnostic = "physical projectile model is missing from the winning VFS";
                        return false;
                    }
                    Nif::NIFFile nifFile(modelPath);
                    Nif::Reader reader(nifFile, nullptr);
                    reader.parse(mVfs.get(modelPath));
                    const NifRender::TranslationBundle bundle
                        = NifRender::translateStaticNif(Nif::FileView(nifFile), mVfs);
                    const NifRender::StaticModelCacheResult published = mSession->models().publish(bundle);
                    if (!published.available())
                    {
                        mLastDiagnostic = "physical projectile model translation/publication failed";
                        return false;
                    }
                    model = published.model;
                }

                const RenderCore::ModelRecord* modelRecord = mSession->world().get(*model);
                if (!modelRecord)
                {
                    mLastDiagnostic = "physical projectile model cache returned a stale handle";
                    return false;
                }
                if (modelRecord->dynamicRequirements != 0)
                {
                    mLastDiagnostic = "physical projectile model requires unsupported dynamic NIF playback";
                    return false;
                }

                RenderCore::ModelHandle renderModel = *model;
                if (!ptr.getClass().getEnchantment(ptr).empty())
                {
                    const osg::Vec4f sourceColor = ptr.getClass().getEnchantmentColor(ptr);
                    const RenderCore::Color color{
                        sourceColor.r(), sourceColor.g(), sourceColor.b(), sourceColor.a() };
                    const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(
                        mSession->world(), mSession->publisher(), mVfs, renderModel, color,
                        Settings::shaders().mApplyLightingToEnvironmentMaps);
                    if (!glow.available())
                    {
                        mLastDiagnostic = "physical projectile enchanted-glow publication failed";
                        return false;
                    }
                    renderModel = glow.model;
                    modelRecord = mSession->world().get(renderModel);
                    if (!modelRecord)
                    {
                        mLastDiagnostic = "physical projectile enchanted-glow variant returned a stale model handle";
                        return false;
                    }
                }

                RenderCore::StaticInstanceSource source;
                source.identity = "projectile:" + std::to_string(projectile.runtimeId);
                source.cellIdentity = activeCell->identity;
                source.model = renderModel;
                source.transform.translation
                    = { projectile.position.x(), projectile.position.y(), projectile.position.z() };
                source.transform.rotation = { static_cast<float>(projectile.orientation.w()),
                    static_cast<float>(projectile.orientation.x()), static_cast<float>(projectile.orientation.y()),
                    static_cast<float>(projectile.orientation.z()) };
                source.transform.scale = { 1.0f, 1.0f, 1.0f };
                source.localBounds = modelRecord->bounds;
                const RenderCore::ActiveCellPublishResult published = mSession->cells().upsertStaticInstance(source);
                if (published.status != RenderCore::ActiveCellPublishStatus::Applied
                    && published.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)
                {
                    mLastDiagnostic = "physical projectile instance publication failed";
                    return false;
                }
                currentProjectiles.insert(source.identity);
            }
            catch (const std::exception& e)
            {
                mLastDiagnostic = "physical projectile publication failed: " + std::string(e.what());
                return false;
            }
        }

        for (const std::string& identity : mProjectileInstances)
        {
            if (currentProjectiles.contains(identity))
                continue;
            const RenderCore::ActiveCellPublishResult removed = mSession->cells().removeStaticInstance(identity);
            if (removed.status != RenderCore::ActiveCellPublishStatus::Applied
                && removed.status != RenderCore::ActiveCellPublishStatus::NotFound)
            {
                mLastDiagnostic = "stale physical projectile retirement failed";
                return false;
            }
        }
        mProjectileInstances = std::move(currentProjectiles);

        // Magic bolts use rotating NIF controllers, particle texture overrides,
        // dynamic lights, and sometimes multi-effect Dummy attachments. Until
        // that evaluated payload has a neutral V4 representation, failing here
        // is required; treating it as a static mesh would be silent visual loss.
        if (frame.liveMagicBoltCount != 0)
        {
            mLastDiagnostic = "live magic projectile requires particle/controller realization";
            return false;
        }

        return true;
    }

    RenderCore::RenderFrameResult V4EngineFrameCoordinator::render(const RenderingManager& rendering,
        const MWWorld::Cell& cell, double simulationTime, double frameDelta, bool invalidateHistory)
    {
        if (!mHealthy)
            return RenderCore::RenderFrameResult::Failed;

        const std::optional<RenderCore::Extent2D> extent = mBridge.outputExtent();
        if (!extent)
        {
            mLastDiagnostic = "V4 output surface is hidden, minimized, or temporarily has no pixel extent";
            return RenderCore::RenderFrameResult::Skipped;
        }

        MWBase::World* const world = MWBase::Environment::get().getWorld();
        if (!world)
            return fail("authoritative world state is unavailable for named visual switch capture");
        RenderCore::NightDaySwitchState nightDayState;
        switch (world->getNightDayMode())
        {
            case 0:
                nightDayState = RenderCore::NightDaySwitchState::Default;
                break;
            case 1:
                nightDayState = RenderCore::NightDaySwitchState::ExteriorNight;
                break;
            case 2:
                nightDayState = RenderCore::NightDaySwitchState::InteriorDay;
                break;
            default:
                return fail("authoritative weather state produced an invalid NightDaySwitch mode");
        }
        if (!mBridge.configureNamedSwitchState(nightDayState, Settings::game().mDayNightSwitches))
            return fail("V4 renderer rejected the authoritative NightDaySwitch state");

        if (!mBridge.synchronizeExteriorTerrain(rendering, cell))
            return fail(mBridge.lastDiagnostic().empty()
                    ? "authoritative terrain state could not produce a compatible V4 frame"
                    : mBridge.lastDiagnostic());

        if (!mBridge.synchronizeProjectiles())
            return fail(mBridge.lastDiagnostic().empty()
                    ? "authoritative projectile state could not produce a compatible V4 frame"
                    : mBridge.lastDiagnostic());

        std::optional<V4MainFrameSource> source = makeV4MainFrameSource(
            rendering, cell, rendering.isUnderwater(), *extent, simulationTime, frameDelta, invalidateHistory);
        if (!source)
            return fail("authoritative gameplay state could not produce a compatible V4 main frame");
        if (!mBridge.captureDynamicFrameState(rendering, *source))
            return fail(mBridge.lastDiagnostic().empty()
                    ? "authoritative actor state could not produce a compatible V4 frame"
                    : mBridge.lastDiagnostic());

        const RenderCore::RenderFrameResult result = mBridge.renderMainFrameWithNativeLocalMap(*source);
        mLastDiagnostic = mBridge.lastDiagnostic();
        if (result == RenderCore::RenderFrameResult::Failed)
            return fail(mLastDiagnostic.empty() ? "V4 render bridge rejected the main frame" : mLastDiagnostic);
        return result;
    }

    RenderCore::RenderFrameResult V4EngineFrameCoordinator::fail(std::string diagnostic)
    {
        if (mHealthy)
        {
            mHealthy = false;
            mLastDiagnostic = std::move(diagnostic);
        }
        return RenderCore::RenderFrameResult::Failed;
    }
}
