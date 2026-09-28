#ifndef OPENMW_COMPONENTS_TERRAIN_COMPOSITEMAPRENDERER_H
#define OPENMW_COMPONENTS_TERRAIN_COMPOSITEMAPRENDERER_H

#include <osg/Drawable>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>

namespace osg
{
    class FrameBufferObject;
    class RenderInfo;
    class Texture2D;
}

namespace Terrain
{

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

        void drawImplementation(osg::RenderInfo& renderInfo) const override;

        void compile(CompositeMap& compositeMap, osg::RenderInfo& renderInfo) const;

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
        void compileUntil(CompositeMap& compositeMap, osg::RenderInfo& renderInfo, Deadline deadline) const;
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
    };

}

#endif
