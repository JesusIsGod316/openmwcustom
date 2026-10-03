#ifndef OPENMW_COMPONENTS_TERRAIN_TEXTUREMANAGER_H
#define OPENMW_COMPONENTS_TERRAIN_TEXTUREMANAGER_H

#include <components/resource/resourcemanager.hpp>
#include <components/resource/nativetexture.hpp>
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
    class NativeTextureProducer;

    class TextureManager : public Resource::ResourceManager
    {
    public:
        explicit TextureManager(Resource::SceneManager* sceneMgr, double expiryDelay, bool canonicalPublication = false);
        ~TextureManager() override;

        void updateTextureFiltering();

        osg::ref_ptr<osg::Texture2D> getTexture(VFS::Path::NormalizedView name,
            Resource::TextureRole role=Resource::TextureRole::Diffuse,
            Resource::TexturePurpose purpose=Resource::TexturePurpose::MainDraw);
        void prepareTexture(VFS::Path::NormalizedView,Resource::TextureRole,Resource::TexturePurpose);
        void clearCache() override;
        void reportRuntimeDiagnostics(double referenceTime) const noexcept override;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

    private:
        Resource::SceneManager* mSceneManager;
        const bool mCanonicalPublication; // startup-only; preserves the old path when off
        std::atomic_uint64_t mPublicationRaces{0};
        std::unique_ptr<NativeTextureProducer> mNativePreparation;
        mutable Debug::RuntimeDiagnostics::Sampler mTextureSampler;
    };

}

#endif
