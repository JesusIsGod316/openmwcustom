#ifndef OPENMW_RESOURCE_TEMPLATECOALESCING_H
#define OPENMW_RESOURCE_TEMPLATECOALESCING_H
#include <osg/Node>
#include <osg/Object>
#include <osg/UserDataContainer>
#include <atomic>
#include <cstdint>

namespace Resource::TemplateCoalescing
{
    inline std::atomic_bool enabled{false};
    struct Counters
    {
        inline static std::atomic_uint64_t afterLoadReuses{0}, publishReuses{0}, publications{0}, changedEpochs{0};
    };
    struct Epoch
    {
        const void* manager = nullptr;
        std::uint64_t cache = 0, vfs = 0, shader = 0;
        friend bool operator==(const Epoch&, const Epoch&) = default;
    };
    class Identity final : public osg::Object
    {
    public:
        Identity() = default;
        explicit Identity(Epoch value) : epoch(value) {}
        Identity(const Identity& other, const osg::CopyOp& copy = osg::CopyOp::SHALLOW_COPY)
            : osg::Object(other,copy), epoch(other.epoch) {}
        META_Object(Resource, Identity)
        Epoch epoch;
    };
    inline bool matches(const osg::Object* object, const Epoch& epoch)
    {
        if (!object || !epoch.manager || !epoch.cache || !epoch.vfs || !epoch.shader) return false;
        const auto* data = object->getUserDataContainer();
        if (!data || data->getNumUserObjects()>256) return false;
        for (unsigned i=0;i<data->getNumUserObjects();++i)
            if (const auto* identity=dynamic_cast<const Identity*>(data->getUserObject(i))) return identity->epoch==epoch;
        return false;
    }
    inline void mark(osg::Node& node, Epoch epoch)
    { node.getOrCreateUserDataContainer()->addUserObject(new Identity(epoch)); }
}
#endif
