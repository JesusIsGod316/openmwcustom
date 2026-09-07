#include <apps/openmw/mwworld/scenerenderlifecycle.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace MWWorld
{
    class CellStore
    {
    };

    class Ptr
    {
    };
}

namespace
{
    class Recorder final : public MWWorld::SceneRenderLifecycle
    {
    public:
        void cellActivated(const MWWorld::CellStore&) override { mEvents.emplace_back("cell+"); }
        void cellDeactivating(const MWWorld::CellStore&) noexcept override { mEvents.emplace_back("cell-"); }
        void objectAdded(const MWWorld::Ptr&) override { mEvents.emplace_back("object+"); }
        void objectChanged(const MWWorld::Ptr&) override { mEvents.emplace_back("object~"); }
        void objectRemoving(const MWWorld::Ptr&) noexcept override { mEvents.emplace_back("object-"); }
        void worldResetting() noexcept override { mEvents.emplace_back("reset"); }

        std::vector<std::string> mEvents;
    };

    void require(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    static_assert(std::has_virtual_destructor_v<MWWorld::SceneRenderLifecycle>);
    static_assert(noexcept(std::declval<Recorder&>().cellDeactivating(std::declval<const MWWorld::CellStore&>())));
    static_assert(noexcept(std::declval<Recorder&>().objectRemoving(std::declval<const MWWorld::Ptr&>())));
    static_assert(noexcept(std::declval<Recorder&>().worldResetting()));

    MWWorld::CellStore cell;
    MWWorld::Ptr object;
    Recorder recorder;
    recorder.cellActivated(cell);
    recorder.objectAdded(object);
    recorder.objectChanged(object);
    recorder.objectRemoving(object);
    recorder.cellDeactivating(cell);
    recorder.worldResetting();

    require(recorder.mEvents
            == std::vector<std::string>{ "cell+", "object+", "object~", "object-", "cell-", "reset" },
        "scene render lifecycle order changed");
}
