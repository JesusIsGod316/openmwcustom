#ifndef OPENMW_COMPONENTS_TERRAIN_TEXTUREMANAGER_H
#define OPENMW_COMPONENTS_TERRAIN_TEXTUREMANAGER_H

#include <components/resource/resourcemanager.hpp>
#include <atomic>
#include <components/vfs/pathutil.hpp>

namespace Resource
{
    class SceneManager;
}

namespace osg
{
    class Texture2D;
}

namespace Terrain
{

    class TextureManager : public Resource::ResourceManager
    {
    public:
        explicit TextureManager(Resource::SceneManager* sceneMgr, double expiryDelay, bool canonicalPublication = false);

        void updateTextureFiltering();

        osg::ref_ptr<osg::Texture2D> getTexture(VFS::Path::NormalizedView name);

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

    private:
        Resource::SceneManager* mSceneManager;
        const bool mCanonicalPublication; // startup-only; preserves the old path when off
        std::atomic_uint64_t mPublicationRaces{0};
    };

}

#endif
