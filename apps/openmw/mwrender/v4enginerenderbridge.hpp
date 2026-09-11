#ifndef OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H
#define OPENMW_MWRENDER_V4ENGINERENDERBRIDGE_H

#include "v4engineframesource.hpp"
#include "v4renderroutestatus.hpp"

#include "../mwworld/scenerenderlifecycle.hpp"

#include <components/render/backend/vsg/vsgruntimebootstrap.hpp>
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

namespace RenderVsg
{
    class VsgSemanticSession;
}

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
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

        [[nodiscard]] std::optional<RenderCore::Extent2D> outputExtent() const noexcept;
        [[nodiscard]] SDL_Window* sdlWindow() const noexcept;
        [[nodiscard]] std::unique_ptr<MyGUIPlatform::PlatformBase> createGuiPlatform(
            const std::filesystem::path& logName = {});
        [[nodiscard]] bool captureDynamicFrameState(const RenderingManager& rendering, V4MainFrameSource& source);
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
        [[nodiscard]] bool synchronizeGroundcover(const RenderingManager& rendering,
            std::string_view worldspaceIdentity, std::span<const RenderCore::TerrainResidencyCell> residency);

        const VFS::Manager& mVfs;
        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        std::shared_ptr<V4RenderRouteStatus> mRouteStatus;
        std::unique_ptr<RenderCore::TerrainChunkProducer> mTerrain;
        std::unique_ptr<RenderCore::TerrainPreparationService> mTerrainPreparation;
        RenderCore::TerrainResidencyPlanner mTerrainResidencyPlanner;
        std::vector<RenderCore::TerrainChunkSource> mPendingTerrainPublication;
        std::string mLastDiagnostic;
        bool mLifecycleTaken = false;
        bool mGuiOnlyFramePresented = false;
        unsigned int mPoseTraversal = 0;
        struct ComposedActorEntry
        {
            RenderCore::ModelHandle model;
            std::string signature;
        };
        std::map<std::string, ComposedActorEntry, std::less<>> mComposedActors;
        RenderCore::WorldEpoch mComposedActorEpoch;
        std::set<std::string, std::less<>> mGroundcoverCells;
        RenderCore::WorldEpoch mGroundcoverEpoch;

        struct NativeMapUiEntry
        {
            std::string logicalIdentity;
            std::string mapTextureName;
            std::string fogTextureName;
            bool mapPublished = false;
            std::uint64_t fogRevision = 0;
        };
        std::map<std::uint32_t, NativeMapUiEntry> mNativeMapUiEntries;
    };
}

#endif
