#ifndef OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITYCACHE_H
#define OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITYCACHE_H

#include "vfsidentity.hpp"

#include <list>
#include <map>
#include <stdexcept>
#include <cstdlib>

namespace NifRender
{
    // Owned by one capture context, not global or shared with Lua workers.
    // Cache only content/provenance, never material interpretation or GPU data.
    // VFS generation handles same-name/same-time winning-archive replacement.
    // Loose-file edits follow OpenMW's last-modified invalidation convention.
    // Timestamp-preserving external edits require clear() or an index rebuild.
    class TextureIdentityCache
    {
    public:
        explicit TextureIdentityCache(const VFS::Manager& vfs, std::size_t capacity = 4096)
            : mVfs(vfs), mCapacity(capacity), mGeneration(vfs.getIndexGeneration())
        {
        }
        TextureIdentityCache(const TextureIdentityCache&) = delete;
        TextureIdentityCache& operator=(const TextureIdentityCache&) = delete;

        // One main-thread snapshot observes each winning texture once. Recheck
        // metadata on the next snapshot, not once per drawable using the image.
        // Never use this scope across a loading/event boundary or worker thread.
        class CaptureScope
        {
        public:
            explicit CaptureScope(TextureIdentityCache& cache) : mCache(cache)
            {
                if (mCache.mCaptureDepth++ == 0) mCache.mCaptured.clear();
            }
            ~CaptureScope()
            {
                if (--mCache.mCaptureDepth == 0) mCache.mCaptured.clear();
            }
            CaptureScope(const CaptureScope&) = delete;
            CaptureScope& operator=(const CaptureScope&) = delete;
        private:
            TextureIdentityCache& mCache;
        };

        void clear()
        {
            mEntries.clear();
            mLru.clear();
            mCaptured.clear();
            mGeneration = mVfs.getIndexGeneration();
        }

        [[nodiscard]] ResolvedVfsIdentity resolve(VFS::Path::NormalizedView authoredPath)
        {
            if (mGeneration != mVfs.getIndexGeneration())
                clear();
            // Same-executable control retains the original full read/hash path.
            if (std::getenv("OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES"))
                return resolveTextureVfsIdentity(authoredPath, mVfs);
            const std::string captureKey(authoredPath.value());
            if (mCaptureDepth != 0)
            {
                const auto captured = mCaptured.find(captureKey);
                if (captured != mCaptured.end()) return captured->second;
            }
            auto identity = resolveChecked(authoredPath);
            if (mCaptureDepth != 0 && identity.valid() && mCaptured.size() < mCapacity)
                mCaptured.emplace(captureKey, identity);
            return identity;
        }

        [[nodiscard]] std::size_t size() const noexcept { return mEntries.size(); }

    private:
        [[nodiscard]] ResolvedVfsIdentity resolveChecked(VFS::Path::NormalizedView authoredPath)
        {
            const auto path = Misc::ResourceHelpers::correctTexturePath(authoredPath, mVfs);
            if (!mVfs.exists(path))
                return { path, {}, {} }; // never cache missing/failed assets
            const auto modified = mVfs.getLastModified(path);
            const std::string key(path.value());
            auto found = mEntries.find(key);
            if (found != mEntries.end() && found->second.modified == modified)
            {
                mLru.splice(mLru.begin(), mLru, found->second.lru);
                return found->second.identity;
            }
            if (found != mEntries.end())
            {
                mLru.erase(found->second.lru);
                mEntries.erase(found);
            }
            auto identity = resolveTextureVfsIdentity(path, mVfs);
            if (!identity.valid() || mCapacity == 0)
                return identity;
            // Do not cache a payload that changed while being read.
            if (mVfs.getLastModified(path) != modified)
                throw std::runtime_error("Texture changed during identity capture: " + key);
            if (mEntries.size() == mCapacity)
            {
                mEntries.erase(mLru.back());
                mLru.pop_back();
            }
            mLru.push_front(key);
            try { mEntries.emplace(key, Entry{ identity, modified, mLru.begin() }); }
            catch (...) { mLru.pop_front(); throw; }
            return identity;
        }

        struct Entry
        {
            ResolvedVfsIdentity identity;
            std::filesystem::file_time_type modified;
            std::list<std::string>::iterator lru;
        };
        const VFS::Manager& mVfs;
        std::size_t mCapacity;
        std::uint64_t mGeneration;
        std::list<std::string> mLru;
        std::map<std::string, Entry, std::less<>> mEntries;
        unsigned int mCaptureDepth = 0;
        std::map<std::string, ResolvedVfsIdentity, std::less<>> mCaptured;
    };
}
#endif
