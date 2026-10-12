#include "texturemanager.hpp"
#include "nativetextureproducer.hpp"

#include <osg/Texture2D>

#include <components/resource/imagemanager.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/assetpolicy.hpp>
#include <components/resource/texturestreaming.hpp>
#include <components/sceneutil/glextensions.hpp>
#include <components/vfs/manager.hpp>

namespace Terrain
{
    TextureManager::TextureManager(Resource::SceneManager* sceneMgr, double expiryDelay, bool canonicalPublication)
        : ResourceManager(sceneMgr->getVFS(), expiryDelay)
        , mSceneManager(sceneMgr)
        , mCanonicalPublication(canonicalPublication)
    {
        if(Resource::AssetPolicy::enabled(Resource::AssetPolicy::TexturePreparation)
            || Resource::AssetPolicy::enabled(Resource::AssetPolicy::TextureStreaming))
            mNativePreparation=std::make_unique<NativeTextureProducer>(sceneMgr->getVFS());
    }
    TextureManager::~TextureManager()=default;

    void TextureManager::prepareTexture(VFS::Path::NormalizedView name,Resource::TextureRole role,
        Resource::TexturePurpose purpose)
    { if(mNativePreparation) mNativePreparation->prepare(name,role,purpose); }

    void TextureManager::clearCache()
    {
        if(mNativePreparation) mNativePreparation->clear();
        ResourceManager::clearCache();
    }

    struct UpdateTextureFilteringFunctor
    {
        UpdateTextureFilteringFunctor(Resource::SceneManager* sceneMgr)
            : mSceneManager(sceneMgr)
        {
        }
        Resource::SceneManager* mSceneManager;

        void operator()(std::string, osg::Object* obj)
        {
            mSceneManager->applyFilterSettings(static_cast<osg::Texture2D*>(obj));
        }
    };

    void TextureManager::updateTextureFiltering()
    {
        UpdateTextureFilteringFunctor f(mSceneManager);
        mCache->call(f);
    }

    osg::ref_ptr<osg::Texture2D> TextureManager::getTexture(VFS::Path::NormalizedView name,
        Resource::TextureRole role,Resource::TexturePurpose purpose)
    {
        // don't bother with case folding, since there is only one way of referring to terrain textures we can assume
        // the case is always the same
        const auto generation=mNativePreparation ? mVFS->getIndexGeneration() : 0;
        const std::string key=mNativePreparation
            ? Resource::nativeTextureKey(name.value(),role,purpose)+"|vfs="+std::to_string(generation)
            : std::string(name.value());
        osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(key);

        if (obj != nullptr)
        {
            // Retry only producer-owned transient refusals. Cached wrappers
            // retain the same ticket and owner publication observes readiness.
            if(mNativePreparation) mNativePreparation->prepare(name,role,purpose);
            return static_cast<osg::Texture2D*>(obj.get());
        }

        // Capabilities are qualified by the actual GL owner. Early terrain
        // preparation commonly precedes global extension initialization.
        const bool supported=bool(mNativePreparation);
        const auto selected=mNativePreparation ? mNativePreparation->select(name,role,purpose,supported)
            : NativeTextureProducer::Selection{};
        osg::ref_ptr<osg::Image> image=selected.image;
        if(!image)
        {
            if(mNativePreparation) ++Resource::nativeTextureStats().fallback;
            image=mSceneManager->getImageManager()->getImage(name);
        }
        osg::ref_ptr<osg::Texture2D> texture=NativeTextureProducer::realize(selected,image,mCanonicalPublication);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::REPEAT);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::REPEAT);
        mSceneManager->applyFilterSettings(texture);
        if (mCanonicalPublication || mNativePreparation)
        {
            auto [published, inserted] = mCache->getOrInsert(key, texture.get());
            if (!inserted)
                mPublicationRaces.fetch_add(1, std::memory_order_relaxed);
            return static_cast<osg::Texture2D*>(published.get());
        }
        mCache->addEntryToObjectCache(key, texture.get());
        return texture;
    }

    void TextureManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Terrain Texture", frameNumber, mCache->getStats(), *stats);
        if (mCanonicalPublication)
            stats->setAttribute(frameNumber, "P8U1 Terrain Texture Publication Races",
                static_cast<double>(mPublicationRaces.load(std::memory_order_relaxed)));
        if(mNativePreparation)
        {
            const auto& native=Resource::nativeTextureStats();
            const auto& stream=Resource::TextureStreaming::counters();
            stats->setAttribute(frameNumber,"T1 Native Texture Ready",static_cast<double>(native.ready.load()));
            stats->setAttribute(frameNumber,"T2 Texture Coarse Draws",static_cast<double>(stream.coarseDraws.load()));
            stats->setAttribute(frameNumber,"T2 Texture Fine Draws",static_cast<double>(stream.fineDraws.load()));
            stats->setAttribute(frameNumber,"T2 Texture Actual Deletions",static_cast<double>(stream.deletions.load()));
        }
    }

    void TextureManager::reportRuntimeDiagnostics(double referenceTime) const noexcept
    {
        ResourceManager::reportRuntimeDiagnostics(referenceTime);
        if(!mNativePreparation || !mTextureSampler.due()) return;
        const auto& v=Resource::nativeTextureStats();
        Debug::RuntimeDiagnostics::recordEvent("native_texture_stats","terrain",{},
            {{"requested",v.requested.load()},{"eligible",v.eligible.load()},{"ready",v.ready.load()},
             {"dds_reuse",v.reusedDds.load()},{"converted",v.converted.load()},
             {"cache_hits",v.cacheHits.load()},{"cache_misses",v.cacheMisses.load()},
             {"cache_writes",v.cacheWrites.load()},{"fallback",v.fallback.load()},{"retained_bytes",v.retainedBytes.load()}});
        Debug::RuntimeDiagnostics::recordEvent("native_texture_use","terrain",{},
            {{"native_images",v.nativeImages.load()},{"full_applies",v.fullApplies.load()},
             {"handoffs",v.handoffs.load()},{"retries",v.retries.load()},
             {"terminal_declines",v.terminalDeclines.load()},{"transient_declines",v.transientDeclines.load()},
             {"consumed_main_draws",v.consumedMainDraws.load()},{"consumed_composite_draws",v.consumedCompositeDraws.load()}});
        for(unsigned i=1;i<static_cast<unsigned>(Resource::NativeTextureSkip::Count);++i)
            Debug::RuntimeDiagnostics::recordEvent("native_texture_skip","terrain",
                Resource::nativeTextureSkipName(static_cast<Resource::NativeTextureSkip>(i)),{{"count",v.skips[i].load()}});
        Resource::TextureStreaming::reportDiagnostics();
    }

}
