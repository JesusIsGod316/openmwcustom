#ifndef OPENMW_COMPONENTS_TERRAIN_COMPOSITEMAPRENDERER_H
#define OPENMW_COMPONENTS_TERRAIN_COMPOSITEMAPRENDERER_H

#include <osg/Drawable>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <memory>
#include <vector>

namespace Resource
{
    class OpenMWIncrementalCompileOperation;
    class P9DiscretionaryAdmission;
}

namespace osg
{
    class FrameBufferObject;
    class RenderInfo;
    class Texture2D;
    class State;
}

namespace Terrain
{
    struct CompositePreparation;

    class CompositeMap : public osg::Referenced
    {
    public:
        CompositeMap();
        ~CompositeMap();
        std::vector<osg::ref_ptr<osg::Drawable>> mDrawables;
        osg::ref_ptr<osg::Texture2D> mTexture;
        size_t mCompiled;
        // Requiredness survives removal from the queue while the GL thread works.
        // The cull thread must not lose its promotion when a map is in flight.
        std::atomic_bool mRequired{false};
        // Built/advanced on the owning draw context; demand is the only cull
        // mutation. The generation rejects queued producers after cancellation.
        std::shared_ptr<CompositePreparation> mPreparation;
        std::atomic_uint64_t mPreparationGeneration{0};
        unsigned mQueuedFrame = 0;
        bool mQueueFrameKnown = false;
    };

    /**
     * @brief The CompositeMapRenderer is responsible for updating composite map textures in a blocking or non-blocking
     * way.
     */
    class CompositeMapRenderer : public osg::Drawable
    {
    public:
        CompositeMapRenderer();
        ~CompositeMapRenderer();

        const char* className() const override { return "CompositeMapRenderer"; }
        const char* libraryName() const override { return "Terrain"; }

        void drawImplementation(osg::RenderInfo& renderInfo) const override;

        void compile(CompositeMap& compositeMap, osg::RenderInfo& renderInfo) const;
        void configurePreparation(Resource::OpenMWIncrementalCompileOperation* ico);
        bool preparationEnabled() const { return mPreparationEnabled; }
        void releaseGLObjects(osg::State* state = nullptr) const override;
        struct PreparationStats
        {
            std::uint64_t scheduled = 0, invalidated = 0, cancelled = 0, requiredFallbacks = 0;
            std::uint64_t ageProgress = 0, budgetYields = 0;
            std::size_t pendingBytes = 0;
            double chargedMs = 0;
        };
        PreparationStats preparationStats() const;

        // Startup only. The limit is cooperative between drawables, not a
        // preemptive GL deadline; a single driver call can still overrun it.
        void setCooperativeBackgroundCompile(bool enabled) { mCooperativeBackgroundCompile = enabled; }
        std::uint64_t backgroundYields() const { return mBackgroundYields.load(std::memory_order_relaxed); }

        /// Set the available time in seconds for compiling (non-immediate) composite maps each frame
        void setMinimumTimeAvailableForCompile(double time);

        /// If current frame rate is higher than this, the extra time will be set aside to do more compiling
        void setTargetFrameRate(float framerate);

        /// Add a composite map to be rendered
        void addCompositeMap(CompositeMap* map, bool immediate = false);

        /// Mark this composite map to be required for the current frame
        void setImmediate(CompositeMap* map);

        size_t getCompileSetSize() const;

    private:
        using Deadline = std::chrono::steady_clock::time_point;
        struct CompileTelemetry
        {
            std::uint64_t maps = 0;
            std::uint64_t requiredMaps = 0;
            std::uint64_t drawables = 0;
            double fboMs = 0.0;
            double stateMs = 0.0;
            double drawMs = 0.0;
        };
        void compileUntil(CompositeMap& compositeMap, osg::RenderInfo& renderInfo, Deadline deadline,
            CompileTelemetry* telemetry = nullptr) const;
        bool prepare(CompositeMap& map, osg::RenderInfo& info) const;
        bool dependenciesReady(const CompositeMap& map, osg::State& state) const;
        bool preparationSupported(const osg::State& state) const;
        void retirePreparations() const;
        bool mCooperativeBackgroundCompile = false;
        mutable std::atomic_uint64_t mBackgroundYields{0};
        float mTargetFrameRate;
        double mMinimumTimeAvailable;
        mutable osg::Timer mTimer;

        typedef std::set<osg::ref_ptr<CompositeMap>> CompileSet;

        mutable CompileSet mCompileSet;
        mutable CompileSet mImmediateCompileSet;

        mutable std::mutex mMutex;

        osg::ref_ptr<osg::FrameBufferObject> mFBO;
        bool mPreparationEnabled = false;
        osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> mICO;
        std::shared_ptr<Resource::P9DiscretionaryAdmission> mAdmission;
        mutable std::vector<osg::ref_ptr<CompositeMap>> mPendingPreparations;
        mutable PreparationStats mPreparationStats;
        mutable double mBakeEmaMs = 0.25, mBakeRiskMs = 0.25;
        mutable unsigned mBakeSamples = 0;
        mutable double mFboEmaMs = 0.25, mFboRiskMs = 0.25;
        mutable unsigned mFboSamples = 0;
        static constexpr std::size_t sMaxPreparationBytes = 32u * 1024u * 1024u;
        static constexpr std::size_t sMaxPreparationMaps = 8;
    };

}

#endif
