#include "compositemaprenderer.hpp"

#include <osg/FrameBufferObject>
#include <osg/RenderInfo>
#include <osg/Texture2D>

#include <algorithm>

namespace Terrain
{

    CompositeMapRenderer::CompositeMapRenderer()
        : mTargetFrameRate(120)
        , mMinimumTimeAvailable(0.0025)
    {
        setSupportsDisplayList(false);
        setCullingActive(false);

        mFBO = new osg::FrameBufferObject;
    }

    CompositeMapRenderer::~CompositeMapRenderer() = default;

    void CompositeMapRenderer::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        double dt = mTimer.time_s();
        dt = std::min(dt, 0.2);
        mTimer.setStartTick();
        double targetFrameTime = 1.0 / static_cast<double>(mTargetFrameRate);
        double conservativeTimeRatio(0.75);
        double availableTime = std::max((targetFrameTime - dt) * conservativeTimeRatio, mMinimumTimeAvailable);

        const auto frameDeadline = std::chrono::steady_clock::now()
            + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(availableTime));
        std::unique_lock<std::mutex> lock(mMutex);

        if (mImmediateCompileSet.empty() && mCompileSet.empty())
            return;

        while (!mImmediateCompileSet.empty())
        {
            osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
            mImmediateCompileSet.erase(node);

            lock.unlock();
            compile(*node, renderInfo);
            lock.lock();
        }

        const auto deadline = mCooperativeBackgroundCompile ? frameDeadline
            : std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(availableTime));
        while (!mCompileSet.empty() && std::chrono::steady_clock::now() < deadline)
        {
            osg::ref_ptr<CompositeMap> node = *mCompileSet.begin();
            mCompileSet.erase(node);

            lock.unlock();
            compileUntil(*node, renderInfo, mCooperativeBackgroundCompile ? deadline : Deadline::max());
            lock.lock();

            if (node->mCompiled < node->mDrawables.size())
            {
                // We did not compile the map fully.
                // Place it back to queue to continue work in the next time.
                if (mCooperativeBackgroundCompile && node->mRequired.load(std::memory_order_acquire))
                    mImmediateCompileSet.insert(node);
                else
                    mCompileSet.insert(node);
            }
        }
        // A cull-side promotion may have happened after compileUntil decided
        // to yield while the map was temporarily outside both queues. Drain
        // those required maps before this renderer returns to visible drawing.
        if (mCooperativeBackgroundCompile)
        {
            while (!mImmediateCompileSet.empty())
            {
                osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
                mImmediateCompileSet.erase(node);
                lock.unlock();
                compile(*node, renderInfo);
                lock.lock();
            }
        }
        mTimer.setStartTick();
    }

    void CompositeMapRenderer::compile(CompositeMap& compositeMap, osg::RenderInfo& renderInfo) const
    {
        compileUntil(compositeMap, renderInfo, Deadline::max());
    }

    void CompositeMapRenderer::compileUntil(
        CompositeMap& compositeMap, osg::RenderInfo& renderInfo, Deadline deadline) const
    {
        // if there are no more external references we can assume the texture is no longer required
        if (compositeMap.mTexture->referenceCount() <= 1)
        {
            compositeMap.mCompiled = compositeMap.mDrawables.size();
            return;
        }

        osg::Timer timer;
        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();

        if (!mFBO)
            return;

        if (!ext->isFrameBufferObjectSupported)
            return;

        osg::FrameBufferAttachment attach(compositeMap.mTexture);
        mFBO->setAttachment(osg::Camera::COLOR_BUFFER, attach);
        mFBO->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        GLenum status = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);

        if (status != GL_FRAMEBUFFER_COMPLETE_EXT)
        {
            GLuint fboId = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
            ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, fboId);
            OSG_ALWAYS << "Error attaching FBO" << std::endl;
            return;
        }

        // inform State that Texture attribute has changed due to compiling of FBO texture
        // should OSG be doing this on its own?
        state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);

        for (size_t i = compositeMap.mCompiled; i < compositeMap.mDrawables.size(); ++i)
        {
            if (deadline != Deadline::max() && !compositeMap.mRequired.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() >= deadline)
            {
                mBackgroundYields.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            osg::Drawable* drw = compositeMap.mDrawables[i];
            osg::StateSet* stateset = drw->getStateSet();

            if (stateset)
                renderInfo.getState()->pushStateSet(stateset);

            renderInfo.getState()->apply();

            glViewport(0, 0, compositeMap.mTexture->getTextureWidth(), compositeMap.mTexture->getTextureHeight());
            drw->drawImplementation(renderInfo);

            if (stateset)
                renderInfo.getState()->popStateSet();

            ++compositeMap.mCompiled;

            compositeMap.mDrawables[i] = nullptr;
        }
        if (compositeMap.mCompiled == compositeMap.mDrawables.size())
            compositeMap.mDrawables = std::vector<osg::ref_ptr<osg::Drawable>>();

        state.haveAppliedAttribute(osg::StateAttribute::VIEWPORT);

        GLuint fboId = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
        ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, fboId);
    }

    void CompositeMapRenderer::setMinimumTimeAvailableForCompile(double time)
    {
        mMinimumTimeAvailable = time;
    }

    void CompositeMapRenderer::setTargetFrameRate(float framerate)
    {
        mTargetFrameRate = framerate;
    }

    void CompositeMapRenderer::addCompositeMap(CompositeMap* compositeMap, bool immediate)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (immediate)
        {
            compositeMap->mRequired.store(true, std::memory_order_release);
            mCompileSet.erase(compositeMap);
            mImmediateCompileSet.insert(compositeMap);
        }
        else if (!mImmediateCompileSet.contains(compositeMap))
            mCompileSet.insert(compositeMap);
    }

    void CompositeMapRenderer::setImmediate(CompositeMap* compositeMap)
    {
        compositeMap->mRequired.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> lock(mMutex);
        CompileSet::iterator found = mCompileSet.find(compositeMap);
        if (found == mCompileSet.end())
            return;
        else
        {
            mImmediateCompileSet.insert(compositeMap);
            mCompileSet.erase(found);
        }
    }

    size_t CompositeMapRenderer::getCompileSetSize() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        return mCompileSet.size();
    }

    CompositeMap::CompositeMap()
        : mCompiled(0)
    {
    }

    CompositeMap::~CompositeMap() {}

}
