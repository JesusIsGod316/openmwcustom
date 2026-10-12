#include <components/resource/resourcesystem.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/cachemaintenance.hpp>
#include <components/files/memorystream.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>

#include <gtest/gtest.h>
#include <osg/observer_ptr>
#include <osgDB/Registry>
#include <chrono>
#include <functional>
#include <limits>

namespace
{
    struct CachedImages final : Resource::ImageManager
    {
        using ImageManager::ImageManager;
        void insert(const std::string& key, osg::Image* image, double stamp)
        { mCache->addEntryToObjectCache(key, image, stamp); }
        osg::ref_ptr<osg::Object> get(const std::string& key)
        { return mCache->getRefFromObjectCache(key); }
        std::size_t size() const { return mCache->getStats().mSize; }
    };
    void sweep(CachedImages& manager, double time, std::size_t scans = 4096)
    {
        Resource::CacheMaintenanceBudget budget(scans, 64, std::chrono::seconds(1));
        Resource::CacheMaintenanceScope scope(budget);
        manager.updateCache(time);
    }
    osg::ref_ptr<osg::Image> image()
    {
        osg::ref_ptr<osg::Image> value = new osg::Image;
        value->allocateImage(4, 4, 1, GL_RGBA, GL_UNSIGNED_BYTE);
        return value;
    }

    TEST(ResourceVisualRetention, zeroOverridesPreserveCommonAndParsedPolicies)
    {
        VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resources(&vfs, 120, &encoder.getStatelessEncoder());
        resources.configureVisualCacheRetention(0, 0);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 120);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 120);
        resources.setExpiryDelay(80);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 80);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 80);
        EXPECT_EQ(resources.getKeyframeManager()->getExpiryDelay(), 80);
        EXPECT_EQ(resources.getNifFileManager()->getExpiryDelay(), 0);
    }

    TEST(ResourceVisualRetention, independentOverridesSurviveGlobalChanges)
    {
        VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resources(&vfs, 120, &encoder.getStatelessEncoder(), true);
        resources.configureVisualCacheRetention(15, 30);
        resources.setExpiryDelay(240);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 15);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 30);
        EXPECT_EQ(resources.getKeyframeManager()->getExpiryDelay(), 240);
        EXPECT_EQ(resources.getNifFileManager()->getExpiryDelay(), 240);
    }

    TEST(ResourceVisualRetention, oneZeroOverrideStillInherits)
    {
        VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resources(&vfs, 120, &encoder.getStatelessEncoder());
        resources.configureVisualCacheRetention(15, 0);
        resources.setExpiryDelay(90);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 15);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 90);
    }

    TEST(ResourceVisualRetention, invalidOrRepeatedConfigurationCannotPartiallyChangePolicy)
    {
        VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resources(&vfs, 120, &encoder.getStatelessEncoder());
        EXPECT_THROW(resources.configureVisualCacheRetention(15, std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
        EXPECT_THROW(resources.configureVisualCacheRetention(-1, 30), std::invalid_argument);
        EXPECT_THROW(resources.configureVisualCacheRetention(15, 3601), std::invalid_argument);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 120);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 120);
        resources.configureVisualCacheRetention(15, 30);
        EXPECT_THROW(resources.configureVisualCacheRetention(1, 1), std::logic_error);
        EXPECT_EQ(resources.getImageManager()->getExpiryDelay(), 15);
        EXPECT_EQ(resources.getSceneManager()->getExpiryDelay(), 30);
    }

    TEST(ResourceVisualRetention, actualBoundedImageExpiryRetiresCandidateAndKeepsControl)
    {
        VFS::Manager vfs;
        CachedImages control(&vfs, 120), candidate(&vfs, 15);
        auto source = image();
        osg::observer_ptr<osg::Image> candidateOwner(source);
        candidate.insert("candidate", source, 1);
        source = nullptr;
        auto original = image();
        osg::observer_ptr<osg::Image> controlOwner(original);
        control.insert("control", original, 1);
        original = nullptr;
        sweep(candidate, 16.1);
        sweep(control, 16.1);
        EXPECT_FALSE(candidateOwner.valid());
        EXPECT_TRUE(controlOwner.valid());
        EXPECT_EQ(candidate.size(), 0);
        EXPECT_EQ(control.size(), 1);
    }

    TEST(ResourceVisualRetention, externalImagePinsRefreshResidenceAndBudgetStillBoundsWork)
    {
        VFS::Manager vfs;
        CachedImages candidate(&vfs, 15);
        auto active = image();
        candidate.insert("live", active, 1);
        sweep(candidate, 100);
        EXPECT_EQ(candidate.get("live"), active.get());
        active = nullptr;
        sweep(candidate, 116, 0);
        EXPECT_EQ(candidate.size(), 1);
        sweep(candidate, 110);
        EXPECT_EQ(candidate.size(), 1);
        sweep(candidate, 116);
        EXPECT_EQ(candidate.size(), 0);
    }

    TEST(ResourceVisualRetention, finalImageDestructionOccursOutsideCacheMutex)
    {
        struct Image final : osg::Image
        {
            std::function<void()> onDestroy;
            ~Image() override { onDestroy(); }
        };
        VFS::Manager vfs;
        CachedImages candidate(&vfs, 15);
        bool destroyed = false;
        osg::ref_ptr<Image> value = new Image;
        value->onDestroy = [&] { destroyed = true; EXPECT_EQ(candidate.size(), 0); };
        candidate.insert("unused", value, 1);
        value = nullptr;
        sweep(candidate, 16.1);
        EXPECT_TRUE(destroyed);
    }

    TEST(ResourceVisualRetention, requiredImageReloadUsesOrdinaryReaderWithIdenticalPixels)
    {
        struct File final : VFS::File
        {
            unsigned opens = 0;
            // Actual uncompressed 2x2 24-bit TGA stream, top-left origin.
            std::string bytes = std::string(18, '\0') + std::string("\x03\x02\x01\x06\x05\x04\x09\x08\x07\x0c\x0b\x0a", 12);
            File() { bytes[2]=2; bytes[12]=2; bytes[14]=2; bytes[16]=24; bytes[17]=0x20; }
            Files::IStreamPtr open() override
            { ++opens; return std::make_unique<Files::IMemStream>(bytes.data(), bytes.size()); }
            std::filesystem::file_time_type getLastModified() const override { return {}; }
            std::string getStem() const override { return "visual-retention"; }
        };
        struct Archive final : VFS::Archive
        {
            File file;
            void listResources(VFS::FileMap& out) override
            { out.emplace(VFS::Path::Normalized("textures/retention.tga"), &file); }
            bool contains(VFS::Path::NormalizedView path) const override
            { return path.value()=="textures/retention.tga"; }
            std::string getDescription() const override { return "retention-ordinary-reader"; }
        };
        ASSERT_NE(osgDB::Registry::instance()->getReaderWriterForExtension("tga"), nullptr);
        VFS::Manager vfs;
        auto archive=std::make_unique<Archive>();
        auto* file=&archive->file;
        vfs.addArchive(std::move(archive));
        vfs.buildIndex();
        CachedImages candidate(&vfs, 15);
        const VFS::Path::NormalizedView path("textures/retention.tga");
        auto first=candidate.getImage(path);
        ASSERT_EQ(first->s(), 2);
        ASSERT_EQ(first->t(), 2);
        const auto color=first->getColor(0,0,0);
        osg::observer_ptr<osg::Image> retired(first);
        first=nullptr;
        sweep(candidate, 1); // Ordinary zero-stamp entry establishes residence.
        sweep(candidate, 16.1);
        EXPECT_FALSE(retired.valid());
        auto reloaded=candidate.getImage(path);
        EXPECT_EQ(reloaded->getColor(0,0,0), color);
        EXPECT_EQ(reloaded->s(), 2);
        EXPECT_EQ(file->opens, 2);
    }
}
