#ifndef OPENMW_COMPONENTS_RESOURCE_NIFFILEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_NIFFILEMANAGER_H

#include <components/nif/niffile.hpp>

#include "resourcemanager.hpp"
#include <algorithm>

namespace ToUTF8
{
    class StatelessUtf8Encoder;
}

namespace Resource
{

    /// @brief Handles caching of NIFFiles.
    /// @note May be used from any thread.
    class NifFileManager : public ResourceManager
    {
        const ToUTF8::StatelessUtf8Encoder* mEncoder;

    public:
        NifFileManager(const VFS::Manager* vfs, const ToUTF8::StatelessUtf8Encoder* encoder);
        ~NifFileManager();

        /// Retrieve a NIF file from the cache, or load it from the VFS if not cached yet.
        /// @note For performance reasons the NifFileManager does not handle case folding, needs
        /// to be done in advance by other managers accessing the NifFileManager.
        Nif::NIFFilePtr get(VFS::Path::NormalizedView name);
        // Startup-only C2. Legacy shortens the ordinary TTL; explicit v2 can
        // retain a short parsed-source tier even when Normal's ordinary TTL is0.
        void setColdRetentionSeconds(double seconds,bool independent=false);
        double effectiveExpiryDelay() const noexcept
        { return mColdRetentionSeconds>0 ? (mIndependentColdRetention ? mColdRetentionSeconds
            : (std::min)(mExpiryDelay,mColdRetentionSeconds)) : mExpiryDelay; }
        void updateCache(double referenceTime) override;
        void reportRuntimeDiagnostics(double referenceTime) const noexcept override;

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;
    private:
        double mColdRetentionSeconds = 0;
        bool mIndependentColdRetention=false;
    };

}

#endif
