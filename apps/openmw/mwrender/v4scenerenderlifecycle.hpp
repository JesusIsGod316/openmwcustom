#ifndef OPENMW_MWRENDER_V4SCENERENDERLIFECYCLE_H
#define OPENMW_MWRENDER_V4SCENERENDERLIFECYCLE_H

#include "v4renderroutestatus.hpp"

#include "../mwworld/scenerenderlifecycle.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace RenderVsg
{
    class VsgSemanticSession;
}

namespace VFS
{
    class Manager;
}

namespace MWRender
{
    // Production game-world adapter for the build-gated Vulkan session. Shared
    // session ownership keeps GPU state alive until the scene has retired its
    // observer. Static assets are parsed from the winning OpenMW VFS entry and
    // published through the one CP3B translator/cache path. Unsupported dynamic
    // categories remain outside this adapter and keep the compatibility gate shut.
    class V4SceneRenderLifecycle final : public MWWorld::SceneRenderLifecycle
    {
    public:
        V4SceneRenderLifecycle(std::shared_ptr<RenderVsg::VsgSemanticSession> session, const VFS::Manager& vfs,
            std::shared_ptr<V4RenderRouteStatus> routeStatus = std::make_shared<V4RenderRouteStatus>());

        void cellActivated(const MWWorld::CellStore& cell) override;
        void cellDeactivating(const MWWorld::CellStore& cell) noexcept override;
        void objectAdded(const MWWorld::Ptr& ptr) override;
        void objectChanged(const MWWorld::Ptr& ptr) override;
        void objectRemoving(const MWWorld::Ptr& ptr) noexcept override;
        void worldResetting() noexcept override;

        [[nodiscard]] bool healthy() const noexcept { return mRouteStatus->healthy(); }
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept
        {
            return mRouteStatus->firstDiagnostic();
        }

    private:
        void requireHealthy() const;
        void publishObject(const MWWorld::Ptr& ptr);
        void recordFailure(std::string_view message) noexcept;

        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        std::shared_ptr<V4RenderRouteStatus> mRouteStatus;
        const VFS::Manager& mVfs;
        // Source capability metadata is model-global but deliberately remains
        // outside backend ownership. Cache it by normalized winning VFS path so
        // live door/reference updates never reparse the NIF merely to recover
        // NightDaySwitch/HerbalismSwitch root user descriptions.
        std::map<std::string, std::uint64_t, std::less<>> mModelVisualCapabilities;
    };
}

#endif
