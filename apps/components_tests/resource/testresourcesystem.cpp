#include <components/files/configurationmanager.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/niffilemanager.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/toutf8/toutf8.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/files/memorystream.hpp>
#include <components/resource/templatecoalescing.hpp>
#include <osgDB/Registry>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <thread>
#include <barrier>
#include <array>
#include <exception>
#include <functional>

namespace
{
    using namespace testing;

    TEST(ResourceSceneManager, concurrentExactTemplatePublicationUsesOneFinishedOwner)
    {
        namespace TC = Resource::TemplateCoalescing;
        struct RestoreFlag { bool old = TC::enabled.exchange(true); ~RestoreFlag() { TC::enabled = old; } } restore;
        constexpr unsigned Count = 8;
        std::barrier gate(Count);
        struct File final : VFS::File
        {
            std::function<void()> waitForMisses;
            std::string data = "Group { name CanonicalTemplate }\n";
            Files::IStreamPtr open() override
            {
                if (waitForMisses) waitForMisses();
                return std::make_unique<Files::IMemStream>(data.data(), data.size());
            }
            std::filesystem::file_time_type getLastModified() const override { return {}; }
            std::string getStem() const override { return "canonical"; }
        };
        struct Archive final : VFS::Archive
        {
            File file;
            void listResources(VFS::FileMap& out) override { out.emplace(VFS::Path::Normalized("meshes/canonical.osg"), &file); }
            bool contains(VFS::Path::NormalizedView path) const override { return path.value()=="meshes/canonical.osg"; }
            std::string getDescription() const override { return "exact-template-coalescing-fixture"; }
        };
        ASSERT_NE(osgDB::Registry::instance()->getReaderWriterForExtension("osg"), nullptr);
        VFS::Manager vfs;
        auto archive=std::make_unique<Archive>(); auto* file=&archive->file;
        file->waitForMisses=[&]{gate.arrive_and_wait();};
        vfs.addArchive(std::move(archive)); vfs.buildIndex();
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resources(&vfs,1.0,&encoder.getStatelessEncoder());
        auto* scenes=resources.getSceneManager();
        std::array<osg::ref_ptr<const osg::Node>,Count> results;
        std::array<std::exception_ptr,Count> failures;
        std::vector<std::thread> threads;
        const auto reused=TC::Counters::afterLoadReuses.load()+TC::Counters::publishReuses.load();
        for (unsigned i=0;i<Count;++i)
            threads.emplace_back([&,i] { try { results[i]=scenes->getTemplate(VFS::Path::NormalizedView("meshes/canonical.osg"),false); }
                catch(...) { failures[i]=std::current_exception(); } });
        for(auto& thread:threads) thread.join(); file->waitForMisses={};
        for(unsigned i=0;i<Count;++i)
        { ASSERT_FALSE(failures[i]); ASSERT_TRUE(results[i]); EXPECT_EQ(results[i].get(),results[0].get()); }
        EXPECT_GT(TC::Counters::afterLoadReuses.load()+TC::Counters::publishReuses.load(),reused);
        EXPECT_EQ(results[0]->getName(),"CanonicalTemplate");
        const TC::Epoch current{scenes,1,vfs.getIndexGeneration(),scenes->getShaderManager().assetGeneration()};
        EXPECT_TRUE(TC::matches(results[0],current));
        auto changed=current; ++changed.vfs; EXPECT_FALSE(TC::matches(results[0],changed));
        changed=current; ++changed.shader; EXPECT_FALSE(TC::matches(results[0],changed));
        scenes->clearCache();
        auto replacement=scenes->getTemplate(VFS::Path::NormalizedView("meshes/canonical.osg"),false);
        EXPECT_NE(replacement.get(),results[0].get()); EXPECT_FALSE(TC::matches(replacement,current));

        // Every producer captured the old policy before entering the reader.
        // Change that policy at the barrier, before any conversion resumes.
        // None may adopt a completed template from an incompatible epoch.
        scenes->clearCache();
        std::barrier changedPolicy(Count,[&]() noexcept { scenes->setAutoUseNormalMaps(true); });
        file->waitForMisses=[&]{changedPolicy.arrive_and_wait();};
        threads.clear(); failures={}; results={};
        const auto changedBefore=TC::Counters::changedEpochs.load();
        const auto reuseBefore=TC::Counters::afterLoadReuses.load()+TC::Counters::publishReuses.load();
        for(unsigned i=0;i<Count;++i)
            threads.emplace_back([&,i] { try {results[i]=scenes->getTemplate(VFS::Path::NormalizedView("meshes/canonical.osg"),false);}
                catch(...) {failures[i]=std::current_exception();} });
        for(auto& thread:threads) thread.join(); file->waitForMisses={};
        for(unsigned i=0;i<Count;++i)
        { ASSERT_FALSE(failures[i]); ASSERT_TRUE(results[i]); EXPECT_FALSE(TC::matches(results[i],current)); }
        EXPECT_EQ(TC::Counters::changedEpochs.load()-changedBefore,Count);
        EXPECT_EQ(TC::Counters::afterLoadReuses.load()+TC::Counters::publishReuses.load(),reuseBefore);
    }

    TEST(ResourceNifFileManager, legacyColdRetentionPreservesZeroExpiryAndShortensRetainedProfile)
    {
        const VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::NifFileManager manager(&vfs, &encoder.getStatelessEncoder());
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 0.0);
        manager.setColdRetentionSeconds(30.0);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 0.0);
        manager.setExpiryDelay(1800.0);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 30.0);
        manager.setExpiryDelay(5.0);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 5.0);
    }

    TEST(ResourceNifFileManager, independentColdTierHasExplicitThirtySecondResidence)
    {
        const VFS::Manager vfs;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::NifFileManager manager(&vfs, &encoder.getStatelessEncoder());
        manager.setColdRetentionSeconds(30.0, true);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 30.0);
        manager.setExpiryDelay(1800.0);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 30.0);
        manager.setColdRetentionSeconds(0.0, true);
        EXPECT_DOUBLE_EQ(manager.effectiveExpiryDelay(), 1800.0);
    }

    TEST(ResourceResourceSystem, scenemanager_getinstance_should_be_thread_safe)
    {
        const VFS::Manager vfsManager;
        const ToUTF8::Utf8Encoder encoder(ToUTF8::WINDOWS_1252);
        Resource::ResourceSystem resourceSystem(&vfsManager, 1.0, &encoder.getStatelessEncoder());
        Resource::SceneManager* sceneManager = resourceSystem.getSceneManager();
        const Files::ConfigurationManager configurationManager;
        sceneManager->setShaderPath(configurationManager.getLocalPath() / "resources/shaders");

        auto defines = Shader::getDefaultDefines();

        auto shadowDefines = SceneUtil::ShadowManager::getShadowsDisabledDefines();

        osg::ref_ptr<SceneUtil::LightManager> lightManager
            = new SceneUtil::LightManager(SceneUtil::LightSettings{}, &resourceSystem);
        auto lightDefines = lightManager->getLightDefines();

        for (const auto& define : shadowDefines)
            defines[define.first] = define.second;
        for (const auto& define : lightDefines)
            defines[define.first] = define.second;

        sceneManager->getShaderManager().setGlobalDefines(defines);

        constexpr VFS::Path::NormalizedView noSuchPath("meshes/whatever.nif");
        std::vector<std::thread> threads;

        for (int i = 0; i < 50; ++i)
        {
            threads.emplace_back([=]() { sceneManager->getInstance(noSuchPath); });
        }
        for (std::thread& thread : threads)
            thread.join();
    }
}
