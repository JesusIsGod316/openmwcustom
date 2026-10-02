#ifndef OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H
#define OPENMW_COMPONENTS_RESOURCE_IMAGEMANAGER_H

#include <osg/Image>
#include <osg/Texture2D>
#include <osg/ref_ptr>

#include <components/vfs/pathutil.hpp>

#include "resourcemanager.hpp"

namespace osgDB
{
    class Options;
}

namespace Resource
{

    /// @brief Handles loading/caching of Images.
    /// @note May be used from any thread.
    class ImageManager : public ResourceManager
    {
    public:
        explicit ImageManager(const VFS::Manager* vfs, double expiryDelay);
        ~ImageManager();

        /// Create or retrieve an Image
        /// Returns the dummy image if the given image is not found.
        osg::ref_ptr<osg::Image> getImage(VFS::Path::NormalizedView path, bool disableFlip = false);

        // Optional owners must match publication, decode options and VFS
        // generation. Copying the matching charge does not double-charge bytes.
        ObjectCacheLease getImageLease(VFS::Path::NormalizedView path, const osg::Image* expected = nullptr,
            bool disableFlip = false);
        bool imageLeaseCurrent(VFS::Path::NormalizedView path, const ObjectCacheLease& lease,
            bool disableFlip = false);
        ObjectCacheLease prepareImage(VFS::Path::NormalizedView path, std::uint64_t maximumEstimate);

        osg::Image* getWarningImage();

        void reportStats(unsigned int frameNumber, osg::Stats* stats) const override;

    private:
        std::uint64_t sourceRevision(bool disableFlip) const noexcept;
        osg::ref_ptr<osg::Image> mWarningImage;
        osg::ref_ptr<osgDB::Options> mOptions;

        ImageManager(const ImageManager&);
        void operator=(const ImageManager&);
    };

}

#endif
