#include "engine.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <array>
#include <deque>
#include <mutex>
#include <components/resource/v321classifiedcompileset.hpp>
#include <cstdlib>
#include <future>
#include <system_error>

#include <osgDB/ReaderWriter>
#include <osgDB/Registry>
#include <osgUtil/IncrementalCompileOperation>

#include <osgViewer/Renderer>
#include <osgViewer/ViewerEventHandlers>

#include <SDL3/SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/debug/gldebug.hpp>

#include <components/misc/rng.hpp>
#include <components/misc/strings/format.hpp>

#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>

#include <components/sdlutil/imagetosurface.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>

#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/resource/stats.hpp>

#include <components/compiler/extensions0.hpp>

#include <components/stereo/stereomanager.hpp>

#include <components/sceneutil/glextensions.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <components/files/configurationmanager.hpp>

#include <components/version/version.hpp>

#include <components/l10n/manager.hpp>

#include <components/loadinglistener/asynclistener.hpp>
#include <components/loadinglistener/loadinglistener.hpp>

#include <components/misc/frameratelimiter.hpp>

#include <components/sceneutil/color.hpp>
#include <components/sceneutil/depth.hpp>
#include <components/sceneutil/screencapture.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/util.hpp>

#include <components/settings/ramcache.hpp>
#include <components/settings/v36profile.hpp>
#include <components/settings/shadermanager.hpp>
#include <components/settings/values.hpp>

#include "mwinput/inputmanagerimp.hpp"

#include "mwgui/windowmanagerimp.hpp"

#include "mwlua/luamanagerimp.hpp"
#include "mwlua/worker.hpp"

#include "mwscript/interpretercontext.hpp"
#include "mwscript/scriptmanagerimp.hpp"

#include "mwsound/constants.hpp"
#include "mwsound/soundmanagerimp.hpp"

#include "mwworld/class.hpp"
#include "mwworld/datetimemanager.hpp"
#include "mwworld/worldimp.hpp"

#include "mwrender/vismask.hpp"

#include "mwclass/classes.hpp"

#include "mwdialogue/dialoguemanagerimp.hpp"
#include "mwdialogue/journalimp.hpp"
#include "mwdialogue/scripttest.hpp"

#include "mwmechanics/mechanicsmanagerimp.hpp"

#include "mwstate/statemanagerimp.hpp"

#include "profile.hpp"

namespace
{
    void checkSDLError(bool success)
    {
        if (!success)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }

    void initStatsHandler(Resource::Profiler& profiler)
    {
        const osg::Vec4f textColor(1.f, 1.f, 1.f, 1.f);
        const osg::Vec4f barColor(1.f, 1.f, 1.f, 1.f);
        const float multiplier = 1000;
        const bool average = true;
        const bool averageInInverseSpace = false;
        const float maxValue = 10000;

        OMW::forEachUserStatsValue([&](const OMW::UserStats& v) {
            profiler.addUserStatsLine(v.mLabel, textColor, barColor, v.mTaken, multiplier, average,
                averageInInverseSpace, v.mBegin, v.mEnd, maxValue);
        });
        // the forEachUserStatsValue loop is "run" at compile time, hence the settings manager is not available.
        // Unconditionnally add the async physics stats, and then remove it at runtime if necessary
        if (Settings::physics().mAsyncNumThreads == 0)
            profiler.removeUserStatsLine(" -Async");
    }

    struct ScreenCaptureMessageBox
    {
        void operator()(std::string filePath) const
        {
            if (filePath.empty())
            {
                MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                    "#{OMWEngine:ScreenshotFailed}", MWGui::ShowInDialogueMode_Never);

                return;
            }

            auto l10n = MWBase::Environment::get().getL10nManager()->getContext("OMWEngine");
            std::string message = l10n->formatMessage("ScreenshotMade", { "file" }, { L10n::toUnicode(filePath) });

            MWBase::Environment::get().getWindowManager()->scheduleMessageBox(
                std::move(message), MWGui::ShowInDialogueMode_Never);
        }
    };

    struct IgnoreString
    {
        void operator()(std::string) const {}
    };

    class IdentifyOpenGLOperation : public osg::GraphicsOperation
    {
    public:
        IdentifyOpenGLOperation()
            : GraphicsOperation("IdentifyOpenGLOperation", false)
        {
        }

        void operator()(osg::GraphicsContext* graphicsContext) override
        {
            Log(Debug::Info) << "Build identity: openmw-custom-v3.17 / openmw-custom-v3.18-render-scale-p0 / openmw-custom-v3.19-cpu-p0 / openmw-custom-v3.19-p0-stable-gaming / openmw-custom-v3.20-cp1-focus / openmw-custom-v3.21-cp1-completion-governor / openmw-custom-v3.21-cp1-adaptive-governor / openmw-custom-v3.21-cp2-fairness-dephasing / openmw-custom-v3.21-cp3-fullbody-first-person / openmw-custom-v3.21-cp4-shadow-compat / openmw-custom-v3.22-cp1-msoc-hotpath / openmw-custom-v3.22-cp2-occluder-efficiency / openmw-custom-v3.22-parallel-architecture-cp1 / openmw-custom-v3.23-parallel-msoc / openmw-custom-v3.24-frame-job-qos / openmw-custom-v3.25-engine-ownership-bridge / openmw-custom-v3.20-cp2-lua / openmw-custom-v3.20-cp3-sound-query";
            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);
            Log(Debug::Info) << "OpenGL Renderer: " << glGetString(GL_RENDERER);
            Log(Debug::Info) << "OpenGL Version: " << glGetString(GL_VERSION);
            glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &mMaxTextureImageUnits);
        }

        int getMaxTextureImageUnits() const
        {
            if (mMaxTextureImageUnits == 0)
                throw std::logic_error("mMaxTextureImageUnits is not initialized");
            return mMaxTextureImageUnits;
        }

    private:
        int mMaxTextureImageUnits = 0;
    };

    void reportStats(unsigned frameNumber, osgViewer::Viewer& viewer, std::ostream& stream)
    {
        viewer.getViewerStats()->report(stream, frameNumber);
        osgViewer::Viewer::Cameras cameras;
        viewer.getCameras(cameras);
        for (osg::Camera* camera : cameras)
            camera->getStats()->report(stream, frameNumber);
    }
}

void OMW::Engine::executeLocalScripts()
{
    MWWorld::LocalScripts& localScripts = mWorld->getLocalScripts();

    localScripts.startIteration();
    std::pair<ESM::RefId, MWWorld::Ptr> script;
    while (localScripts.getNext(script))
    {
        MWScript::InterpreterContext interpreterContext(&script.second.getRefData().getLocals(), script.second);
        mScriptManager->run(script.first, interpreterContext);
    }
}

bool OMW::Engine::frame(unsigned frameNumber, float frametime)
{
    const osg::Timer_t frameStart = mViewer->getStartTick();
    const osg::Timer* const timer = osg::Timer::instance();
    osg::Stats* const stats = mViewer->getViewerStats();

    mEnvironment.setFrameDuration(frametime);

    try
    {
        // Stop the background GC started at the previous frame's end.
        // Input handling can run Lua (the menu key does), so the state
        // must not be collected from this point on.
        mLuaWorker->finishGc();

        // update input
        {
            ScopedProfile<UserStatsType::Input> profile(frameStart, frameNumber, *timer, *stats);
            mInputManager->update(frametime, false);
        }

        // When the window is minimized, pause the game. Currently this *has* to be here to work around a MyGUI bug.
        // If we are not currently rendering, then RenderItems will not be reused resulting in a memory leak upon
        // changing widget textures (fixed in MyGUI 3.3.2), and destroyed widgets will not be deleted (not fixed yet,
        // https://github.com/MyGUI/mygui/issues/21)
        {
            ScopedProfile<UserStatsType::Sound> profile(frameStart, frameNumber, *timer, *stats);

            if (!mWindowManager->isWindowVisible())
            {
                mSoundManager->pausePlayback();
                return false;
            }
            else
                mSoundManager->resumePlayback();

            // sound
            if (mUseSound)
                mSoundManager->update(frametime);
        }

        {
            ScopedProfile<UserStatsType::LuaSyncUpdate> profile(frameStart, frameNumber, *timer, *stats);
            // Should be called after input manager update and before any change to the game world.
            // It applies to the game world queued changes from the previous frame.
            mLuaManager->synchronizedUpdate();
        }

        // update game state
        {
            ScopedProfile<UserStatsType::State> profile(frameStart, frameNumber, *timer, *stats);
            mStateManager->update(frametime);
        }

        bool paused = mWorld->getTimeManager()->isPaused();

        {
            ScopedProfile<UserStatsType::Script> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                if (!mWindowManager->containsMode(MWGui::GM_MainMenu) || !paused)
                {
                    if (mWorld->getScriptsEnabled())
                    {
                        // local scripts
                        executeLocalScripts();

                        // global scripts
                        mScriptManager->getGlobalScripts().run();
                    }

                    mWorld->getWorldScene().markCellAsUnchanged();
                }

                if (!paused)
                {
                    double hours = (frametime * mWorld->getTimeManager()->getGameTimeScale()) / 3600.0;
                    mWorld->advanceTime(hours, true);
                    mWorld->rechargeItems(frametime, true);
                }
            }
        }

        // update mechanics
        {
            ScopedProfile<UserStatsType::Mechanics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mMechanicsManager->update(frametime, paused);
            }

            if (mStateManager->getState() == MWBase::StateManager::State_Running)
            {
                MWWorld::Ptr player = mWorld->getPlayerPtr();
                if (!paused && player.getClass().getCreatureStats(player).isDead())
                    mStateManager->endGame();
            }
        }

        // update physics
        {
            ScopedProfile<UserStatsType::Physics> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->updatePhysics(frametime, paused, frameStart, frameNumber, *stats);
            }
        }

        // update world
        {
            ScopedProfile<UserStatsType::World> profile(frameStart, frameNumber, *timer, *stats);

            if (mStateManager->getState() != MWBase::StateManager::State_NoGame)
            {
                mWorld->update(frametime, paused);
            }
        }

        // update GUI
        {
            ScopedProfile<UserStatsType::Gui> profile(frameStart, frameNumber, *timer, *stats);
            mWindowManager->update(frametime);
        }
    }
    catch (const std::exception& e)
    {
        Log(Debug::Error) << "Error in frame: " << e.what();
    }

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(Debug::V3HitchTelemetry::FrameTailStage::PreViewer);
        const bool reportResource = stats->collectStats("resource");

        if (reportResource)
            stats->setAttribute(frameNumber, "UnrefQueue", static_cast<double>(mUnrefQueue->getSize()));

        mUnrefQueue->flush(*mWorkQueue);

        if (reportResource)
        {
            stats->setAttribute(frameNumber, "FrameNumber", frameNumber);

            mResourceSystem->reportStats(frameNumber, stats);

            stats->setAttribute(frameNumber, "WorkQueue", static_cast<double>(mWorkQueue->getNumItems()));
            stats->setAttribute(frameNumber, "WorkThread", static_cast<double>(mWorkQueue->getNumActiveThreads()));

            mMechanicsManager->reportStats(frameNumber, *stats);
            mWorld->reportStats(frameNumber, *stats);
            mLuaManager->reportStats(frameNumber, *stats);

            stats->setAttribute(frameNumber, "StringRefId Count", static_cast<double>(ESM::StringRefId::totalCount()));
        }

        mStereoManager->updateSettings(Settings::camera().mNearClip, Settings::camera().mViewingDistance);
    }

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::EventTraversal);
        mViewer->eventTraversal();
    }
    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::UpdateTraversal);

        // V3.21 CP1: OSG normally merges every fully compiled CompileSet here.
        // Producers and GL preparation remain active; only completed-set admission
        // to the main-thread merge is bounded. FIFO plus bounded-age extra service
        // prevents indefinite starvation while smoothing completion bursts.
        static const int v321CompletionGovernorMode = [] {
            const int configured = static_cast<int>(Settings::cells().mV321CompletionGovernorMode);
            const char* value = std::getenv("OPENMW_V321_COMPLETION_GOVERNOR");
            if (value == nullptr || *value == '\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 0 && parsed <= 2 ? parsed : configured;
        }();

        const bool v321CP2FairnessMode = Resource::v321CP2FairnessEnabled();

        if (v321CompletionGovernorMode > 0 || v321CP2FairnessMode)
        {
            osgUtil::IncrementalCompileOperation* const ico = mViewer->getIncrementalCompileOperation();
            if (ico != nullptr)
            {
                struct V321DeferredCompileSet
                {
                    osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> mSet;
                    unsigned int mFirstDeferredFrame = 0;
                };
                struct V321CompletionCounters
                {
                    std::uint64_t mCompletedSeen = 0;
                    std::uint64_t mAdmitted = 0;
                    std::uint64_t mForced = 0;
                    std::uint64_t mPeakDeferred = 0;
                };

                static std::deque<V321DeferredCompileSet> deferred;
                static V321CompletionCounters counters;

                // Separate queues are used only by CP2 Mode129. CP1 modes keep
                // their original single FIFO and admission behavior unchanged.
                static std::array<std::deque<V321DeferredCompileSet>, 4> cp2Deferred;
                static std::array<unsigned int, 4> cp2Deficit = { 0, 0, 0, 0 };
                static std::array<std::uint64_t, 4> cp2Seen = { 0, 0, 0, 0 };
                static std::array<std::uint64_t, 4> cp2Admitted = { 0, 0, 0, 0 };
                static unsigned int cp2Cursor = 0;

                const unsigned int baseBudget
                    = static_cast<unsigned int>(Settings::cells().mV321MergeSetsPerFrame);
                const unsigned int maxDeferredFrames
                    = static_cast<unsigned int>(Settings::cells().mV321MaxDeferredFrames);
                const unsigned int forcedBudget
                    = static_cast<unsigned int>(Settings::cells().mV321ForcedMergeSets);

                unsigned int completedThisFrame = 0;
                unsigned int admittedThisFrame = 0;
                unsigned int forcedThisFrame = 0;
                unsigned int oldestAge = 0;
                unsigned int v321DeferredDepthForStats = 0;
                unsigned int cp2ActiveClasses = 0;
                std::array<unsigned int, 4> cp2AdmittedThisFrame = { 0, 0, 0, 0 };

                {
                    std::lock_guard<OpenThreads::Mutex> lock(*ico->getCompiledMutex());
                    osgUtil::IncrementalCompileOperation::CompileSets& completed = ico->getCompiled();

                    auto cp2ClassIndex = [](Resource::V321CompileClass value) -> unsigned int {
                        switch (value)
                        {
                            case Resource::V321CompileClass::ObjectPaging: return 0;
                            case Resource::V321CompileClass::Terrain: return 1;
                            case Resource::V321CompileClass::GenericModel: return 2;
                            case Resource::V321CompileClass::Unknown:
                            default: return 3;
                        }
                    };

                    while (!completed.empty())
                    {
                        osg::ref_ptr<osgUtil::IncrementalCompileOperation::CompileSet> set = completed.front();
                        completed.pop_front();
                        if (v321CP2FairnessMode)
                        {
                            const unsigned int index
                                = cp2ClassIndex(Resource::getV321CompileClass(set.get()));
                            cp2Deferred[index].push_back(V321DeferredCompileSet{ set, frameNumber });
                            ++cp2Seen[index];
                        }
                        else
                            deferred.push_back(V321DeferredCompileSet{ set, frameNumber });
                        ++completedThisFrame;
                        ++counters.mCompletedSeen;
                    }

                    if (v321CP2FairnessMode)
                    {
                        const unsigned int serviceBudget
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ServiceSetsPerFrame);
                        const unsigned int configuredClassBurst
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ClassBurstSetsPerFrame);
                        const unsigned int maxDeferredFrames
                            = static_cast<unsigned int>(Settings::cells().mV321CP2MaxDeferredFrames);
                        const unsigned int forcedBudget
                            = static_cast<unsigned int>(Settings::cells().mV321CP2ForcedSets);
                        const unsigned int deficitCap
                            = static_cast<unsigned int>(Settings::cells().mV321CP2DeficitCap);
                        constexpr std::array<unsigned int, 4> quantum = { 2, 2, 1, 1 };

                        auto totalDeferred = [&]() -> unsigned int {
                            unsigned int total = 0;
                            for (const auto& queue : cp2Deferred)
                                total += static_cast<unsigned int>(queue.size());
                            return total;
                        };
                        auto recomputeOldestAge = [&]() -> unsigned int {
                            unsigned int age = 0;
                            for (const auto& queue : cp2Deferred)
                                if (!queue.empty())
                                    age = std::max(age, frameNumber - queue.front().mFirstDeferredFrame);
                            return age;
                        };
                        auto admitClass = [&](unsigned int index, bool forced) {
                            completed.push_back(cp2Deferred[index].front().mSet);
                            cp2Deferred[index].pop_front();
                            ++admittedThisFrame;
                            ++cp2AdmittedThisFrame[index];
                            ++cp2Admitted[index];
                            if (forced)
                                ++forcedThisFrame;
                        };

                        for (unsigned int i = 0; i < 4; ++i)
                        {
                            if (!cp2Deferred[i].empty())
                                ++cp2ActiveClasses;
                            cp2Deficit[i] = std::min(deficitCap, cp2Deficit[i] + quantum[i]);
                        }

                        if (cp2ActiveClasses == 1)
                        {
                            // A single active class gets the full budget: fairness
                            // must not become an artificial producer throttle.
                            for (unsigned int i = 0; i < 4; ++i)
                            {
                                if (cp2Deferred[i].empty())
                                    continue;
                                const unsigned int count
                                    = std::min(serviceBudget, static_cast<unsigned int>(cp2Deferred[i].size()));
                                for (unsigned int n = 0; n < count; ++n)
                                    admitClass(i, false);
                                cp2Deficit[i] = cp2Deficit[i] > count ? cp2Deficit[i] - count : 0;
                                cp2Cursor = (i + 1) % 4;
                                break;
                            }
                        }
                        else if (cp2ActiveClasses > 1)
                        {
                            const unsigned int classBurst = std::min(configuredClassBurst, serviceBudget);
                            unsigned int refills = 0;
                            while (admittedThisFrame < serviceBudget && totalDeferred() > 0)
                            {
                                int selected = -1;
                                for (unsigned int offset = 0; offset < 4; ++offset)
                                {
                                    const unsigned int index = (cp2Cursor + offset) % 4;
                                    if (!cp2Deferred[index].empty()
                                        && cp2AdmittedThisFrame[index] < classBurst
                                        && cp2Deficit[index] > 0)
                                    {
                                        selected = static_cast<int>(index);
                                        break;
                                    }
                                }
                                if (selected < 0)
                                {
                                    bool classCapBlocksAll = true;
                                    for (unsigned int i = 0; i < 4; ++i)
                                        if (!cp2Deferred[i].empty() && cp2AdmittedThisFrame[i] < classBurst)
                                        {
                                            classCapBlocksAll = false;
                                            break;
                                        }
                                    if (classCapBlocksAll || refills >= 4)
                                        break;
                                    for (unsigned int i = 0; i < 4; ++i)
                                        cp2Deficit[i] = std::min(deficitCap, cp2Deficit[i] + quantum[i]);
                                    ++refills;
                                    continue;
                                }
                                const unsigned int index = static_cast<unsigned int>(selected);
                                admitClass(index, false);
                                --cp2Deficit[index];
                                cp2Cursor = (index + 1) % 4;
                            }
                        }

                        // Global age escape ignores class cap/deficit so no class
                        // can starve indefinitely. Extra service itself is bounded.
                        for (unsigned int forced = 0; forced < forcedBudget; ++forced)
                        {
                            int oldestClass = -1;
                            unsigned int oldestClassAge = 0;
                            for (unsigned int i = 0; i < 4; ++i)
                            {
                                if (cp2Deferred[i].empty())
                                    continue;
                                const unsigned int age = frameNumber - cp2Deferred[i].front().mFirstDeferredFrame;
                                if (age >= maxDeferredFrames && (oldestClass < 0 || age > oldestClassAge))
                                {
                                    oldestClass = static_cast<int>(i);
                                    oldestClassAge = age;
                                }
                            }
                            if (oldestClass < 0)
                                break;
                            admitClass(static_cast<unsigned int>(oldestClass), true);
                        }

                        oldestAge = recomputeOldestAge();
                        v321DeferredDepthForStats = totalDeferred();
                        counters.mAdmitted += admittedThisFrame;
                        counters.mForced += forcedThisFrame;
                        counters.mPeakDeferred = std::max<std::uint64_t>(
                            counters.mPeakDeferred, v321DeferredDepthForStats);
                    }
                    else
                    {
                        if (!deferred.empty())
                            oldestAge = frameNumber - deferred.front().mFirstDeferredFrame;

                        unsigned int admissionBudget = baseBudget;
                        unsigned int adaptiveDebtRepaid = 0;
                        static double adaptiveFrameEmaMs = 0.0;
                        static unsigned int adaptiveDebt = 0;
                        const double previousFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();

                        if (v321CompletionGovernorMode == 2)
                        {
                            const double targetMs
                                = static_cast<double>(Settings::cells().mV321AdaptiveTargetMilliseconds);
                            const double alpha
                                = static_cast<double>(Settings::cells().mV321AdaptiveFrameEmaAlpha);
                            const unsigned int minBudget
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMin);
                            const unsigned int configuredMax
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveMergeMax);
                            const unsigned int maxBudget = std::max(minBudget, configuredMax);
                            const unsigned int debtCap
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtCap);
                            const unsigned int repayCap
                                = static_cast<unsigned int>(Settings::cells().mV321AdaptiveDebtRepayPerFrame);
                            if (previousFrameMs > 0.0)
                                adaptiveFrameEmaMs = adaptiveFrameEmaMs <= 0.0
                                    ? previousFrameMs
                                    : adaptiveFrameEmaMs * (1.0 - alpha) + previousFrameMs * alpha;
                            if (previousFrameMs >= targetMs + 6.0)
                                admissionBudget = minBudget;
                            else if (previousFrameMs >= targetMs + 2.0)
                                admissionBudget = std::max(minBudget, baseBudget > 1 ? baseBudget - 1 : 1u);
                            else if (previousFrameMs > 0.0 && previousFrameMs <= targetMs - 3.0
                                && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                                admissionBudget = maxBudget;
                            else
                                admissionBudget = std::min(maxBudget, std::max(minBudget, baseBudget));
                            if (!deferred.empty() && admissionBudget < baseBudget && adaptiveDebt < debtCap)
                            {
                                const unsigned int withheld = baseBudget - admissionBudget;
                                adaptiveDebt += std::min(withheld, debtCap - adaptiveDebt);
                            }
                            if (!deferred.empty() && adaptiveDebt > 0 && previousFrameMs > 0.0
                                && previousFrameMs <= targetMs - 2.0
                                && (adaptiveFrameEmaMs <= 0.0 || adaptiveFrameEmaMs <= targetMs))
                            {
                                const unsigned int room
                                    = maxBudget > admissionBudget ? maxBudget - admissionBudget : 0;
                                adaptiveDebtRepaid = std::min(repayCap, std::min(adaptiveDebt, room));
                                admissionBudget += adaptiveDebtRepaid;
                                adaptiveDebt -= adaptiveDebtRepaid;
                            }
                        }

                        const unsigned int adaptiveBudgetBeforeForced = admissionBudget;
                        const unsigned int maxDeferredFrames
                            = static_cast<unsigned int>(Settings::cells().mV321MaxDeferredFrames);
                        const unsigned int forcedBudget
                            = static_cast<unsigned int>(Settings::cells().mV321ForcedMergeSets);
                        if (deferred.size() > admissionBudget && oldestAge >= maxDeferredFrames)
                            admissionBudget += forcedBudget;
                        while (admittedThisFrame < admissionBudget && !deferred.empty())
                        {
                            completed.push_back(deferred.front().mSet);
                            deferred.pop_front();
                            ++admittedThisFrame;
                        }
                        if (admittedThisFrame > adaptiveBudgetBeforeForced)
                            forcedThisFrame = admittedThisFrame - adaptiveBudgetBeforeForced;
                        counters.mAdmitted += admittedThisFrame;
                        counters.mForced += forcedThisFrame;
                        if (deferred.size() > counters.mPeakDeferred)
                            counters.mPeakDeferred = deferred.size();
                        v321DeferredDepthForStats = static_cast<unsigned int>(deferred.size());

                        if (stats->collectStats("resource") && v321CompletionGovernorMode == 2)
                        {
                            stats->setAttribute(frameNumber, "V321 Adaptive PreviousFrameMs", previousFrameMs);
                            stats->setAttribute(frameNumber, "V321 Adaptive FrameEmaMs", adaptiveFrameEmaMs);
                            stats->setAttribute(frameNumber, "V321 Adaptive MergeBudget", adaptiveBudgetBeforeForced);
                            stats->setAttribute(frameNumber, "V321 Adaptive Debt", adaptiveDebt);
                            stats->setAttribute(frameNumber, "V321 Adaptive DebtRepaid", adaptiveDebtRepaid);
                        }
                    }
                }

                if (stats->collectStats("resource"))
                {
                    stats->setAttribute(frameNumber, "V321 Completion Seen", counters.mCompletedSeen);
                    stats->setAttribute(frameNumber, "V321 Completion Admitted", counters.mAdmitted);
                    stats->setAttribute(frameNumber, "V321 Completion Forced", counters.mForced);
                    stats->setAttribute(frameNumber, "V321 Completion PeakDeferred", counters.mPeakDeferred);
                    stats->setAttribute(frameNumber, "V321 Completion CompletedThisFrame", completedThisFrame);
                    stats->setAttribute(frameNumber, "V321 Completion AdmittedThisFrame", admittedThisFrame);
                    stats->setAttribute(
                        frameNumber, "V321 Completion Deferred", static_cast<double>(v321DeferredDepthForStats));
                    stats->setAttribute(frameNumber, "V321 Completion OldestAge", oldestAge);
                    if (v321CP2FairnessMode)
                    {
                        stats->setAttribute(frameNumber, "V321 CP2 ActiveClasses", cp2ActiveClasses);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Deferred", cp2Deferred[0].size());
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Deferred", cp2Deferred[1].size());
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Deferred", cp2Deferred[2].size());
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Deferred", cp2Deferred[3].size());
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging AdmittedFrame", cp2AdmittedThisFrame[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain AdmittedFrame", cp2AdmittedThisFrame[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel AdmittedFrame", cp2AdmittedThisFrame[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown AdmittedFrame", cp2AdmittedThisFrame[3]);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Seen", cp2Seen[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Seen", cp2Seen[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Seen", cp2Seen[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Seen", cp2Seen[3]);
                        stats->setAttribute(frameNumber, "V321 CP2 ObjectPaging Admitted", cp2Admitted[0]);
                        stats->setAttribute(frameNumber, "V321 CP2 Terrain Admitted", cp2Admitted[1]);
                        stats->setAttribute(frameNumber, "V321 CP2 GenericModel Admitted", cp2Admitted[2]);
                        stats->setAttribute(frameNumber, "V321 CP2 Unknown Admitted", cp2Admitted[3]);
                    }
                }
            }
        }

        mViewer->updateTraversal();
    }

    // update focus object for GUI
    {
        ScopedProfile<UserStatsType::Focus> profile(frameStart, frameNumber, *timer, *stats);
        // V3.20 preserves the exact V3.19 fixed-cadence path by default. Optional
        // adaptive mode only adds an immediate refresh when the main camera's view or
        // projection contract changes. The configured cadence remains a hard maximum
        // staleness bound for moving world objects while the camera is stationary.
        // GUI mode always refreshes. Activation/input queries remain untouched.
        static const unsigned v319FocusCadence = [] {
            const unsigned configured = static_cast<unsigned>(Settings::cells().mV319FocusCadence);
            const char* value = std::getenv("OPENMW_V319_FOCUS_CADENCE");
            if (value == nullptr || *value == '\0')
                return configured;
            const int parsed = std::atoi(value);
            return parsed >= 1 && parsed <= 3 ? static_cast<unsigned>(parsed) : configured;
        }();
        static const bool v320AdaptiveFocusCadence = [] {
            const bool configured = Settings::cells().mV320AdaptiveFocusCadence;
            const char* value = std::getenv("OPENMW_V320_FOCUS_ADAPTIVE");
            return value == nullptr || *value == '\0' ? configured : std::atoi(value) != 0;
        }();

        struct V320FocusCounters
        {
            std::uint64_t mAttempted = 0;
            std::uint64_t mExecuted = 0;
            std::uint64_t mCadenceSkipped = 0;
            std::uint64_t mDirtyForced = 0;
            std::uint64_t mFixedAttempted = 0;
            std::uint64_t mFixedExecuted = 0;
            std::uint64_t mAdaptiveAttempted = 0;
            std::uint64_t mAdaptiveExecuted = 0;
        };
        static V320FocusCounters v320FocusCounters;

        bool cameraDirty = false;
        if (v320AdaptiveFocusCadence)
        {
            static bool initialized = false;
            static osg::Matrixd previousView;
            static osg::Matrixd previousProjection;
            const osg::Camera* const camera = mViewer->getCamera();
            const osg::Matrixd currentView = camera->getViewMatrix();
            const osg::Matrixd currentProjection = camera->getProjectionMatrix();
            cameraDirty = !initialized || currentView != previousView || currentProjection != previousProjection;
            previousView = currentView;
            previousProjection = currentProjection;
            initialized = true;
        }

        const bool guiForced = mWindowManager->isGuiMode();
        const bool cadenceDue = v319FocusCadence <= 1 || frameNumber % v319FocusCadence == 0;
        const bool dirtyForced = v320AdaptiveFocusCadence && cameraDirty && !guiForced && !cadenceDue;
        const bool execute = cadenceDue || guiForced || (v320AdaptiveFocusCadence && cameraDirty);

        ++v320FocusCounters.mAttempted;
        if (v320AdaptiveFocusCadence)
            ++v320FocusCounters.mAdaptiveAttempted;
        else
            ++v320FocusCounters.mFixedAttempted;

        if (execute)
        {
            mWorld->updateFocusObject();
            ++v320FocusCounters.mExecuted;
            if (v320AdaptiveFocusCadence)
                ++v320FocusCounters.mAdaptiveExecuted;
            else
                ++v320FocusCounters.mFixedExecuted;
            if (dirtyForced)
                ++v320FocusCounters.mDirtyForced;
        }
        else
            ++v320FocusCounters.mCadenceSkipped;

        // Aggregate-only attribution. No per-frame file logging is added; values are
        // published only when the existing resource-stat collector is enabled.
        if (stats->collectStats("resource"))
        {
            stats->setAttribute(frameNumber, "V320 Focus Attempted", v320FocusCounters.mAttempted);
            stats->setAttribute(frameNumber, "V320 Focus Executed", v320FocusCounters.mExecuted);
            stats->setAttribute(frameNumber, "V320 Focus CadenceSkipped", v320FocusCounters.mCadenceSkipped);
            stats->setAttribute(frameNumber, "V320 Focus DirtyForced", v320FocusCounters.mDirtyForced);
            stats->setAttribute(frameNumber, "V320 Focus FixedAttempted", v320FocusCounters.mFixedAttempted);
            stats->setAttribute(frameNumber, "V320 Focus FixedExecuted", v320FocusCounters.mFixedExecuted);
            stats->setAttribute(frameNumber, "V320 Focus AdaptiveAttempted", v320FocusCounters.mAdaptiveAttempted);
            stats->setAttribute(frameNumber, "V320 Focus AdaptiveExecuted", v320FocusCounters.mAdaptiveExecuted);
        }
    }

    // if there is a separate Lua thread, it starts the update now
    mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
            Debug::V3HitchTelemetry::FrameTailStage::RenderingTraversal);
        mViewer->renderingTraversals();
    }

    {
        Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(Debug::V3HitchTelemetry::FrameTailStage::LuaWait);
        mLuaWorker->finishUpdate(frameStart, frameNumber, *stats);
    }

    // The Lua state is unused until the next frame starts: the worker collects
    // garbage through the frame tail and the framerate-limiter sleep.
    mLuaWorker->gc();

    return true;
}

OMW::Engine::Engine(Files::ConfigurationManager& configurationManager)
    : mWindow(nullptr)
    , mEncoding(ToUTF8::WINDOWS_1252)
    , mScreenCaptureOperation(nullptr)
    , mSelectDepthFormatOperation(new SceneUtil::SelectDepthFormatOperation())
    , mSelectColorFormatOperation(new SceneUtil::Color::SelectColorFormatOperation())
    , mStereoManager(nullptr)
    , mSkipMenu(false)
    , mUseSound(true)
    , mCompileAll(false)
    , mCompileAllDialogue(false)
    , mWarningsMode(1)
    , mScriptConsoleMode(false)
    , mActivationDistanceOverride(-1)
    , mGrab(true)
    , mExportFonts(false)
    , mRandomSeed(0)
    , mNewGame(false)
    , mCfgMgr(configurationManager)
    , mGlMaxTextureImageUnits(0)
{
#if SDL_VERSION_ATLEAST(2, 24, 0)
    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, "1");
#endif
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0"); // We use only gamepads

    const SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
    if (SDL_WasInit(flags) == 0)
    {
        SDL_SetMainReady();
        if (!SDL_Init(flags))
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
    }
}

OMW::Engine::~Engine()
{
    if (mScreenCaptureOperation != nullptr)
    {
        mScreenCaptureOperation->stop();
        mScreenCaptureOperation = nullptr;
    }
    mScreenCaptureHandler = nullptr;

    mMechanicsManager = nullptr;
    mDialogueManager = nullptr;
    mJournal = nullptr;
    mWindowManager = nullptr;
    mScriptManager = nullptr;
    mWorld = nullptr;
    mStereoManager = nullptr;
    mSoundManager = nullptr;
    mInputManager = nullptr;
    mStateManager = nullptr;
    mLuaWorker = nullptr;
    mLuaManager = nullptr;
    mL10nManager = nullptr;

    mScriptContext = nullptr;

    mUnrefQueue = nullptr;
    mWorkQueue = nullptr;

    mViewer = nullptr;

    mResourceSystem.reset();

    mEncoder = nullptr;

    if (mWindow)
    {
        SDL_DestroyWindow(mWindow);
        mWindow = nullptr;
    }

    SDL_Quit();

    Log(Debug::Info) << "Quitting peacefully.";
}

// Set data dir

void OMW::Engine::setDataDirs(const Files::PathContainer& dataDirs)
{
    mDataDirs = dataDirs;
    mDataDirs.insert(mDataDirs.begin(), mResDir / "vfs");
    mFileCollections = Files::Collections(mDataDirs);
}

// Add BSA archive
void OMW::Engine::addArchive(const std::string& archive)
{
    mArchives.push_back(archive);
}

// Set resource dir
void OMW::Engine::setResourceDir(const std::filesystem::path& parResDir)
{
    mResDir = parResDir;
    if (!Version::checkResourcesVersion(mResDir))
        Log(Debug::Error) << "Resources dir " << mResDir
                          << " doesn't match OpenMW binary, the game may work incorrectly.";
}

// Set start cell name
void OMW::Engine::setCell(const std::string& cellName)
{
    mCellName = cellName;
}

void OMW::Engine::addContentFile(const std::string& file)
{
    mContentFiles.push_back(file);
}

void OMW::Engine::addGroundcoverFile(const std::string& file)
{
    mGroundcoverFiles.emplace_back(file);
}

void OMW::Engine::setSkipMenu(bool skipMenu, bool newGame)
{
    mSkipMenu = skipMenu;
    mNewGame = newGame;
}

void OMW::Engine::createWindow()
{
    const int screen = Settings::video().mScreen;
    const int width = Settings::video().mResolutionX;
    const int height = Settings::video().mResolutionY;
    const Settings::WindowMode windowMode = Settings::video().mWindowMode;
    const bool windowBorder = Settings::video().mWindowBorder;
    const SDLUtil::VSyncMode vsync = Settings::video().mVsyncMode;
    unsigned antialiasing = static_cast<unsigned>(Settings::video().mAntialiasing);

    int displayCount = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
    SDL_DisplayID displayId = 0;
    if (displays && screen >= 0 && screen < displayCount)
        displayId = displays[screen];
    SDL_free(displays);
    if (!displayId)
        displayId = SDL_GetPrimaryDisplay();

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(displayId);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(displayId);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;

    // Allows for Windows snapping features to properly work in borderless window
    SDL_SetHint("SDL_BORDERLESS_WINDOWED_STYLE", "1");
    SDL_SetHint("SDL_BORDERLESS_RESIZABLE_STYLE", "1");

    if (!windowBorder)
        flags |= SDL_WINDOW_BORDERLESS;

    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, Settings::video().mMinimizeOnFocusLoss ? "1" : "0");

    checkSDLError(SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0));
    checkSDLError(SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24));
    if (Debug::shouldDebugOpenGL())
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG));

    if (antialiasing > 0)
    {
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1));
        checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
    }

    osg::ref_ptr<SDLUtil::GraphicsWindowSDL2> graphicsWindow;
    while (!graphicsWindow || !graphicsWindow->valid())
    {
        while (!mWindow)
        {
            mWindow = SDL_CreateWindow("OpenMW", width, height, flags);
            if (mWindow)
            {
                SDL_SetWindowPosition(mWindow, posX, posY);
                if (windowMode == Settings::WindowMode::Fullscreen)
                {
                    SDL_DisplayMode mode{};
                    if (displayId
                        && SDL_GetClosestFullscreenDisplayMode(displayId, width, height, 0.f, true, &mode))
                        checkSDLError(SDL_SetWindowFullscreenMode(mWindow, &mode));
                    checkSDLError(SDL_SetWindowFullscreen(mWindow, true));
                }
                else if (windowMode == Settings::WindowMode::WindowedFullscreen)
                {
                    checkSDLError(SDL_SetWindowFullscreenMode(mWindow, nullptr));
                    checkSDLError(SDL_SetWindowFullscreen(mWindow, true));
                }
            }
            if (!mWindow)
            {
                // Try with a lower AA
                if (antialiasing > 0)
                {
                    Log(Debug::Warning) << "Warning: " << antialiasing << "x antialiasing not supported, trying "
                                        << antialiasing / 2;
                    antialiasing /= 2;
                    Settings::video().mAntialiasing.set(antialiasing);
                    checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
                    continue;
                }
                else
                {
                    std::stringstream error;
                    error << "Failed to create SDL window: " << SDL_GetError();
                    throw std::runtime_error(error.str());
                }
            }
        }

        // Since we use physical resolution internally, we have to create the window with scaled resolution,
        // but we can't get the scale before the window exists, so instead we have to resize aftewards.
        int w, h;
        SDL_GetWindowSize(mWindow, &w, &h);
        int dw, dh;
        SDL_GetWindowSizeInPixels(mWindow, &dw, &dh);
        if (dw != w || dh != h)
        {
            SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));
        }

        setWindowIcon();

        osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
        SDL_GetWindowPosition(mWindow, &traits->x, &traits->y);
        SDL_GetWindowSizeInPixels(mWindow, &traits->width, &traits->height);
        traits->windowName = SDL_GetWindowTitle(mWindow);
        traits->windowDecoration = !(SDL_GetWindowFlags(mWindow) & SDL_WINDOW_BORDERLESS);
        traits->screenNum = screen;
        traits->vsync = 0;
        traits->inheritedWindowData = new SDLUtil::GraphicsWindowSDL2::WindowData(mWindow);

        graphicsWindow = new SDLUtil::GraphicsWindowSDL2(traits, vsync);
        if (!graphicsWindow->valid())
            throw std::runtime_error("Failed to create GraphicsContext");

        if (traits->samples < antialiasing)
        {
            Log(Debug::Warning) << "Warning: Framebuffer MSAA level is only " << traits->samples << "x instead of "
                                << antialiasing << "x. Trying " << antialiasing / 2 << "x instead.";
            graphicsWindow->closeImplementation();
            SDL_DestroyWindow(mWindow);
            mWindow = nullptr;
            antialiasing /= 2;
            Settings::video().mAntialiasing.set(antialiasing);
            checkSDLError(SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, antialiasing));
            continue;
        }

        if (traits->red < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->red << " bit red channel.";
        if (traits->green < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->green << " bit green channel.";
        if (traits->blue < 8)
            Log(Debug::Warning) << "Warning: Framebuffer only has a " << traits->blue << " bit blue channel.";
        if (traits->depth < 24)
            Log(Debug::Warning) << "Warning: Framebuffer only has " << traits->depth << " bits of depth precision.";

        traits->alpha = 0; // set to 0 to stop ScreenCaptureHandler reading the alpha channel
    }

    osg::ref_ptr<osg::Camera> camera = mViewer->getCamera();
    camera->setGraphicsContext(graphicsWindow);
    camera->setViewport(0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);

    osg::ref_ptr<SceneUtil::OperationSequence> realizeOperations = new SceneUtil::OperationSequence(false);
    mViewer->setRealizeOperation(realizeOperations);
    osg::ref_ptr<IdentifyOpenGLOperation> identifyOp = new IdentifyOpenGLOperation();
    realizeOperations->add(identifyOp);
    realizeOperations->add(new SceneUtil::GetGLExtensionsOperation());

    if (Debug::shouldDebugOpenGL())
        realizeOperations->add(new Debug::EnableGLDebugOperation());

    realizeOperations->add(mSelectDepthFormatOperation);
    realizeOperations->add(mSelectColorFormatOperation);

    if (Stereo::getStereo())
    {
        Stereo::Settings settings;

        settings.mMultiview = Settings::stereo().mMultiview;
        settings.mAllowDisplayListsForMultiview = Settings::stereo().mAllowDisplayListsForMultiview;
        settings.mSharedShadowMaps = Settings::stereo().mSharedShadowMaps;

        if (Settings::stereo().mUseCustomView)
        {
            const osg::Vec3 leftEyeOffset(Settings::stereoView().mLeftEyeOffsetX,
                Settings::stereoView().mLeftEyeOffsetY, Settings::stereoView().mLeftEyeOffsetZ);

            const osg::Quat leftEyeOrientation(Settings::stereoView().mLeftEyeOrientationX,
                Settings::stereoView().mLeftEyeOrientationY, Settings::stereoView().mLeftEyeOrientationZ,
                Settings::stereoView().mLeftEyeOrientationW);

            const osg::Vec3 rightEyeOffset(Settings::stereoView().mRightEyeOffsetX,
                Settings::stereoView().mRightEyeOffsetY, Settings::stereoView().mRightEyeOffsetZ);

            const osg::Quat rightEyeOrientation(Settings::stereoView().mRightEyeOrientationX,
                Settings::stereoView().mRightEyeOrientationY, Settings::stereoView().mRightEyeOrientationZ,
                Settings::stereoView().mRightEyeOrientationW);

            settings.mCustomView = Stereo::CustomView{
                .mLeft = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = leftEyeOffset,
                        .orientation = leftEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mLeftEyeFovLeft,
                        .angleRight = Settings::stereoView().mLeftEyeFovRight,
                        .angleUp = Settings::stereoView().mLeftEyeFovUp,
                        .angleDown = Settings::stereoView().mLeftEyeFovDown,
                    },
                },
                .mRight = Stereo::View{
                    .pose = Stereo::Pose{
                        .position = rightEyeOffset,
                        .orientation = rightEyeOrientation,
                    },
                    .fov = Stereo::FieldOfView{
                        .angleLeft = Settings::stereoView().mRightEyeFovLeft,
                        .angleRight = Settings::stereoView().mRightEyeFovRight,
                        .angleUp = Settings::stereoView().mRightEyeFovUp,
                        .angleDown = Settings::stereoView().mRightEyeFovDown,
                    },
                },
            };
        }

        if (Settings::stereo().mUseCustomEyeResolution)
            settings.mEyeResolution
                = osg::Vec2i(Settings::stereoView().mEyeResolutionX, Settings::stereoView().mEyeResolutionY);

        realizeOperations->add(new Stereo::InitializeStereoOperation(settings));
    }

    mViewer->realize();
    mGlMaxTextureImageUnits = identifyOp->getMaxTextureImageUnits();

    mViewer->getEventQueue()->getCurrentEventState()->setWindowRectangle(
        0, 0, graphicsWindow->getTraits()->width, graphicsWindow->getTraits()->height);
}

void OMW::Engine::setWindowIcon()
{
    std::ifstream windowIconStream;
    const auto windowIcon = mResDir / "openmw.png";
    windowIconStream.open(windowIcon, std::ios_base::in | std::ios_base::binary);
    if (windowIconStream.fail())
        Log(Debug::Error) << "Error: Failed to open " << windowIcon;
    osgDB::ReaderWriter* reader = osgDB::Registry::instance()->getReaderWriterForExtension("png");
    if (!reader)
    {
        Log(Debug::Error) << "Error: Failed to read window icon, no png readerwriter found";
        return;
    }
    osgDB::ReaderWriter::ReadResult result = reader->readImage(windowIconStream);
    if (!result.success())
        Log(Debug::Error) << "Error: Failed to read " << windowIcon << ": " << result.message() << " code "
                          << result.status();
    else
    {
        osg::ref_ptr<osg::Image> image = result.getImage();
        auto surface = SDLUtil::imageToSurface(image, true);
        SDL_SetWindowIcon(mWindow, surface.get());
    }
}

void OMW::Engine::prepareEngine()
{
    mStateManager = std::make_unique<MWState::StateManager>(mCfgMgr.getUserDataPath() / "saves", mContentFiles);
    mEnvironment.setStateManager(*mStateManager);

    const bool stereoEnabled = Settings::stereo().mStereoEnabled || osg::DisplaySettings::instance().get()->getStereo();
    mStereoManager = std::make_unique<Stereo::Manager>(
        mViewer, stereoEnabled, Settings::camera().mNearClip, Settings::camera().mViewingDistance);

    osg::ref_ptr<osg::Group> rootNode(new osg::Group);
    mViewer->setSceneData(rootNode);

    createWindow();

    mVFS = std::make_unique<VFS::Manager>();

    VFS::registerArchives(mVFS.get(), mFileCollections, mArchives, true, &mEncoder.get()->getStatelessEncoder());

    const float effectiveResourceCacheExpiry = Settings::RamCache::cacheExpiryDelay();
    Log(Debug::Info) << "V3 RAM cache mode: " << Settings::RamCache::name()
                     << " resource expiry=" << effectiveResourceCacheExpiry << "s"
                     << " preload min/max=" << Settings::RamCache::preloadCellCacheMin() << "/"
                     << Settings::RamCache::preloadCellCacheMax()
                     << " preload expiry=" << Settings::RamCache::preloadCellExpiryDelay() << "s"
                     << " overdrive preload=" << Settings::RamCache::overdrivePreloadName()
                     << " v3.6 profile=" << (Settings::V36Profile::enabled() ? "on" : "off")
                     << " lua-fast=" << (Settings::V36Profile::luaFastPathEnabled() ? "on" : "off")
                     << " coarse-msoc="
                     << (Settings::V36Profile::coarseChunkOcclusionEnabled() ? "on" : "off")
                     << " shape-instance pool=" << Settings::RamCache::shapeInstancePoolSize()
                     << " streaming=" << std::string(Settings::cells().mV3StreamingScheduler)
                     << " prepared instances="
                     << (static_cast<bool>(Settings::cells().mV3PreparedInstanceCache) ? "on" : "off")
                     << "/" << static_cast<int>(Settings::cells().mV3PreparedInstanceCacheMax)
                     << " v3.2 hibernation="
                     << (static_cast<bool>(Settings::cells().mV32ExteriorHibernation) ? "on" : "off")
                     << " gpu telemetry="
                     << (static_cast<bool>(Settings::cells().mV32GpuMemoryTelemetry) ? "on" : "off")
                     << " gpu management="
                     << (static_cast<bool>(Settings::cells().mV32GpuMemoryManagement) ? "on" : "off")
                     << " gpu budget=" << static_cast<int>(Settings::cells().mV32GpuSoftBudgetMb) << "/"
                     << static_cast<int>(Settings::cells().mV32GpuHardBudgetMb) << " MiB";
    mResourceSystem = std::make_unique<Resource::ResourceSystem>(mVFS.get(), effectiveResourceCacheExpiry,
        &mEncoder.get()->getStatelessEncoder(), Settings::RamCache::retainNifFiles());
    mResourceSystem->getSceneManager()->setPreparedInstanceCacheLimit(
        Settings::cells().mV3PreparedInstanceCache
            ? static_cast<std::size_t>(Settings::cells().mV3PreparedInstanceCacheMax)
            : 0u);
    mResourceSystem->getSceneManager()->getShaderManager().setMaxTextureUnits(mGlMaxTextureImageUnits);
    mResourceSystem->getSceneManager()->setUnRefImageDataAfterApply(
        false); // keep to Off for now to allow better state sharing
    mResourceSystem->getSceneManager()->setFilterSettings(Settings::general().mTextureMagFilter,
        Settings::general().mTextureMinFilter, Settings::general().mTextureMipmap,
        static_cast<float>(Settings::general().mAnisotropy));
    mEnvironment.setResourceSystem(*mResourceSystem);

    mWorkQueue = new SceneUtil::WorkQueue(Settings::cells().mPreloadNumThreads);
    mUnrefQueue = std::make_unique<SceneUtil::UnrefQueue>();

    mScreenCaptureOperation = new SceneUtil::AsyncScreenCaptureOperation(mWorkQueue,
        new SceneUtil::WriteScreenshotToFileOperation(mCfgMgr.getScreenshotPath(),
            Settings::general().mScreenshotFormat,
            Settings::general().mNotifyOnSavedScreenshot ? std::function<void(std::string)>(ScreenCaptureMessageBox{})
                                                         : std::function<void(std::string)>(IgnoreString{})));

    mScreenCaptureHandler = new osgViewer::ScreenCaptureHandler(mScreenCaptureOperation);

    mViewer->addEventHandler(mScreenCaptureHandler);

    mL10nManager = std::make_unique<L10n::Manager>(mVFS.get());
    mL10nManager->setPreferredLocales(Settings::general().mPreferredLocales, Settings::general().mGmstOverridesL10n);
    mEnvironment.setL10nManager(*mL10nManager);

    mLuaManager = std::make_unique<MWLua::LuaManager>(mVFS.get(), mResDir / "lua_libs");
    mEnvironment.setLuaManager(*mLuaManager);

    // Create input and UI first to set up a bootstrapping environment for
    // showing a loading screen and keeping the window responsive while doing so

    const auto keybinderUser = mCfgMgr.getUserConfigPath() / "input_v3.xml";
    bool keybinderUserExists = std::filesystem::exists(keybinderUser);
    if (!keybinderUserExists)
    {
        const auto input2 = (mCfgMgr.getUserConfigPath() / "input_v2.xml");
        if (std::filesystem::exists(input2))
        {
            keybinderUserExists = std::filesystem::copy_file(input2, keybinderUser);
            Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;
        }
    }
    else
        Log(Debug::Info) << "Loading keybindings file: " << keybinderUser;

    const auto userdefault = mCfgMgr.getUserConfigPath() / "gamecontrollerdb.txt";
    const auto localdefault = mCfgMgr.getLocalPath() / "gamecontrollerdb.txt";

    std::filesystem::path userGameControllerdb;
    if (std::filesystem::exists(userdefault))
        userGameControllerdb = userdefault;

    std::filesystem::path gameControllerdb;
    if (std::filesystem::exists(localdefault))
        gameControllerdb = localdefault;
    else if (!mCfgMgr.getGlobalPath().empty())
    {
        const auto globaldefault = mCfgMgr.getGlobalPath() / "gamecontrollerdb.txt";
        if (std::filesystem::exists(globaldefault))
            gameControllerdb = globaldefault;
    }
    // else if it doesn't exist, pass in an empty path

    // gui needs our shaders path before everything else
    mResourceSystem->getSceneManager()->setShaderPath(mResDir / "shaders");

    osg::GLExtensions& exts = SceneUtil::getGLExtensions();

#if OSG_VERSION_LESS_THAN(3, 6, 6)
    // hack fix for https://github.com/openscenegraph/OpenSceneGraph/issues/1028
    if (!osg::isGLExtensionSupported(exts.contextID, "NV_framebuffer_multisample_coverage"))
        exts.glRenderbufferStorageMultisampleCoverageNV = nullptr;
#endif

    osg::ref_ptr<osg::Group> guiRoot = new osg::Group;
    guiRoot->setName("GUI Root");
    guiRoot->setNodeMask(MWRender::Mask_GUI);
    mStereoManager->disableStereoForNode(guiRoot);
    rootNode->addChild(guiRoot);

    mWindowManager = std::make_unique<MWGui::WindowManager>(mWindow, mViewer, guiRoot, mResourceSystem.get(),
        mWorkQueue.get(), mCfgMgr.getLogPath(), mScriptConsoleMode, mTranslationDataStorage, mEncoding, mExportFonts,
        Version::getOpenmwVersionDescription(), mCfgMgr);
    mEnvironment.setWindowManager(*mWindowManager);

    mInputManager = std::make_unique<MWInput::InputManager>(mWindow, mViewer, mScreenCaptureHandler, keybinderUser,
        keybinderUserExists, userGameControllerdb, gameControllerdb, mGrab);
    mEnvironment.setInputManager(*mInputManager);

    // Create sound system
    mSoundManager = std::make_unique<MWSound::SoundManager>(mVFS.get(), mUseSound);
    mEnvironment.setSoundManager(*mSoundManager);

    // Create the world
    mWorld = std::make_unique<MWWorld::World>(
        mResourceSystem.get(), mActivationDistanceOverride, mCellName, mCfgMgr.getUserDataPath());
    mEnvironment.setWorld(*mWorld);
    mEnvironment.setWorldModel(mWorld->getWorldModel());
    mEnvironment.setESMStore(mWorld->getStore());

    const MWWorld::Store<ESM::GameSetting>* gmst = &mWorld->getStore().get<ESM::GameSetting>();
    mL10nManager->setGmstLoader([gmst, misses = std::set<std::string, Misc::StringUtils::CiComp>()](
                                    std::string_view gmstName) mutable -> const std::string* {
        const ESM::GameSetting* res = gmst->search(gmstName);
        if (res && res->mValue.getType() == ESM::VT_String)
            return &res->mValue.getString();
        if (misses.emplace(gmstName).second)
            Log(Debug::Error) << "GMST " << gmstName << " not found";
        return nullptr;
    });

    mWindowManager->setStore(mWorld->getStore());

    // Load translation data
    mTranslationDataStorage.setEncoder(mEncoder.get());
    for (auto& mContentFile : mContentFiles)
        mTranslationDataStorage.loadTranslationData(mFileCollections, mContentFile);

    Compiler::registerExtensions(mExtensions);

    // Create script system
    mScriptContext = std::make_unique<MWScript::CompilerContext>(MWScript::CompilerContext::Type_Full);
    mScriptContext->setExtensions(&mExtensions);

    mScriptManager = std::make_unique<MWScript::ScriptManager>(mWorld->getStore(), *mScriptContext, mWarningsMode);
    mEnvironment.setScriptManager(*mScriptManager);

    // Create game mechanics system
    mMechanicsManager = std::make_unique<MWMechanics::MechanicsManager>();
    mEnvironment.setMechanicsManager(*mMechanicsManager);

    // Create dialog system
    mJournal = std::make_unique<MWDialogue::Journal>();
    mEnvironment.setJournal(*mJournal);

    mDialogueManager = std::make_unique<MWDialogue::DialogueManager>(mExtensions, mTranslationDataStorage);
    mEnvironment.setDialogueManager(*mDialogueManager);

    mLuaManager->loadPermanentStorage(mCfgMgr.getUserConfigPath());
    mLuaManager->initPreLoad();

    Loading::Listener* listener = MWBase::Environment::get().getWindowManager()->getLoadingScreen();
    Loading::AsyncListener asyncListener(*listener);
    auto dataLoading = std::async(std::launch::async,
        [&] { mWorld->loadData(mFileCollections, mContentFiles, mGroundcoverFiles, mEncoder.get(), &asyncListener); });

    if (!mSkipMenu)
    {
        std::string_view logo = Fallback::Map::getString("Movies_Company_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, true);
    }

    listener->loadingOn();
    {
        using namespace std::chrono_literals;
        while (dataLoading.wait_for(50ms) != std::future_status::ready)
            asyncListener.update();
        dataLoading.get();
    }
    if (static_cast<bool>(Settings::cells().mV316SfxMetadataFrontload))
    {
        mSoundManager->prepareSfxMetadata();
        // Mode89 has the PCM predecode reservoir enabled. Queue its ESM-backed
        // resource list now so neither enumeration nor queue construction lands
        // on the first ordinary gameplay frame. Modes without predecode return
        // immediately from this call.
        mSoundManager->queueSfxPredecode();
    }
    listener->loadingOff();

    mWorld->init(mMaxRecastLogLevel, mViewer, std::move(rootNode), mWorkQueue.get(), *mUnrefQueue);
    mEnvironment.setWorldScene(mWorld->getWorldScene());
    mWorld->setupPlayer();
    mWorld->setRandomSeed(mRandomSeed);
    mWindowManager->initUI();
    mLuaManager->initPostLoad();

    // scripts
    if (mCompileAll)
    {
        std::pair<int, int> result = mScriptManager->compileAll();
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }
    if (mCompileAllDialogue)
    {
        std::pair<int, int> result = MWDialogue::ScriptTest::compileAll(&mExtensions, mWarningsMode);
        if (result.first)
            Log(Debug::Info) << "compiled " << result.second << " of " << result.first << " dialogue scripts ("
                             << 100 * static_cast<double>(result.second) / result.first << "%)";
    }

    // starts a separate lua thread if "lua num threads" > 0
    mLuaWorker = std::make_unique<MWLua::Worker>(*mLuaManager);
}

// Initialise and enter main loop.
void OMW::Engine::go()
{
    assert(!mContentFiles.empty());

    Log(Debug::Info) << "OSG version: " << osgGetVersion();
    const int sdlVersion = SDL_GetVersion();
    Log(Debug::Info) << "SDL version: " << SDL_VERSIONNUM_MAJOR(sdlVersion) << "."
                     << SDL_VERSIONNUM_MINOR(sdlVersion) << "." << SDL_VERSIONNUM_MICRO(sdlVersion);

    Misc::Rng::init(mRandomSeed);

    Settings::ShaderManager::get().load(mCfgMgr.getUserConfigPath() / "shaders.yaml");

    MWClass::registerClasses();

    // Create encoder
    mEncoder = std::make_unique<ToUTF8::Utf8Encoder>(mEncoding);

    // Setup viewer
    mViewer = new osgViewer::Viewer;
    mViewer->getCamera()->getOrCreateStateSet()->removeAttribute(osg::StateAttribute::MATERIAL);
    SceneUtil::disableFFPStateForRenderer(static_cast<osgViewer::Renderer*>(mViewer->getCamera()->getRenderer()));
    mViewer->setReleaseContextAtEndOfFrameHint(false);

    // Do not try to outsmart the OS thread scheduler (see bug #4785).
    mViewer->setUseConfigureAffinity(false);

    mEnvironment.setFrameRateLimit(Settings::video().mFramerateLimit);

    prepareEngine();

#ifdef _WIN32
    const auto* statsFile = _wgetenv(L"OPENMW_OSG_STATS_FILE");
#else
    const auto* statsFile = std::getenv("OPENMW_OSG_STATS_FILE");
#endif

    std::filesystem::path path;
    if (statsFile != nullptr)
        path = statsFile;

    std::ofstream stats;
    if (!path.empty())
    {
        stats.open(path, std::ios_base::out);
        if (stats.is_open())
            Log(Debug::Info) << "OSG stats will be written to: " << path;
        else
            Log(Debug::Warning) << "Failed to open file to write OSG stats \"" << path
                                << "\": " << std::generic_category().message(errno);
    }

    // Setup profiler
    osg::ref_ptr<Resource::Profiler> statsHandler = new Resource::Profiler(stats.is_open(), *mVFS);

    initStatsHandler(*statsHandler);

    mViewer->addEventHandler(statsHandler);

    osg::ref_ptr<Resource::StatsHandler> resourcesHandler = new Resource::StatsHandler(stats.is_open(), *mVFS);
    mViewer->addEventHandler(resourcesHandler);

    if (stats.is_open())
        Resource::collectStatistics(*mViewer);

    // Start the game
    if (!mSaveGameFile.empty())
    {
        mStateManager->loadGame(mSaveGameFile);
    }
    else if (!mSkipMenu)
    {
        // start in main menu
        mWindowManager->pushGuiMode(MWGui::GM_MainMenu);

        if (mVFS->exists(MWSound::titleMusic))
            mSoundManager->streamMusic(MWSound::titleMusic, MWSound::MusicType::Normal);
        else
            Log(Debug::Warning) << "Title music not found";

        std::string_view logo = Fallback::Map::getString("Movies_Morrowind_Logo");
        if (!logo.empty())
            mWindowManager->playVideo(logo, /*allowSkipping*/ true, /*overrideSounds*/ false);
    }
    else
    {
        mStateManager->newGame(!mNewGame);
    }

    if (!mStartupScript.empty() && mStateManager->getState() == MWState::StateManager::State_Running)
    {
        mWindowManager->executeInConsole(mStartupScript);
    }

    // Start the main rendering loop
    MWWorld::DateTimeManager& timeManager = *mWorld->getTimeManager();
    Misc::FrameRateLimiter frameRateLimiter = Misc::makeFrameRateLimiter(mEnvironment.getFrameRateLimit());
    const std::chrono::steady_clock::duration maxSimulationInterval(std::chrono::milliseconds(200));
    while (!mViewer->done() && !mStateManager->hasQuitRequest())
    {
        const double dt = std::chrono::duration_cast<std::chrono::duration<double>>(
                              std::min(frameRateLimiter.getLastFrameDuration(), maxSimulationInterval))
                              .count()
            * timeManager.getSimulationTimeScale();

        {
            Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
                Debug::V3HitchTelemetry::FrameTailStage::ViewerAdvance);
            mViewer->advance(timeManager.getRenderingSimulationTime());
        }

        const unsigned frameNumber = mViewer->getFrameStamp()->getFrameNumber();

        if (!frame(frameNumber, static_cast<float>(dt)))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        timeManager.updateIsPaused();
        if (!timeManager.isPaused())
        {
            timeManager.setSimulationTime(timeManager.getSimulationTime() + dt);
            timeManager.setRenderingSimulationTime(timeManager.getRenderingSimulationTime() + dt);
        }

        if (stats)
        {
            // The delay is required because rendering happens in parallel to the main thread and stats from there is
            // available with delay.
            constexpr unsigned statsReportDelay = 3;
            if (frameNumber >= statsReportDelay)
            {
                // Viewer frame number can be different from frameNumber because of loading screens which render new
                // frames inside a simulation frame.
                const unsigned currentFrameNumber = mViewer->getFrameStamp()->getFrameNumber();
                for (unsigned i = frameNumber; i <= currentFrameNumber; ++i)
                    reportStats(i - statsReportDelay, *mViewer, stats);
            }
        }

        {
            Debug::V3HitchTelemetry::ScopedFrameTail v33Tail(
                Debug::V3HitchTelemetry::FrameTailStage::FrameLimiter);
            frameRateLimiter.limit();
        }
    }

    mLuaWorker->join();

    // Save user settings
    Settings::Manager::saveUser(mCfgMgr.getUserConfigPath() / "settings.cfg");
    Settings::ShaderManager::get().save();
    mLuaManager->savePermanentStorage(mCfgMgr.getUserConfigPath());
}

void OMW::Engine::setCompileAll(bool all)
{
    mCompileAll = all;
}

void OMW::Engine::setCompileAllDialogue(bool all)
{
    mCompileAllDialogue = all;
}

void OMW::Engine::setSoundUsage(bool soundUsage)
{
    mUseSound = soundUsage;
}

void OMW::Engine::setEncoding(const ToUTF8::FromType& encoding)
{
    mEncoding = encoding;
}

void OMW::Engine::setScriptConsoleMode(bool enabled)
{
    mScriptConsoleMode = enabled;
}

void OMW::Engine::setStartupScript(const std::filesystem::path& path)
{
    mStartupScript = path;
}

void OMW::Engine::setActivationDistanceOverride(int distance)
{
    mActivationDistanceOverride = distance;
}

void OMW::Engine::setWarningsMode(int mode)
{
    mWarningsMode = mode;
}

void OMW::Engine::enableFontExport(bool exportFonts)
{
    mExportFonts = exportFonts;
}

void OMW::Engine::setSaveGameFile(const std::filesystem::path& savegame)
{
    mSaveGameFile = savegame;
}

void OMW::Engine::setRandomSeed(unsigned int seed)
{
    mRandomSeed = seed;
}
