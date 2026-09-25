#ifndef OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H
#define OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H

#include "v4engineframesource.hpp"
#include <components/nifrender/textureidentitycache.hpp>
#include "v4renderroutestatus.hpp"

#include "../mwworld/scenerenderlifecycle.hpp"

#include <components/render/backend/vsg/vsgruntimebootstrap.hpp>
#include <components/render/native/nifassetservice.hpp>
#include <components/render/native/staticworldservice.hpp>
#include <components/render/backend/vsg/vsgsemanticsession.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <components/rendercore/renderer.hpp>
#include <components/rendercore/terrainchunkproducer.hpp>
#include <components/rendercore/terrainpreparationservice.hpp>
#include <components/rendercore/terrainresidencyplanner.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace VFS
{
    class Manager;
}

namespace MyGUIPlatform
{
    class PlatformBase;
}

struct SDL_Window;

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    class V4SkyCapture;
    class V4ObjectCapturePlans;
    class RenderingManager;
    // Build-gated application bridge for the distinct VSG route. It creates the
    // session directly from OpenMW's winning VFS, hands the world an observer
    // before its first cell activation, and publishes main-camera frames without
    // consulting the OSG scene graph or OSG graphics-window state.
    class V4EngineRenderBridge final
    {
    public:
        // Out-of-line probe used by the guarded production executable target.
        // Referencing it forces the bridge/session/backend archive chain through
        // the final link even while renderer selection remains unadvertised.
        [[nodiscard]] static bool linkedRuntimeAvailable() noexcept;

        [[nodiscard]] static std::unique_ptr<V4EngineRenderBridge> create(
            const VFS::Manager& vfs, RenderVsg::VsgRuntimeBootstrapOptions options = {});
        [[nodiscard]] static std::unique_ptr<V4EngineRenderBridge> createConfigured(const VFS::Manager& vfs);

        ~V4EngineRenderBridge();
        V4EngineRenderBridge(const V4EngineRenderBridge&) = delete;
        V4EngineRenderBridge& operator=(const V4EngineRenderBridge&) = delete;

        // Exactly one scene owns the lifecycle observer. The observer shares
        // session ownership so teardown remains GPU-safe even if application
        // members are later reordered; the registered VFS must still outlive it.
        [[nodiscard]] std::unique_ptr<MWWorld::SceneRenderLifecycle> takeSceneRenderLifecycle();
        [[nodiscard]] bool sceneRenderLifecycleTaken() const noexcept { return mLifecycleTaken; }

        [[nodiscard]] bool configureNamedSwitchState(
            RenderCore::NightDaySwitchState state, bool dayNightSwitchesEnabled) noexcept
        {
            return mSession && mSession->configureNamedSwitchState(state, dayNightSwitchesEnabled);
        }

        [[nodiscard]] std::optional<RenderCore::Extent2D> outputExtent() const noexcept;
        [[nodiscard]] SDL_Window* sdlWindow() const noexcept;
        [[nodiscard]] std::unique_ptr<MyGUIPlatform::PlatformBase> createGuiPlatform(
            const std::filesystem::path& logName = {});
        [[nodiscard]] bool captureDynamicFrameState(const RenderingManager& rendering, V4MainFrameSource& source);
        [[nodiscard]] bool prepareNativeLocalMapFrame();
        bool prepareNativeSkyFrame(const RenderingManager& rendering, V4MainFrameSource& frame);
        [[nodiscard]] bool prepareNativePreviewFrame(V4MainFrameSource& source);
        void nativePreviewFramePresented(const V4MainFrameSource& source);
        [[nodiscard]] bool prepareGuiFrame();
        [[nodiscard]] bool synchronizeProjectiles(V4MainFrameSource& source);
        [[nodiscard]] bool synchronizeExteriorTerrain(const RenderingManager& rendering, const MWWorld::Cell& cell);
        RenderCore::RenderFrameResult renderMainFrame(const V4MainFrameSource& source);
        // Production CP4F gameplay path. It preserves the established main
        // frame semantics while adding backend-neutral LocalMap auxiliary views
        // and native VSG/MyGUI publication/retirement around the same session.
        RenderCore::RenderFrameResult renderMainFrameWithNativeLocalMap(const V4MainFrameSource& source);
        RenderCore::RenderFrameResult renderGuiFrame(double simulationTime, double frameDelta);
        void stopBackgroundPreparation();
        void waitIdle();

        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        V4EngineRenderBridge(const VFS::Manager& vfs, std::shared_ptr<RenderVsg::VsgSemanticSession> session);
        void publishImmediateEffects(RenderCore::SingleViewFrameInput& input, const V4MainFrameSource& source);
        [[nodiscard]] bool synchronizeGroundcover(const RenderingManager& rendering,
            std::string_view worldspaceIdentity, std::span<const RenderCore::TerrainResidencyCell> residency);

        const VFS::Manager& mVfs;
        NifRender::TextureIdentityCache mTextureIdentities;
        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        std::unique_ptr<RenderNative::NifAssetService> mNativeAssets;
        std::unique_ptr<RenderNative::StaticWorldService> mNativeStaticWorld;
        std::shared_ptr<V4RenderRouteStatus> mRouteStatus;
        std::unique_ptr<RenderNative::TerrainWorldService> mNativeTerrain;
        std::string mLastDiagnostic;
        bool mLifecycleTaken = false;
        bool mGuiOnlyFramePresented = false;
        std::unique_ptr<RenderCore::BoundedParallelFor> mEffectPublicationWorkers;
        std::unique_ptr<V4ObjectCapturePlans> mObjectCapturePlans;
        RenderCore::PersistentDrawWorld mPersistentDraws;
        unsigned int mPoseTraversal = 0;
        struct ComposedActorEntry
        {
            RenderCore::ModelHandle model;
            std::string signature;
        };
        std::map<std::string, bool, std::less<>> mEvaluatedObjectPlayback;
        RenderCore::WorldEpoch mEvaluatedObjectPlaybackEpoch;
        std::map<std::string, RenderCore::SkeletonHandle, std::less<>> mForcedActorSkeletons;
        std::map<std::string, ComposedActorEntry, std::less<>> mComposedActors;
        RenderCore::WorldEpoch mComposedActorEpoch;
        std::set<std::string, std::less<>> mActorLights;
        RenderCore::WorldEpoch mActorLightEpoch;
        std::set<std::string, std::less<>> mProjectileInstances;
        std::set<std::string, std::less<>> mProjectileLights;
        RenderCore::WorldEpoch mProjectileEpoch;
        std::set<std::string, std::less<>> mGroundcoverCells;
        RenderCore::WorldEpoch mGroundcoverEpoch;

        struct NativePreviewEntry
        {
            std::string textureName;
            std::uint64_t renderedRevision = 0;
            bool published = false;
        };
        std::map<std::uint32_t, NativePreviewEntry> mNativePreviewEntries;
        std::shared_ptr<V4SkyCapture> mNativeSkyCapture;
        bool mReportedSkyOcclusionDeferral = false;

        struct NativeMapUiEntry
        {
            std::string logicalIdentity;
            std::string mapTextureName;
            std::string fogTextureName;
            bool mapPublished = false;
            std::uint64_t fogRevision = 0;
        };
        std::map<std::uint32_t, NativeMapUiEntry> mNativeMapUiEntries;
        struct NativeMapPreparedFrame;
        // The concrete type lives beside the LocalMap bridge so this header does
        // not expose LocalMap or VSG image details. A shared pointer permits the
        // out-of-line bridge destructor to retain that incomplete type safely.
        std::shared_ptr<NativeMapPreparedFrame> mPreparedNativeMapFrame;
    };
}

#endif
