from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"optimization-lab fixup patched {rel}")


# Keep components/resource independent from components/settings. The app layer
# resolves the policy and passes one boolean down to ResourceSystem.
replace_once(
    "components/resource/resourcesystem.hpp",
    '''        explicit ResourceSystem(
            const VFS::Manager* vfs, double expiryDelay, const ToUTF8::StatelessUtf8Encoder* encoder);''',
    '''        explicit ResourceSystem(const VFS::Manager* vfs, double expiryDelay,
            const ToUTF8::StatelessUtf8Encoder* encoder, bool retainNifFiles = false);''',
)
replace_once(
    "components/resource/resourcesystem.hpp",
    '''        const VFS::Manager* mVFS;

        ResourceSystem(const ResourceSystem&);''',
    '''        const VFS::Manager* mVFS;
        bool mRetainNifFiles = false;

        ResourceSystem(const ResourceSystem&);''',
)

replace_once(
    "components/resource/resourcesystem.cpp",
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/settings/ramcache.hpp>

#include "animblendrulesmanager.hpp"''',
    '''#include <components/debug/v3diagnostics.hpp>

#include "animblendrulesmanager.hpp"''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''    ResourceSystem::ResourceSystem(
        const VFS::Manager* vfs, double expiryDelay, const ToUTF8::StatelessUtf8Encoder* encoder)
        : mVFS(vfs)''',
    '''    ResourceSystem::ResourceSystem(const VFS::Manager* vfs, double expiryDelay,
        const ToUTF8::StatelessUtf8Encoder* encoder, bool retainNifFiles)
        : mVFS(vfs)
        , mRetainNifFiles(retainNifFiles)''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''        if (Settings::RamCache::retainNifFiles())
            mNifFileManager->setExpiryDelay(expiryDelay);''',
    '''        if (mRetainNifFiles)
            mNifFileManager->setExpiryDelay(expiryDelay);''',
)
replace_once(
    "components/resource/resourcesystem.cpp",
    '''        mNifFileManager->setExpiryDelay(Settings::RamCache::retainNifFiles() ? expiryDelay : 0.0);''',
    '''        mNifFileManager->setExpiryDelay(mRetainNifFiles ? expiryDelay : 0.0);''',
)

replace_once(
    "apps/openmw/engine.cpp",
    '''    mResourceSystem = std::make_unique<Resource::ResourceSystem>(
        mVFS.get(), effectiveResourceCacheExpiry, &mEncoder.get()->getStatelessEncoder());''',
    '''    mResourceSystem = std::make_unique<Resource::ResourceSystem>(mVFS.get(), effectiveResourceCacheExpiry,
        &mEncoder.get()->getStatelessEncoder(), Settings::RamCache::retainNifFiles());''',
)

print("V3 Optimization Lab resource-layer fixups completed successfully.")
