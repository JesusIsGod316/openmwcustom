#ifndef OPENMW_SCENEUTIL_TEMPORALMOTIONIDENTITY_H
#define OPENMW_SCENEUTIL_TEMPORALMOTIONIDENTITY_H
#include <osg/Geometry>
#include <osg/UserDataContainer>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdint>
namespace SceneUtil
{
    inline bool temporalDynamicMotionEnabled()
    {
        static const bool enabled = [] {
            const char* value = std::getenv("OPENMW_P9_DYNAMIC_MOTION");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }
    // One identity shared by the two private CPU-deformed buffers of ONE
    // actor/morph instance. A resource template or frame parity is not an
    // instance identity. Replacing the source mesh creates a new identity.
    class TemporalMotionIdentity final : public osg::Object
    {
    public:
        explicit TemporalMotionIdentity() : id(++sNext) {}
        TemporalMotionIdentity(const TemporalMotionIdentity& value, const osg::CopyOp& copy)
            : osg::Object(value, copy), id(++sNext) {}
        META_Object(SceneUtil, TemporalMotionIdentity)
        const std::uint64_t id;
    private:
        inline static std::atomic<std::uint64_t> sNext{0};
    };
    inline void installTemporalMotionIdentity(osg::Geometry& geometry, TemporalMotionIdentity* identity)
    {
        // Geometry's SHALLOW_COPY may retain the asset/template's container.
        // Do not write this instance identity into a shared resource object.
        if (const auto* original = geometry.getUserDataContainer())
            geometry.setUserDataContainer(static_cast<osg::UserDataContainer*>(original->clone(osg::CopyOp::SHALLOW_COPY)));
        auto* data = geometry.getOrCreateUserDataContainer();
        for (unsigned i = data->getNumUserObjects(); i > 0; --i)
            if (dynamic_cast<TemporalMotionIdentity*>(data->getUserObject(i - 1))) data->removeUserObject(i - 1);
        data->addUserObject(identity);
    }
    inline const TemporalMotionIdentity* temporalMotionIdentity(const osg::Geometry& geometry)
    {
        const auto* data = geometry.getUserDataContainer();
        if (!data) return nullptr;
        for (unsigned i = 0; i < data->getNumUserObjects(); ++i)
            if (const auto* identity = dynamic_cast<const TemporalMotionIdentity*>(data->getUserObject(i))) return identity;
        return nullptr;
    }
}
#endif
