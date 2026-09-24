#ifndef OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITYCACHE_H
#define OPENMW_COMPONENTS_NIFRENDER_TEXTUREIDENTITYCACHE_H

#include "vfsidentity.hpp"
#include <components/misc/environmentflag.hpp>
#include <components/rendercore/boundedparallelfor.hpp>

#include <list>
#include <map>
#include <stdexcept>
#include <cstdlib>
#include <atomic>

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
        // Load-time texture bindings share one checked identity per capture.
        // The slot owns no GPU data and is invalidated by scope, cache lifetime,
        // explicit clear, or winning-VFS generation. Only the capture owner
        // thread reads/writes it; no live cache escapes to rendering workers.
        class Binding
        {
            friend class TextureIdentityCache;
            VFS::Path::Normalized path;
            ResolvedVfsIdentity value;
            std::uint64_t owner = 0, capture = 0, generation = 0, invalidation = 0;
        public:
            explicit Binding(VFS::Path::NormalizedView authored) : path(authored) {}
        };
        std::shared_ptr<Binding> bind(VFS::Path::NormalizedView path)
        {
            const auto found = mBindings.find(path.value());
            if (found != mBindings.end())
                if (auto live = found->second.lock()) return live;
            auto result = std::make_shared<Binding>(path);
            if (mBindings.size() >= mCapacity)
                std::erase_if(mBindings, [](const auto& pair) { return pair.second.expired(); });
            if (mBindings.size() < mCapacity)
                mBindings.insert_or_assign(std::string(path.value()), result);
            return result;
        }
        const ResolvedVfsIdentity& resolveBound(Binding& binding)
        {
            if (!mCaptureDepth || binding.owner != mBindingOwner || binding.capture != mCaptureSerial
                || binding.generation != mVfs.getIndexGeneration() || binding.invalidation != mInvalidation)
            {
                binding.value = resolve(binding.path);
                binding.owner = mBindingOwner; binding.capture = mCaptureSerial;
                binding.generation = mVfs.getIndexGeneration(); binding.invalidation = mInvalidation;
            }
            return binding.value;
        }
        std::shared_ptr<Binding> bindLoaded(VFS::Path::NormalizedView path)
        {
            auto binding = bind(path);
            // An engine asset/material rebind is an invalidation event even if
            // its filename is unchanged. Do not inherit an old residency's hash.
            binding->owner = 0;
            return binding;
        }
        const ResolvedVfsIdentity& resolveLoaded(Binding& binding)
        {
            // Match ImageManager's retained loaded-image lifetime, not a live
            // filesystem watcher. External file edits become visible on engine
            // rebind/reload. Winning VFS rebuilds and explicit clear stay immediate.
            if (binding.owner != mBindingOwner || binding.generation != mVfs.getIndexGeneration()
                || binding.invalidation != mInvalidation)
                return resolveBound(binding);
            return binding.value;
        }
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
                if (mCache.mCaptureDepth == 0) mCache.prepareMetadata();
                ++mCache.mCaptureDepth;
            }
            ~CaptureScope()
            {
                if (--mCache.mCaptureDepth == 0) mCache.mObservedMetadata.clear();
            }
            CaptureScope(const CaptureScope&) = delete;
            CaptureScope& operator=(const CaptureScope&) = delete;
        private:
            TextureIdentityCache& mCache;
        };

        void clear()
        {
            ++mInvalidation;
            mEntries.clear();
            mLru.clear();
            mCaptured.clear();
            mObservedMetadata.clear();
            mGeneration = mVfs.getIndexGeneration();
        }

        [[nodiscard]] ResolvedVfsIdentity resolve(VFS::Path::NormalizedView authoredPath)
        {
            if (mGeneration != mVfs.getIndexGeneration())
                clear();
            // Same-executable control retains the original full read/hash path.
            if (Misc::environmentFlag<"OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES">())
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
        void prepareMetadata()
        {
            ++mCaptureSerial;
            if (mGeneration != mVfs.getIndexGeneration()) clear();
            mObservedMetadata.clear();
            if (Misc::environmentFlag<"OPENMW_V4_BATCH_TEXTURE_METADATA">()
                && !Misc::environmentFlag<"OPENMW_V4_UNCACHED_TEXTURE_IDENTITIES">())
            {
                // Predict only last frame's used files. No live VFS object,
                // loader, cache map or stream is accessed by a worker.
                std::map<std::filesystem::path, std::filesystem::file_time_type> paths;
                for (const auto& [name, identity] : mCaptured)
                {
                    const auto found = mEntries.find(identity.canonicalPath.value());
                    if (found != mEntries.end() && found->second.metadataPath)
                        paths.emplace(*found->second.metadataPath, std::filesystem::file_time_type{});
                }
                struct Read { std::filesystem::path path; std::filesystem::file_time_type value; std::error_code error; };
                std::vector<Read> reads;
                reads.reserve(paths.size());
                for (const auto& [path, ignored] : paths) reads.push_back({path,{}, {}});
                if (!mMetadataWorkers && reads.size() >= 32)
                    mMetadataWorkers = std::make_unique<RenderCore::BoundedParallelFor>(2,32,4);
                const auto read = [&](std::size_t i) {
                    reads[i].value = std::filesystem::last_write_time(reads[i].path, reads[i].error);
                };
                if (mMetadataWorkers) mMetadataWorkers->forEach(reads.size(), read);
                else for (std::size_t i=0;i<reads.size();++i) read(i);
                for (auto& result : reads)
                    if (!result.error) mObservedMetadata.emplace(std::move(result.path),result.value);
                // A disappeared predicted file is not fatal unless requested:
                // its normal checked read below still reports the original error.
            }
            mCaptured.clear();
        }

        [[nodiscard]] ResolvedVfsIdentity resolveChecked(VFS::Path::NormalizedView authoredPath)
        {
            const auto path = Misc::ResourceHelpers::correctTexturePath(authoredPath, mVfs);
            if (!mVfs.exists(path))
                return { path, {}, {} }; // never cache missing/failed assets
            const std::string key(path.value());
            auto found = mEntries.find(key);
            const bool batched = mCaptureDepth && Misc::environmentFlag<"OPENMW_V4_BATCH_TEXTURE_METADATA">();
            const auto metadataPath = found != mEntries.end() ? found->second.metadataPath : mVfs.getMetadataPath(path);
            std::filesystem::file_time_type modified;
            if (batched && metadataPath)
            {
                auto observed = mObservedMetadata.find(*metadataPath);
                if (observed == mObservedMetadata.end())
                    observed = mObservedMetadata.emplace(*metadataPath,mVfs.getLastModified(path)).first;
                modified = observed->second;
            }
            else modified = mVfs.getLastModified(path);
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
            try { mEntries.emplace(key, Entry{ identity, modified, mLru.begin(), metadataPath }); }
            catch (...) { mLru.pop_front(); throw; }
            return identity;
        }

        struct Entry
        {
            ResolvedVfsIdentity identity;
            std::filesystem::file_time_type modified;
            std::list<std::string>::iterator lru;
            std::optional<std::filesystem::path> metadataPath;
        };
        const VFS::Manager& mVfs;
        inline static std::atomic<std::uint64_t> sBindingOwners{0};
        const std::uint64_t mBindingOwner = ++sBindingOwners;
        std::uint64_t mCaptureSerial = 0, mInvalidation = 0;
        std::map<std::string, std::weak_ptr<Binding>, std::less<>> mBindings;
        std::size_t mCapacity;
        std::uint64_t mGeneration;
        std::list<std::string> mLru;
        std::map<std::string, Entry, std::less<>> mEntries;
        unsigned int mCaptureDepth = 0;
        std::map<std::string, ResolvedVfsIdentity, std::less<>> mCaptured;
        std::map<std::filesystem::path, std::filesystem::file_time_type> mObservedMetadata;
        std::unique_ptr<RenderCore::BoundedParallelFor> mMetadataWorkers;
    };
}
#endif
