#include "compositemaprenderer.hpp"

#include <osg/FrameBufferObject>
#include <osg/RenderInfo>
#include <osg/Texture2D>
#include <osg/Program>
#include <osg/FrameStamp>
#include <osg/observer_ptr>

#include <components/debug/v3diagnostics.hpp>
#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/preparedterraintexture.hpp>
#include <components/resource/v321classifiedcompileset.hpp>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace Terrain
{
    struct CompositePreparation
    {
        struct Dependency
        {
            osg::ref_ptr<Resource::PreparedTerrainTexture> texture;
            osg::ref_ptr<osg::Program> program;
            osg::ref_ptr<osg::StateSet> pass;
            osg::observer_ptr<osg::Drawable> drawable;
            unsigned unit = 0;
            osg::observer_ptr<osg::Program::PerContextProgram> submittedProgram;
            GLuint programName = 0;
        };
        std::vector<Dependency> dependencies;
        osg::ref_ptr<osg::FrameBufferObject> fbo = new osg::FrameBufferObject;
        osg::observer_ptr<osg::Texture2D> target;
        osg::observer_ptr<osg::GraphicsContext> context;
        std::size_t bytes = 0;
        std::uint64_t generation = 0;
        std::shared_ptr<Resource::V321ResourceCompileLifetime> lifetime
            = std::make_shared<Resource::V321ResourceCompileLifetime>();
        bool completed = false, failed = false;
        GLuint fboName = 0;
    };

    namespace
    {
        bool retainedDependency(const CompositePreparation::Dependency& dep)
        {
            if (!dep.pass || !dep.drawable.valid() || dep.drawable->getStateSet() != dep.pass.get())
                return false;
            if (dep.texture)
                return dep.pass->getTextureAttribute(dep.unit, osg::StateAttribute::TEXTURE) == dep.texture.get();
            return dep.pass->getAttribute(osg::StateAttribute::PROGRAM) == dep.program.get();
        }

        class CompositeDependencyCompileOp final : public Resource::OpenMWCompositeCompileOp
        {
        public:
            CompositeDependencyCompileOp(CompositeMap* map, std::shared_ptr<CompositePreparation> preparation,
                std::size_t dependency, Kind kind, std::size_t bytes)
                : mMap(map), mPreparation(std::move(preparation)), mDependency(dependency), mKind(kind), mBytes(bytes) {}
            Kind kind() const override { return mKind; }
            std::size_t resourceBytes() const override { return mBytes; }
            bool required() const override { return mMap->mRequired.load(std::memory_order_acquire); }
            bool cancelled() const override
            {
                return mPreparation->lifetime->cancelled()
                    || mMap->mPreparationGeneration.load(std::memory_order_acquire) != mPreparation->generation
                    || mMap->mTexture.get() != mPreparation->target.get();
            }
            bool reusable(const osgUtil::IncrementalCompileOperation::CompileInfo& info) const override
            {
                if (cancelled())
                    return true;
                // CompileInfo is const for prediction, but its State is the
                // owning draw context's mutable stack. This balanced push/pop
                // selects only this bounded producer pass's OSG variant; it
                // applies no GL state and creates no resource cache.
                auto& state = const_cast<osg::State&>(*info.getState());
                if (mPreparation->context.get() != state.getGraphicsContext())
                    return false;
                if (mDependency < mPreparation->dependencies.size())
                {
                    const auto& dep = mPreparation->dependencies[mDependency];
                    if (!retainedDependency(dep))
                        return false;
                    if (dep.texture)
                        return !info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                            && dep.texture->preparationUnchanged(state);
                    state.pushStateSet(dep.pass);
                    auto* program = dep.program->getPCP(state);
                    const bool ready = program && program->isLinked() && !program->needsLink();
                    state.popStateSet();
                    return ready;
                }
                const auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(mMap->mTexture.get());
                return mKind == Kind::Texture && target
                    && !info.incrementalCompileOperation->getForceTextureDownloadGeometry()
                    && target->preparationUnchanged(state);
            }
            double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override
            {
                // A conservative initial estimate; existing ICO size tiers and
                // measured EMA/risk replace it after actual submissions.
                return mKind == Kind::Program ? .001 : .00025;
            }
            bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
            {
                if (info.incrementalCompileOperation->getContextSet().size() != 1)
                {
                    mPreparation->lifetime->cancel();
                    return true;
                }
                if (cancelled() || mPreparation->context.get() != info.getState()->getGraphicsContext())
                    return true;
                auto& state = *info.getState();
                if (mDependency < mPreparation->dependencies.size())
                {
                    auto& dep = mPreparation->dependencies[mDependency];
                    if (!retainedDependency(dep))
                    {
                        mPreparation->lifetime->cancel();
                        return true;
                    }
                    if (dep.texture)
                    {
                        osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                            = new Resource::PreparedTerrainTextureCompileOp(dep.texture);
                        return op->compile(info);
                    }
                    // Push the producer's actual pass for the same OSG define
                    // variant. Applying the whole State here would also upload
                    // unrelated textures before their individual admission.
                    state.pushStateSet(dep.pass);
                    dep.program->compileGLObjects(state);
                    auto* program = dep.program->getPCP(state);
                    if (program && program->isLinked() && !program->needsLink())
                    {
                        dep.submittedProgram = program;
                        dep.programName = program->getHandle();
                    }
                    else
                        mPreparation->failed = true;
                    state.popStateSet();
                    return true;
                }
                auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(mMap->mTexture.get());
                if (mKind == Kind::Texture)
                {
                    if (target)
                    {
                        osg::ref_ptr<Resource::PreparedTerrainTextureCompileOp> op
                            = new Resource::PreparedTerrainTextureCompileOp(target);
                        return op->compile(info);
                    }
                    mPreparation->failed = true;
                    return true;
                }
                auto* ext = state.get<osg::GLExtensions>();
                if (!ext->isFrameBufferObjectSupported || !target || !target->preparationUnchanged(state))
                    mPreparation->failed = true;
                else
                {
                    mPreparation->fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(target));
                    mPreparation->fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);
                    mPreparation->failed = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT;
                    mPreparation->fboName = mPreparation->fbo->getHandle(state.getContextID());
                    const GLuint original = state.getGraphicsContext() ? state.getGraphicsContext()->getDefaultFboId() : 0;
                    ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT, original);
                    state.haveAppliedTextureAttribute(state.getActiveTextureUnit(), osg::StateAttribute::TEXTURE);
                }
                mPreparation->completed = true;
                return true;
            }

        private:
            osg::ref_ptr<CompositeMap> mMap;
            std::shared_ptr<CompositePreparation> mPreparation;
            std::size_t mDependency;
            Kind mKind;
            std::size_t mBytes;
        };

        struct CompositeCompileCompleted final : osgUtil::IncrementalCompileOperation::CompileCompletedCallback
        {
            explicit CompositeCompileCompleted(std::shared_ptr<Resource::V321ResourceCompileLifetime> lifetime)
                : mLifetime(std::move(lifetime)) {}
            // There is no duplicate scene publication/merge queue for these
            // producer-owned dependencies.
            bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override
            {
                mLifetime->complete();
                return true;
            }
            std::shared_ptr<Resource::V321ResourceCompileLifetime> mLifetime;
        };
    }

    void CompositeMapRenderer::configurePreparation(Resource::OpenMWIncrementalCompileOperation* ico)
    {
        mICO = ico;
        mAdmission = ico ? ico->compositeAdmission() : nullptr;
        mPreparationEnabled = bool(mAdmission);
    }

    bool CompositeMapRenderer::preparationSupported(const osg::State& state) const
    {
        return mPreparationEnabled && mICO && mICO->getContextSet().size() == 1
            && state.getGraphicsContext() && *mICO->getContextSet().begin() == state.getGraphicsContext();
    }

    CompositeMapRenderer::PreparationStats CompositeMapRenderer::preparationStats() const
    {
        auto result = mPreparationStats;
        result.chargedMs = mAdmission ? mAdmission->chargedMs() : 0;
        return result;
    }

    void CompositeMapRenderer::retirePreparations() const
    {
        for (auto it = mPendingPreparations.begin(); it != mPendingPreparations.end();)
        {
            auto& map = **it;
            auto& preparation = map.mPreparation;
            // Only this map's own target reference is included in the descriptor.
            // Loss of all external consumers cancels speculative queued work.
            if (preparation && map.mTexture.get() == preparation->target.get()
                && map.mTexture->referenceCount() <= (preparation->fbo->hasAttachment(osg::Camera::COLOR_BUFFER) ? 2 : 1))
            {
                preparation->lifetime->cancel();
                ++mPreparationStats.cancelled;
            }
            if (!preparation || preparation->lifetime->cancelled()
                || map.mDrawables.empty())
            {
                if (preparation)
                    mPreparationStats.pendingBytes -= preparation->bytes;
                if (map.mDrawables.empty() && preparation)
                {
                    // A completed map keeps its output, not a hidden residency
                    // cache of every source layer/program/preparation FBO.
                    preparation->lifetime->cancel();
                    map.mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                    map.mPreparation.reset();
                }
                it = mPendingPreparations.erase(it);
            }
            else
                ++it;
        }
    }

    bool CompositeMapRenderer::dependenciesReady(const CompositeMap& map, osg::State& state) const
    {
        const auto& preparation = map.mPreparation;
        if (!preparation || !preparation->completed || preparation->failed
            || preparation->lifetime->cancelled()
            || preparation->generation != map.mPreparationGeneration.load(std::memory_order_acquire)
            || preparation->context.get() != state.getGraphicsContext()
            || preparation->target.get() != map.mTexture.get()
            || preparation->fboName == 0 || preparation->fbo->getHandle(state.getContextID()) != preparation->fboName)
            return false;
        auto* target = dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get());
        if (!target || !target->preparationUnchanged(state))
            return false;
        for (const auto& dep : preparation->dependencies)
        {
            if (!retainedDependency(dep))
                return false;
            if (dep.texture)
            {
                if (!dep.texture->preparationUnchanged(state))
                    return false;
            }
            else
            {
                state.pushStateSet(dep.pass);
                auto* pcp = dep.program->getPCP(state);
                const bool ready = pcp && pcp == dep.submittedProgram.get() && pcp->getHandle() == dep.programName
                    && pcp->isLinked() && !pcp->needsLink();
                state.popStateSet();
                if (!ready)
                    return false;
            }
        }
        return true;
    }

    bool CompositeMapRenderer::prepare(CompositeMap& map, osg::RenderInfo& info) const
    {
        auto& state = *info.getState();
        if (dependenciesReady(map, state))
            return true;
        if (map.mPreparation)
        {
            if (map.mPreparation->completed && map.mPreparation->failed
                && map.mPreparation->context.get() == state.getGraphicsContext()
                && map.mPreparation->target.get() == map.mTexture.get())
                return true; // preserve complete old rendering after preparation failure
            if (!map.mPreparation->completed && !map.mPreparation->lifetime->cancelled()
                && map.mPreparation->target.get() == map.mTexture.get()
                && map.mPreparation->context.get() == state.getGraphicsContext())
                return false;
            map.mPreparation->lifetime->cancel();
            ++mPreparationStats.invalidated;
            retirePreparations();
            map.mPreparation.reset();
            map.mCompiled = 0; // retained candidate layers rebuild the complete new revision
        }
        // The readiness design is deliberately limited to one actual graphics
        // context. Unsupported contexts/resources use the exact original bake.
        if (!mICO || mICO->getContextSet().size() != 1
            || !mICO->getContextSet().contains(state.getGraphicsContext())
            || !dynamic_cast<Resource::PreparedTerrainTexture*>(map.mTexture.get()))
            return true;
        if (mPendingPreparations.size() >= sMaxPreparationMaps)
            return false;

        auto preparation = std::make_shared<CompositePreparation>();
        preparation->target = map.mTexture;
        preparation->context = state.getGraphicsContext();
        preparation->generation = map.mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
        preparation->bytes = static_cast<std::size_t>(map.mTexture->getTextureWidth())
            * static_cast<std::size_t>(map.mTexture->getTextureHeight()) * 4;
        std::unordered_set<const osg::Texture*> seen;
        for (std::size_t i = map.mCompiled; i < map.mDrawables.size(); ++i)
        {
            auto* pass = map.mDrawables[i] ? map.mDrawables[i]->getStateSet() : nullptr;
            if (!pass)
                continue;
            const auto& attributes = pass->getTextureAttributeList();
            for (unsigned unit = 0; unit < attributes.size(); ++unit)
            {
                auto* texture = dynamic_cast<osg::Texture*>(pass->getTextureAttribute(unit, osg::StateAttribute::TEXTURE));
                if (!texture)
                    continue;
                auto* prepared = dynamic_cast<Resource::PreparedTerrainTexture*>(texture);
                if (!prepared)
                    return true;
                if (prepared->getDataVariance() == osg::Object::DYNAMIC
                    || (prepared->getImage() && (prepared->getImage()->getDataVariance() == osg::Object::DYNAMIC
                        || prepared->getImage()->requiresUpdateCall())))
                    return true;
                CompositePreparation::Dependency dep;
                dep.texture = prepared;
                dep.pass = pass;
                dep.drawable = map.mDrawables[i];
                dep.unit = unit;
                preparation->dependencies.push_back(std::move(dep));
                if (preparation->dependencies.size() > 512)
                    return true;
                if (seen.insert(texture).second && prepared->getImage())
                    preparation->bytes += prepared->getImage()->getTotalDataSize();
            }
            if (auto* program = dynamic_cast<osg::Program*>(pass->getAttribute(osg::StateAttribute::PROGRAM)))
            {
                CompositePreparation::Dependency dep;
                dep.program = program;
                dep.pass = pass;
                dep.drawable = map.mDrawables[i];
                preparation->dependencies.push_back(std::move(dep));
                if (preparation->dependencies.size() > 512)
                    return true;
            }
        }
        if (preparation->bytes > sMaxPreparationBytes)
            return true; // no speculative oversized allocation; demand keeps exact fallback
        if (preparation->bytes > sMaxPreparationBytes - mPreparationStats.pendingBytes)
            return false;
        osg::ref_ptr<Resource::V321ClassifiedCompileSet> set = new Resource::V321ClassifiedCompileSet(
            preparation->lifetime, Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        auto& list = set->_compileMap[state.getGraphicsContext()];
        for (std::size_t i = 0; i < preparation->dependencies.size(); ++i)
        {
            const auto& dep = preparation->dependencies[i];
            const auto kind = dep.texture ? Resource::OpenMWCompositeCompileOp::Kind::Texture
                : Resource::OpenMWCompositeCompileOp::Kind::Program;
            list.add(new CompositeDependencyCompileOp(&map, preparation, i, kind,
                dep.texture && dep.texture->getImage() ? dep.texture->getImage()->getTotalDataSize() : 0));
        }
        const auto end = preparation->dependencies.size();
        list.add(new CompositeDependencyCompileOp(&map, preparation, end,
            Resource::OpenMWCompositeCompileOp::Kind::Texture,
            static_cast<std::size_t>(map.mTexture->getTextureWidth()) * map.mTexture->getTextureHeight() * 4));
        list.add(new CompositeDependencyCompileOp(&map, preparation, end,
            Resource::OpenMWCompositeCompileOp::Kind::Target, 0));
        ++set->_numberCompileListsToCompile;
        set->_compileCompletedCallback = new CompositeCompileCompleted(preparation->lifetime);
        map.mPreparation = preparation;
        mPendingPreparations.push_back(&map);
        mPreparationStats.pendingBytes += preparation->bytes;
        ++mPreparationStats.scheduled;
        mICO->add(set, false);
        return false;
    }

    CompositeMapRenderer::CompositeMapRenderer()
        : mTargetFrameRate(120)
        , mMinimumTimeAvailable(0.0025)
    {
        setSupportsDisplayList(false);
        setCullingActive(false);
        setName("TerrainCompositeMapRenderer");

        mFBO = new osg::FrameBufferObject;
    }

    CompositeMapRenderer::~CompositeMapRenderer()
    {
        for (const auto& map : mPendingPreparations)
            if (map->mPreparation)
            {
                map->mPreparation->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
            }
    }

    void CompositeMapRenderer::releaseGLObjects(osg::State* state) const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const auto& map : mPendingPreparations)
            if (map->mPreparation)
            {
                map->mPreparation->lifetime->cancel();
                map->mPreparationGeneration.fetch_add(1, std::memory_order_acq_rel);
                map->mPreparation->fbo->releaseGLObjects(state);
                map->mCompiled = 0;
            }
        mFBO->releaseGLObjects(state);
        osg::Drawable::releaseGLObjects(state);
    }

    void CompositeMapRenderer::drawImplementation(osg::RenderInfo& renderInfo) const
    {
        using Clock = Debug::V3Diagnostics::Clock;
        auto& telemetryWriter = Debug::V3Diagnostics::p9CompositeWriter();
        const bool telemetryEnabled = telemetryWriter.enabled();
        const auto telemetryStart = telemetryEnabled ? Clock::now() : Clock::time_point{};
        const std::uint64_t yieldsStart = mBackgroundYields.load(std::memory_order_relaxed);
        CompileTelemetry telemetry;
        const bool preparationActive = preparationSupported(*renderInfo.getState());
        const osg::FrameStamp* ownerStamp = renderInfo.getState()->getFrameStamp();
        const unsigned ownerFrame = ownerStamp ? ownerStamp->getFrameNumber() : 0;
        if (preparationActive)
        {
            const double headroom = std::max(0.0, mICO->compositeTargetFrameMs()
                - renderInfo.getState()->getGraphicsContext()->getTimeSinceLastClear() * 1000.0);
            mAdmission->begin(renderInfo.getState()->getGraphicsContext(), ownerFrame,
                std::min(mICO->compositeBudgetMs(), headroom * mICO->compositeHeadroomRatio()));
            retirePreparations();
        }

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

        const std::size_t immediateStart = mImmediateCompileSet.size();
        const std::size_t queuedStart = mCompileSet.size();
        if (preparationActive)
            for (const auto& map : mCompileSet)
                if (!map->mQueueFrameKnown)
                {
                    map->mQueuedFrame = ownerFrame;
                    map->mQueueFrameKnown = true;
                }

        while (!mImmediateCompileSet.empty())
        {
            osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
            mImmediateCompileSet.erase(node);

            lock.unlock();
            compileUntil(*node, renderInfo, Deadline::max(), telemetryEnabled ? &telemetry : nullptr);
            lock.lock();
        }

        const auto deadline = mCooperativeBackgroundCompile ? frameDeadline
            : std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(availableTime));
        std::unordered_set<CompositeMap*> attempted;
        while (!mCompileSet.empty() && std::chrono::steady_clock::now() < deadline)
        {
            auto selected = mCompileSet.begin();
            if (preparationActive)
            {
                selected = mCompileSet.end();
                for (auto it = mCompileSet.begin(); it != mCompileSet.end(); ++it)
                    if (!attempted.contains(it->get()) && (selected == mCompileSet.end()
                        || (*it)->mQueuedFrame < (*selected)->mQueuedFrame))
                        selected = it;
                if (selected == mCompileSet.end())
                    break;
            }
            osg::ref_ptr<CompositeMap> node = *selected;
            attempted.insert(node.get());
            mCompileSet.erase(node);

            lock.unlock();
            compileUntil(*node, renderInfo, mCooperativeBackgroundCompile ? deadline : Deadline::max(),
                telemetryEnabled ? &telemetry : nullptr);
            lock.lock();

            if (node->mCompiled < node->mDrawables.size())
            {
                // We did not compile the map fully.
                // Place it back to queue to continue work in the next time.
                if ((mCooperativeBackgroundCompile || preparationActive) && node->mRequired.load(std::memory_order_acquire))
                    mImmediateCompileSet.insert(node);
                else
                    mCompileSet.insert(node);
            }
        }
        // A cull-side promotion may have happened after compileUntil decided
        // to yield while the map was temporarily outside both queues. Drain
        // those required maps before this renderer returns to visible drawing.
        if (mCooperativeBackgroundCompile || preparationActive)
        {
            while (!mImmediateCompileSet.empty())
            {
                osg::ref_ptr<CompositeMap> node = *mImmediateCompileSet.begin();
                mImmediateCompileSet.erase(node);
                lock.unlock();
                compileUntil(*node, renderInfo, Deadline::max(), telemetryEnabled ? &telemetry : nullptr);
                lock.lock();
            }
        }

        const std::size_t immediateEnd = mImmediateCompileSet.size();
        const std::size_t queuedEnd = mCompileSet.size();
        unsigned oldestAge = 0;
        if (preparationActive)
            for (const auto& map : mCompileSet)
                if (map->mQueueFrameKnown && ownerFrame >= map->mQueuedFrame)
                    oldestAge = std::max(oldestAge, ownerFrame - map->mQueuedFrame);
        lock.unlock();
        if (preparationActive)
            retirePreparations();
        mTimer.setStartTick();

        if (telemetryEnabled)
        {
            const double totalMs = Debug::V3Diagnostics::elapsedMs(telemetryStart);
            if (totalMs >= 0.20 || telemetry.maps != 0)
            {
                osg::State* state = renderInfo.getState();
                const osg::FrameStamp* stamp = state ? state->getFrameStamp() : nullptr;
                const unsigned frame = stamp ? stamp->getFrameNumber() : 0;
                const unsigned context = state ? state->getContextID() : 0;
                const std::uint64_t yieldsEnd = mBackgroundYields.load(std::memory_order_relaxed);
                std::ostringstream row;
                row << frame << ',' << Debug::V3Diagnostics::epochMs() << ',' << context << ','
                    << std::fixed << std::setprecision(4) << totalMs << ',' << availableTime * 1000.0 << ','
                    << immediateStart << ',' << queuedStart << ',' << telemetry.maps << ','
                    << telemetry.requiredMaps << ',' << telemetry.drawables << ',' << telemetry.fboMs << ','
                    << telemetry.stateMs << ',' << telemetry.drawMs << ','
                    << (yieldsEnd >= yieldsStart ? yieldsEnd - yieldsStart : 0) << ','
                    << immediateEnd << ',' << queuedEnd << ',' << mPreparationStats.scheduled << ','
                    << mPreparationStats.invalidated << ',' << mPreparationStats.cancelled << ','
                    << mPreparationStats.requiredFallbacks << ',' << mPreparationStats.ageProgress << ','
                    << mPreparationStats.budgetYields << ',' << mPreparationStats.pendingBytes << ','
                    << (preparationActive ? mAdmission->chargedMs() : 0) << ',' << oldestAge;
                telemetryWriter.writeLine(row.str());
            }
        }
    }

    void CompositeMapRenderer::compile(CompositeMap& compositeMap, osg::RenderInfo& renderInfo) const
    {
        compileUntil(compositeMap, renderInfo, Deadline::max(), nullptr);
    }

    void CompositeMapRenderer::compileUntil(
        CompositeMap& compositeMap, osg::RenderInfo& renderInfo, Deadline deadline, CompileTelemetry* telemetry) const
    {
        const bool preparationActive = preparationSupported(*renderInfo.getState());
        const auto admissionStart = std::chrono::steady_clock::now();
        struct SubmissionCharge
        {
            Resource::P9DiscretionaryAdmission* admission;
            std::chrono::steady_clock::time_point start;
            ~SubmissionCharge()
            {
                if (admission)
                    admission->charge(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
        } charge{ preparationActive ? mAdmission.get() : nullptr, admissionStart };
        if (telemetry)
        {
            ++telemetry->maps;
            if (compositeMap.mRequired.load(std::memory_order_acquire))
                ++telemetry->requiredMaps;
        }

        // if there are no more external references we can assume the texture is no longer required
        int retainedTargetReferences = 1;
        if (preparationActive)
        {
            if (compositeMap.mPreparation && compositeMap.mPreparation->fbo->hasAttachment(osg::Camera::COLOR_BUFFER)
                && compositeMap.mPreparation->fbo->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == compositeMap.mTexture.get())
                ++retainedTargetReferences;
            if (mFBO->hasAttachment(osg::Camera::COLOR_BUFFER)
                && mFBO->getAttachment(osg::Camera::COLOR_BUFFER).getTexture() == compositeMap.mTexture.get())
                ++retainedTargetReferences;
        }
        if (compositeMap.mTexture->referenceCount() <= retainedTargetReferences)
        {
            compositeMap.mCompiled = compositeMap.mDrawables.size();
            if (preparationActive)
            {
                if (compositeMap.mPreparation)
                    compositeMap.mPreparation->lifetime->cancel();
                compositeMap.mDrawables.clear();
            }
            return;
        }

        const bool required = compositeMap.mRequired.load(std::memory_order_acquire);
        if (preparationActive && !required && !prepare(compositeMap, renderInfo))
            return;
        if (preparationActive && required && !dependenciesReady(compositeMap, *renderInfo.getState()))
        {
            // Required terrain retains the same complete synchronous bake. A
            // speculative preparation must never delay or partially publish it.
            ++mPreparationStats.requiredFallbacks;
            if (compositeMap.mPreparation)
                compositeMap.mPreparation->lifetime->cancel();
        }

        auto admitted = [&](bool includeFbo) {
            if (!preparationActive || compositeMap.mRequired.load(std::memory_order_acquire))
                return true;
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - admissionStart).count();
            const double predicted = std::max({ .05, mBakeEmaMs * 1.25,
                std::min(mBakeRiskMs, std::max(.25, mBakeEmaMs * 4.0)) })
                + (includeFbo ? std::max(mFboEmaMs * 1.25,
                    std::min(mFboRiskMs, std::max(.25, mFboEmaMs * 4.0))) : 0);
            if (predicted <= std::max(0.0, mAdmission->remainingMs() - elapsed))
                return true;
            const auto* stamp = renderInfo.getState()->getFrameStamp();
            const unsigned frame = stamp ? stamp->getFrameNumber() : 0;
            const unsigned age = compositeMap.mQueueFrameKnown && frame >= compositeMap.mQueuedFrame
                ? frame - compositeMap.mQueuedFrame : 0;
            if (mAdmission->takeProgress(age, mICO->compositeMaxQueueAge()))
            {
                ++mPreparationStats.ageProgress;
                return true;
            }
            ++mPreparationStats.budgetYields;
            return false;
        };
        if (!admitted(true))
            return;

        osg::Timer timer;
        osg::State& state = *renderInfo.getState();
        osg::GLExtensions* ext = state.get<osg::GLExtensions>();

        if (!mFBO)
            return;

        if (!ext->isFrameBufferObjectSupported)
            return;

        const auto fboStart = telemetry || preparationActive ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        osg::FrameBufferObject* fbo = preparationActive && dependenciesReady(compositeMap, state)
            ? compositeMap.mPreparation->fbo.get() : mFBO.get();
        if (fbo == mFBO.get())
            fbo->setAttachment(osg::Camera::COLOR_BUFFER, osg::FrameBufferAttachment(compositeMap.mTexture));
        fbo->apply(state, osg::FrameBufferObject::DRAW_FRAMEBUFFER);

        GLenum status = ext->glCheckFramebufferStatus(GL_FRAMEBUFFER_EXT);
        if (telemetry)
            telemetry->fboMs += Debug::V3Diagnostics::elapsedMs(fboStart);
        if (preparationActive)
        {
            const double actual = Debug::V3Diagnostics::elapsedMs(fboStart);
            mFboEmaMs = mFboSamples++ == 0 ? actual : mFboEmaMs * .85 + actual * .15;
            mFboRiskMs = std::max(actual, mFboRiskMs * .9);
        }

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

        bool firstBake = true;
        for (size_t i = compositeMap.mCompiled; i < compositeMap.mDrawables.size(); ++i)
        {
            if (!firstBake && !admitted(false))
                break;
            firstBake = false;
            if (deadline != Deadline::max() && !compositeMap.mRequired.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() >= deadline)
            {
                mBackgroundYields.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            osg::Drawable* drw = compositeMap.mDrawables[i];
            osg::StateSet* stateset = drw->getStateSet();
            const auto bakeStart = std::chrono::steady_clock::now();

            if (stateset)
                renderInfo.getState()->pushStateSet(stateset);

            const auto stateStart = telemetry ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            renderInfo.getState()->apply();
            if (telemetry)
                telemetry->stateMs += Debug::V3Diagnostics::elapsedMs(stateStart);

            glViewport(0, 0, compositeMap.mTexture->getTextureWidth(), compositeMap.mTexture->getTextureHeight());
            const auto drawStart = telemetry ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
            drw->drawImplementation(renderInfo);
            if (telemetry)
            {
                telemetry->drawMs += Debug::V3Diagnostics::elapsedMs(drawStart);
                ++telemetry->drawables;
            }

            if (stateset)
                renderInfo.getState()->popStateSet();

            if (preparationActive)
            {
                const double actual = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - bakeStart).count();
                mBakeEmaMs = mBakeSamples++ == 0 ? actual : mBakeEmaMs * .85 + actual * .15;
                mBakeRiskMs = std::max(actual, mBakeRiskMs * .9);
            }

            ++compositeMap.mCompiled;

            if (!preparationActive)
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
