#ifndef OPENMW_MWRENDER_V4SCENERENDERLIFECYCLE_H
#define OPENMW_MWRENDER_V4SCENERENDERLIFECYCLE_H

#include "../mwworld/scenerenderlifecycle.hpp"

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
        V4SceneRenderLifecycle(std::shared_ptr<RenderVsg::VsgSemanticSession> session, const VFS::Manager& vfs);

        void cellActivated(const MWWorld::CellStore& cell) override;
        void cellDeactivating(const MWWorld::CellStore& cell) noexcept override;
        void objectAdded(const MWWorld::Ptr& ptr) override;
        void objectChanged(const MWWorld::Ptr& ptr) override;
        void objectRemoving(const MWWorld::Ptr& ptr) noexcept override;
        void worldResetting() noexcept override;

        [[nodiscard]] bool healthy() const noexcept { return mHealthy; }
        [[nodiscard]] const std::string& lastDiagnostic() const noexcept { return mLastDiagnostic; }

    private:
        void publishStaticObject(const MWWorld::Ptr& ptr);
        void recordRetirementFailure(std::string_view message) noexcept;

        std::shared_ptr<RenderVsg::VsgSemanticSession> mSession;
        const VFS::Manager& mVfs;
        bool mHealthy = true;
        std::string mLastDiagnostic;
    };
}

#endif
