#ifndef GAME_MWWORLD_SCENERENDERLIFECYCLE_H
#define GAME_MWWORLD_SCENERENDERLIFECYCLE_H

namespace MWWorld
{
    class CellStore;
    class Ptr;

    // Backend-neutral observation point for the authoritative scene lifecycle.
    // Implementations consume game/VFS state directly; OSG nodes, backend
    // objects, and pointer values must never become semantic identities.
    //
    // Activation and publication callbacks may throw to fail an explicitly
    // selected renderer before incomplete state is presented. Retirement and
    // reset callbacks are noexcept so gameplay teardown can always complete.
    class SceneRenderLifecycle
    {
    public:
        virtual ~SceneRenderLifecycle() = default;

        virtual void cellActivated(const CellStore& cell) = 0;
        virtual void cellDeactivating(const CellStore& cell) noexcept = 0;
        virtual void objectAdded(const Ptr& ptr) = 0;
        virtual void objectChanged(const Ptr& ptr) = 0;
        virtual void objectRemoving(const Ptr& ptr) noexcept = 0;
        virtual void worldResetting() noexcept = 0;
    };
}

#endif
