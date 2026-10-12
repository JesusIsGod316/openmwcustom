#include <components/resource/animationdependencies.hpp>
#include <components/resource/keyframemanager.hpp>
#include <components/resource/preloadestimate.hpp>
#include <components/testing/util.hpp>

#include <gtest/gtest.h>

#include <array>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace
{
    using Paths = std::vector<std::string>;

    Paths historicalAdditional(const VFS::Manager& vfs, VFS::Path::NormalizedView model)
    {
        Paths result;
        if (!model.value().starts_with("meshes/"))
            return result;
        std::string path(model.value());
        path.replace(0, 7, "animations/");
        const auto extension = path.find_last_of('.');
        if (extension == std::string::npos)
            return result;
        path.replace(extension, path.size() - extension, "/");
        for (const auto& name : vfs.getRecursiveDirectoryIterator(path))
            if (name.extension() == VFS::Path::ExtensionView("kf"))
                result.push_back(name.value());
        return result;
    }

    TEST(AnimationDependencies, StartupPolicyDoesNotChangeWithLaterEnvironmentMutation)
    {
        const bool initial = Resource::animationDependencyPreloadEnabled();
        const char* original = std::getenv("OPENMW_HITCH_ANIMATION_PRELOAD");
        const bool wasPresent = original != nullptr;
        const std::string value = wasPresent ? original : "";
#ifdef _WIN32
        ASSERT_EQ(_putenv_s("OPENMW_HITCH_ANIMATION_PRELOAD", initial ? "0" : "1"), 0);
        EXPECT_EQ(Resource::animationDependencyPreloadEnabled(), initial);
        ASSERT_EQ(_putenv_s("OPENMW_HITCH_ANIMATION_PRELOAD", wasPresent ? value.c_str() : ""), 0);
#else
        ASSERT_EQ(setenv("OPENMW_HITCH_ANIMATION_PRELOAD", initial ? "0" : "1", 1), 0);
        EXPECT_EQ(Resource::animationDependencyPreloadEnabled(), initial);
        ASSERT_EQ(wasPresent ? setenv("OPENMW_HITCH_ANIMATION_PRELOAD", value.c_str(), 1)
                             : unsetenv("OPENMW_HITCH_ANIMATION_PRELOAD"),
            0);
#endif
    }

    TEST(AnimationDependencies, NpcSourceOrderMatchesHistoricalBindingIncludingDuplicates)
    {
        for (bool werewolf : { false, true })
            for (bool firstPerson : { false, true })
                for (bool beast : { false, true })
                    for (bool argonian : { false, true })
                        for (bool custom : { false, true })
                            for (bool sameSkeleton : { false, true })
                            {
                                const std::string base = werewolf ? ""
                                    : firstPerson                 ? "meshes/xbaseanim1st.nif"
                                                                  : "meshes/xbaseanim.nif";
                                const std::string skeleton = sameSkeleton ? base : "meshes/female-or-beast.nif";
                                const std::string model = custom ? base : skeleton;
                                const bool customModel = !firstPerson && !werewolf && custom;
                                const bool swim = !firstPerson && !werewolf && beast && argonian;
                                EXPECT_EQ(
                                    Resource::npcAnimationUsesCustomModel(firstPerson, werewolf, custom), customModel);
                                EXPECT_EQ(
                                    Resource::npcAnimationUsesArgonianSwim(firstPerson, werewolf, beast, argonian),
                                    swim);
                                Paths expected;
                                if (!base.empty())
                                    expected.push_back(base);
                                if (skeleton != base)
                                    expected.push_back(skeleton);
                                if (customModel)
                                    expected.push_back(model);
                                if (swim)
                                    expected.push_back("meshes/xargonianswimkna.nif");
                                Paths actual;
                                ASSERT_TRUE(Resource::visitNpcAnimationSources(base, skeleton, model, customModel, swim,
                                    "meshes/xargonianswimkna.nif", [&](std::string_view path) {
                                        actual.emplace_back(path);
                                        return true;
                                    }));
                                EXPECT_EQ(actual, expected);
                            }
    }

    TEST(AnimationDependencies, KeyframeConversionMatchesDemandForNifKfAndOtherFormats)
    {
        for (const char* source : { "meshes/actor.nif", "meshes/actor.kf", "meshes/actor.dae", "meshes/actor",
                 "meshes/a.b/actor", "meshes/.nif" })
        {
            VFS::Path::Normalized historical(source);
            if (historical.extension() == VFS::Path::ExtensionView("nif"))
                historical.changeExtension(VFS::Path::ExtensionView("kf"));
            EXPECT_EQ(Resource::animationKeyframePath(VFS::Path::NormalizedView(source)), historical);
        }
    }

    TEST(AnimationDependencies, RecursiveDirectoryDiscoveryMatchesHistoricalRuntimeOrder)
    {
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("animations/actor/a.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/nested/b.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/a.txt"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/z.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/actorish/wrong.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/a/odd.kf"), nullptr },
        });
        for (const char* source : { "meshes/actor.kf", "meshes/actor.nif", "meshes/actor.dae", "meshes/actor",
                 "other/actor.kf", "meshes/a.b/actor" })
        {
            Paths actual;
            EXPECT_TRUE(Resource::visitAdditionalAnimationSources(
                *vfs, VFS::Path::NormalizedView(source), [&](VFS::Path::NormalizedView path) {
                    actual.emplace_back(path.value());
                    return true;
                }));
            EXPECT_EQ(actual, historicalAdditional(*vfs, VFS::Path::NormalizedView(source)));
        }
    }

    struct MergeArchive : VFS::Archive
    {
        VFS::FileMap files;
        explicit MergeArchive(VFS::FileMap values)
            : files(std::move(values))
        {
        }
        void listResources(VFS::FileMap& out) override
        {
            for (const auto& [path, file] : files)
                out.insert_or_assign(path, file);
        }
        bool contains(VFS::Path::NormalizedView path) const override { return files.contains(path); }
        std::string getDescription() const override { return "animation test archive"; }
    };

    TEST(AnimationDependencies, PreloadUsesWinningVfsSourceAndNewGenerationAfterReset)
    {
        TestingOpenMW::VFSTestFile original("old"), replacement("mod override"), reloaded("hot replacement");
        const VFS::Path::Normalized path("animations/actor/extra.kf");
        VFS::Manager vfs;
        vfs.addArchive(std::make_unique<MergeArchive>(VFS::FileMap{ { path, &original } }));
        vfs.addArchive(std::make_unique<MergeArchive>(VFS::FileMap{ { path, &replacement } }));
        vfs.buildIndex();
        const auto generation = vfs.getIndexGeneration();
        Paths found;
        Resource::visitAnimationDependencies(
            vfs, VFS::Path::NormalizedView("meshes/actor.nif"), true, [&](VFS::Path::NormalizedView name) {
                if (vfs.exists(name))
                {
                    auto stream = vfs.get(name);
                    found.emplace_back(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>());
                }
                return true;
            });
        EXPECT_EQ(found, Paths{ "mod override" });
        vfs.reset();
        vfs.addArchive(std::make_unique<MergeArchive>(VFS::FileMap{ { path, &reloaded } }));
        vfs.buildIndex();
        EXPECT_FALSE(
            Resource::animationDependencyPreparationAllowed(false, true, generation, vfs.getIndexGeneration(), 0, 0));
        EXPECT_TRUE(Resource::animationDependencyPreparationAllowed(
            false, true, vfs.getIndexGeneration(), vfs.getIndexGeneration(), 0, 0));
        auto stream = vfs.get(path);
        EXPECT_EQ(
            std::string(std::istreambuf_iterator<char>(*stream), std::istreambuf_iterator<char>()), "hot replacement");
    }

    TEST(AnimationDependencies, OptionalVisitorStopsBeforeFurtherWorkOnCancelPressureOrCountCap)
    {
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("animations/actor/a.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/b.kf"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/c.kf"), nullptr },
        });
        for (unsigned reason = 0; reason < 4; ++reason)
        {
            Paths completed;
            bool stopped = false;
            EXPECT_FALSE(Resource::visitAnimationDependencies(
                *vfs, VFS::Path::NormalizedView("meshes/actor.nif"), true, [&](VFS::Path::NormalizedView path) {
                    const bool stop = completed.size() == 2;
                    const bool allowed = Resource::animationDependencyPreparationAllowed(reason == 0 && stop,
                        !(reason == 1 && stop), vfs->getIndexGeneration(), vfs->getIndexGeneration(),
                        reason == 2 && stop ? Resource::MaximumAnimationPreloadDependencies : completed.size(),
                        reason == 3 && stop ? Resource::MaximumAnimationPreloadEstimate : 0);
                    if (!allowed)
                    {
                        stopped = true;
                        return false;
                    }
                    completed.emplace_back(path.value());
                    return true;
                }));
            EXPECT_TRUE(stopped);
            EXPECT_EQ(completed, (Paths{ "meshes/actor.kf", "animations/actor/a.kf" }));
        }
        Paths primaryOnly;
        EXPECT_TRUE(Resource::visitAnimationDependencies(
            *vfs, VFS::Path::NormalizedView("meshes/actor.nif"), false, [&](VFS::Path::NormalizedView path) {
                primaryOnly.emplace_back(path.value());
                return true;
            }));
        EXPECT_EQ(primaryOnly, Paths{ "meshes/actor.kf" });
    }

    TEST(AnimationDependencies, ScanningStopsBetweenNonKeyframeEntries)
    {
        const auto vfs = TestingOpenMW::createTestVFS({
            { VFS::Path::NormalizedView("animations/actor/a.txt"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/b.txt"), nullptr },
            { VFS::Path::NormalizedView("animations/actor/z.kf"), nullptr },
        });
        unsigned inspected = 0;
        Paths prepared;
        EXPECT_FALSE(Resource::visitAnimationDependencies(
            *vfs, VFS::Path::NormalizedView("meshes/actor.nif"), true,
            [&](VFS::Path::NormalizedView path) {
                prepared.emplace_back(path.value());
                return true;
            },
            [&] { return ++inspected <= 2; }));
        EXPECT_EQ(prepared, Paths{ "meshes/actor.kf" });
        EXPECT_EQ(inspected, 3u);
    }

    struct ReadCounters
    {
        unsigned opens = 0;
        std::uint64_t bytes = 0;
    };
    class CountedBuffer : public std::stringbuf
    {
        ReadCounters& mCounters;

    public:
        CountedBuffer(const std::string& value, ReadCounters& counters)
            : std::stringbuf(value, std::ios::in)
            , mCounters(counters)
        {
        }
        std::streamsize xsgetn(char* out, std::streamsize size) override
        {
            const auto result = std::stringbuf::xsgetn(out, size);
            mCounters.bytes += static_cast<std::uint64_t>(result);
            return result;
        }
    };
    class CountedStream : public std::istream
    {
        CountedBuffer mBuffer;

    public:
        CountedStream(const std::string& value, ReadCounters& counters)
            : std::istream(nullptr)
            , mBuffer(value, counters)
        {
            rdbuf(&mBuffer);
        }
    };
    class CountedFile : public VFS::File
    {
        std::string mValue;
        ReadCounters& mCounters;

    public:
        CountedFile(std::string value, ReadCounters& counters)
            : mValue(std::move(value))
            , mCounters(counters)
        {
        }
        Files::IStreamPtr open() override
        {
            ++mCounters.opens;
            return std::make_unique<CountedStream>(mValue, mCounters);
        }
        std::filesystem::file_time_type getLastModified() const override { return {}; }
        std::string getStem() const override { return "empty"; }
    };

    TEST(AnimationDependenciesCandidate, BudgetDenialPrecedesHashParseAndDemandFallbackStillWorks)
    {
        if (!Resource::animationDependencyPreloadEnabled())
            GTEST_SKIP() << "Run with OPENMW_HITCH_ANIMATION_PRELOAD=1 to test the candidate resource path";
        std::string nif = "NetImmerse File Format, Version 4.0.0.2\n";
        const auto word = [&](std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i)
                nif.push_back(static_cast<char>(value >> (8 * i)));
        };
        word(0x04000002);
        word(0);
        word(0); // supported empty NIF/KF, zero records/roots
        ReadCounters counters;
        CountedFile file(nif, counters);
        const auto vfs = TestingOpenMW::createTestVFS({ { VFS::Path::NormalizedView("meshes/empty.kf"), &file } });
        Resource::KeyframeManager manager(vfs.get(), nullptr, 300, nullptr);
        Resource::OpenGlPressureSample sample;
        sample.generation = 1;
        sample.growthWatermarkValid = true;
        sample.memory.physicalValid = true;
        sample.memory.physicalTotal = 32ull * 1024 * 1024 * 1024;
        sample.memory.physicalAvailable = 16ull * 1024 * 1024 * 1024;
        sample.memory.commitValid = true;
        sample.memory.commitAvailable = sample.memory.physicalAvailable;
        sample.decision.state = Resource::OpenGlPressure::Normal;
        sample.decision.admissionsPerSample = 4;
        Resource::SpeculativeBudget budget;
        auto query = [](void* state) { return *static_cast<Resource::OpenGlPressureSample*>(state); };
        {
            Resource::SpeculativeScope scope(&budget, query, &sample);
            EXPECT_THROW(manager.get(VFS::Path::NormalizedView("meshes/empty.kf"), 1), Resource::SpeculativeDeferred);
            EXPECT_EQ(counters.opens, 1u);
            EXPECT_EQ(counters.bytes, 0u);
            EXPECT_EQ(budget.stats().futureBytes, 0u);
            sample.decision.state = Resource::OpenGlPressure::Critical;
            EXPECT_THROW(manager.get(VFS::Path::NormalizedView("meshes/empty.kf")), Resource::SpeculativeDeferred);
            EXPECT_EQ(counters.bytes, 0u);
            EXPECT_EQ(budget.stats().futureBytes, 0u);
        }
        const auto demand = manager.get(VFS::Path::NormalizedView("meshes/empty.kf"));
        ASSERT_TRUE(demand.valid());
        EXPECT_GT(counters.bytes, 0u);
        const auto opens = counters.opens;
        {
            // Cache reuse remains available under pressure; no speculative read.
            Resource::SpeculativeScope scope(&budget, query, &sample);
            EXPECT_EQ(manager.get(VFS::Path::NormalizedView("meshes/empty.kf")), demand);
            EXPECT_EQ(counters.opens, opens);
        }
        manager.clearCache();
        manager.setSpeculativeBudget(&budget);
        sample.decision.state = Resource::OpenGlPressure::Normal;
        {
            Resource::SpeculativeScope scope(&budget, query, &sample);
            const auto prepared
                = manager.get(VFS::Path::NormalizedView("meshes/empty.kf"), Resource::MaximumAnimationPreloadEstimate);
            ASSERT_TRUE(prepared.valid());
            EXPECT_EQ(scope.retainedEstimate(), 4 * Resource::SpeculativeBudget::MiB);
            EXPECT_EQ(budget.stats().futureBytes, 0u);
            EXPECT_EQ(budget.stats().estimatedOwnerBytes, 4 * Resource::SpeculativeBudget::MiB);
            EXPECT_EQ(manager.get(VFS::Path::NormalizedView("meshes/empty.kf")), prepared);
        }
        manager.clearCache();
        EXPECT_EQ(budget.stats().estimatedOwnerBytes, 0u);
        EXPECT_EQ(budget.stats().futureBytes, 0u);
    }
}
