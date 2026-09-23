#ifndef OPENMW_MWRENDER_V4SKYCAPTURE_H
#define OPENMW_MWRENDER_V4SKYCAPTURE_H

#include <components/rendercore/skyframe.hpp>
#include <osg/observer_ptr>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace osg { class Geometry; class Group; }
namespace NifRender { class TextureIdentityCache; }
namespace MWRender
{
    class V4SkyCapture
    {
    public:
        struct Result
        {
            std::shared_ptr<const RenderCore::NativeSkySnapshot> snapshot;
            std::string diagnostic;
            explicit operator bool() const noexcept { return snapshot && diagnostic.empty(); }
        };
        Result capture(osg::Group& root, NifRender::TextureIdentityCache& textures);
        struct GeometrySnapshot
        {
            std::uint64_t identity = 0;
            std::shared_ptr<const RenderCore::MeshPayload> mesh;
        };
        GeometrySnapshot geometry(const osg::Geometry& source, std::string& diagnostic);
    private:
        struct GeometryEntry
        {
            osg::observer_ptr<const osg::Geometry> source;
            std::vector<std::uint64_t> signature;
            GeometrySnapshot snapshot;
        };
        std::map<const osg::Geometry*, GeometryEntry> mGeometry;
        std::uint64_t mNextIdentity = 1;
    };
}
#endif
