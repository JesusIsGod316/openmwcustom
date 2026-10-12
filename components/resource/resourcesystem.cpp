#include "resourcesystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <typeinfo>
#include "cachemaintenance.hpp"
#include "loadingnifmaintenance.hpp"
#include "visualcachehandoff.hpp"

#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/misc/thread.hpp>
#include <components/sceneutil/workqueue.hpp>
#include <components/sceneutil/workqueuepublication.hpp>

#include "animblendrulesmanager.hpp"
#include "bgsmfilemanager.hpp"
#include "imagemanager.hpp"
#include "keyframemanager.hpp"
#include "niffilemanager.hpp"
#include "scenemanager.hpp"

namespace Resource
{
    struct VisualCacheHandoffState
    {
        explicit VisualCacheHandoffState(bool enabled) : policy(enabled), owner(policy) {}
        VisualCacheHandoffPolicy policy;
        VisualCacheHandoffQueueOwner owner;
        std::atomic<std::uint64_t> queued{0}, completed{0}, skipped{0}, errors{0};
        std::atomic<std::uint64_t> enqueueErrors{0};
        std::atomic<std::uint64_t> templates{0}, states{0}, images{0}, scanned{0};
    };
    struct LoadingNifMaintenanceState
    {
        explicit LoadingNifMaintenanceState(bool enabled) : policy(enabled), owner(policy) {}
        LoadingNifMaintenancePolicy policy;
        LoadingNifMaintenanceQueueOwner owner;
        std::atomic<std::uint64_t> requests{ 0 }, queued{ 0 }, completed{ 0 }, skipped{ 0 }, errors{ 0 };
        std::atomic<std::uint64_t> scanned{ 0 }, removed{ 0 }, releasedCharges{ 0 };
        std::array<std::atomic<std::uint64_t>,
            static_cast<unsigned>(LoadingNifMaintenancePolicy::Refusal::PhaseExhausted) + 1> refusals{};
        std::atomic<bool> loadingRequested{ false }, queueAttached{ false }, pressureCensusClaimed{ false };
        std::atomic<std::uint64_t> observationSerial{ 0 };
        std::atomic<unsigned> loadingOwnerObservations{ 0 };
        std::uint64_t queueDiagnosticFailures = 0;
    };

    namespace
    {
        class VisualCacheHandoffItem final : public SceneUtil::WorkItem
        {
        public:
            VisualCacheHandoffItem(ResourceSystem& resources, VisualCacheHandoffState& state,
                VisualCacheHandoffPolicy::Admission token) : mResources(resources), mState(state), mToken(token) {}
            void doWork() override
            {
                const auto outcome = mState.policy.runOwned(mToken, OpenGlPressureMonitor::milliseconds, [&]{
                    Misc::setCurrentThreadIdlePriority();
                    Debug::HitchTrace::Scope work("resource.unused_visual_handoff");
                    const auto result = mResources.getSceneManager()->retireUnusedVisualHandoff();
                    mState.templates.fetch_add(result.templates); mState.states.fetch_add(result.states);
                    mState.scanned.fetch_add(result.scanned);
                    CacheMaintenanceBudget budget(128, 4, std::chrono::microseconds(500));
                    {
                        CacheMaintenanceScope scope(budget);
                        mState.images.fetch_add(mResources.getImageManager()->trimCache(4));
                    }
                    mState.scanned.fetch_add(budget.scanned);
                    if (Debug::RuntimeDiagnostics::enabled())
                        Debug::RuntimeDiagnostics::recordEvent("visual_cache_handoff", "cache_only", "counts_not_RAM_savings", {
                            {"generation",mToken.generation}, {"templates",result.templates}, {"shared_states",result.states},
                            {"images",budget.released}, {"scanned",result.scanned+budget.scanned} });
                });
                if (outcome==VisualCacheHandoffPolicy::Outcome::Completed) ++mState.completed;
                else if (outcome==VisualCacheHandoffPolicy::Outcome::Skipped) ++mState.skipped;
                else ++mState.errors;
            }
        private:
            ResourceSystem& mResources;
            VisualCacheHandoffState& mState;
            VisualCacheHandoffPolicy::Admission mToken;
        };
        void reportLoadingNifObservation(LoadingNifMaintenanceState& s, std::string_view reason) noexcept
        {
            if (!Debug::RuntimeDiagnostics::enabled()) return;
            auto serial = s.observationSerial.load();
            do
            {
                if (serial == std::numeric_limits<std::uint64_t>::max()) return;
            } while (!s.observationSerial.compare_exchange_weak(serial, serial + 1));
            const auto observedAt = Debug::RuntimeDiagnostics::nowUs();
            using Refusal = LoadingNifMaintenancePolicy::Refusal;
            const auto refused = [&](Refusal reason) { return s.refusals[static_cast<unsigned>(reason)].load(); };
            // Independently read atomics: a live observation, not a coherent
            // permission grant or a normal-shutdown/completeness footer.
            Debug::RuntimeDiagnostics::recordEvent("loading_nif_lifecycle", "parsed_nif", reason, {
                {"observation", serial + 1}, {"instance", reinterpret_cast<std::uintptr_t>(&s)},
                {"observed_us", observedAt},
                {"enabled", s.policy.enabled()}, {"loading_requested_observed", s.loadingRequested.load()},
                {"queue_attached_observed", s.queueAttached.load()}, {"pending_observed", s.policy.pending()},
                {"requests", s.requests.load()}, {"queued", s.queued.load()}, {"completed", s.completed.load()},
                {"skipped", s.skipped.load()}, {"errors", s.errors.load()}, {"scanned", s.scanned.load()},
                {"removed", s.removed.load()}, {"released_charge_bytes", s.releasedCharges.load()} });
            // Recorder records hold at most16 numeric fields. Keep refusal
            // coverage separate and join by this exact owner/observation pair;
            // loss of either half is incomplete, never an implicit zero.
            Debug::RuntimeDiagnostics::recordEvent("loading_nif_refusals", "parsed_nif", reason, {
                {"observation", serial + 1}, {"instance", reinterpret_cast<std::uintptr_t>(&s)},
                {"observed_us", observedAt},
                {"refused_inactive", refused(Refusal::Inactive)}, {"refused_missing_queue", refused(Refusal::MissingQueue)},
                {"refused_sample", refused(Refusal::Sample)}, {"refused_healthy", refused(Refusal::Healthy)},
                {"refused_busy", refused(Refusal::Busy)}, {"refused_cadence", refused(Refusal::Cadence)},
                {"refused_phase_exhausted", refused(Refusal::PhaseExhausted)} });
        }
        class LoadingNifMaintenanceItem final : public SceneUtil::WorkItem
        {
        public:
            LoadingNifMaintenanceItem(ResourceSystem& resources, LoadingNifMaintenanceState& state,
                std::uint64_t phase, double referenceTime)
                : mResources(resources), mState(state), mPhase(phase), mReferenceTime(referenceTime) {}
            void doWork() override
            {
                const auto outcome = mState.policy.runOwned(mPhase, [&]
                {
                    Misc::setCurrentThreadIdlePriority();
                    const auto sample = ResourceSystem::speculativeSample(&mResources);
                    const auto now = OpenGlPressureMonitor::milliseconds();
                    if (!mState.policy.canRun(mPhase, sample, now))
                    {
                        return LoadingNifMaintenancePolicy::Outcome::Skipped;
                    }
                    Debug::HitchTrace::Scope work("resource.loading_parsed_nif_maintenance");
                    CacheMaintenanceBudget budget(128, 8, std::chrono::microseconds(1000));
                    {
                        CacheMaintenanceScope scope(budget);
                        // This manager alone: no scene/image/terrain publication,
                        // parser call, new thread, or wait on a required producer.
                        mResources.getNifFileManager()->trimUnusedForLoading(8);
                    }
                    ++mState.completed;
                    mState.scanned.fetch_add(budget.scanned, std::memory_order_relaxed);
                    mState.removed.fetch_add(budget.released, std::memory_order_relaxed);
                    mState.releasedCharges.fetch_add(budget.releasedBytes, std::memory_order_relaxed);
                    if (Debug::RuntimeDiagnostics::enabled())
                    {
                        Debug::RuntimeDiagnostics::recordEvent("loading_nif_maintenance", "parsed_nif",
                            "Cache charges; not process RAM or a required-load ceiling", {
                                {"phase", mPhase}, {"sample_generation", sample.generation},
                                {"sample_age_ms", now - sample.sampledAtMs}, {"scanned", budget.scanned},
                                {"removed", budget.released}, {"released_charge_bytes", budget.releasedBytes} });
                        // Diagnostic census is separately timed by its existing
                        // bounded observer and does not grant trim permission.
                        mResources.getNifFileManager()->reportRuntimeDiagnostics(mReferenceTime);
                    }
                    return LoadingNifMaintenancePolicy::Outcome::Completed;
                });
                if (outcome == LoadingNifMaintenancePolicy::Outcome::Skipped) ++mState.skipped;
                if (outcome == LoadingNifMaintenancePolicy::Outcome::Error) ++mState.errors;
                reportLoadingNifObservation(mState, "worker_terminal");
            }
        private:
            ResourceSystem& mResources;
            LoadingNifMaintenanceState& mState;
            std::uint64_t mPhase;
            double mReferenceTime;
        };
    }

    ResourceSystem::ResourceSystem(const VFS::Manager* vfs, double expiryDelay,
        const ToUTF8::StatelessUtf8Encoder* encoder, bool retainNifFiles)
        : mVFS(vfs)
        , mRetainNifFiles(retainNifFiles)
    {
        mNifFileManager = std::make_unique<NifFileManager>(vfs, encoder);
        if (const char* flag=std::getenv("OPENMW_RAM_C2_PARSED_NIF"); flag && std::strcmp(flag,"1")==0)
        {
            const char* v2=std::getenv("OPENMW_RAM_BUDGET_V2");
            mNifFileManager->setColdRetentionSeconds(30.0,v2 && std::strcmp(v2,"1")==0);
        }
        if (mRetainNifFiles)
            mNifFileManager->setExpiryDelay(expiryDelay);
        mBgsmFileManager = std::make_unique<BgsmFileManager>(vfs, expiryDelay);
        mImageManager = std::make_unique<ImageManager>(vfs, expiryDelay);
        mSceneManager = std::make_unique<SceneManager>(
            vfs, mImageManager.get(), mNifFileManager.get(), mBgsmFileManager.get(), expiryDelay);
        mSceneManager->setHostMemoryBudget(&mHostMemoryBudget);
        mKeyframeManager = std::make_unique<KeyframeManager>(vfs, mSceneManager.get(), expiryDelay, encoder);
        mAnimBlendRulesManager = std::make_unique<AnimBlendRulesManager>(vfs, expiryDelay);

        addResourceManager(mNifFileManager.get());
        addResourceManager(mBgsmFileManager.get());
        addResourceManager(mKeyframeManager.get());
        // note, scene references images so add images afterwards for correct implementation of updateCache()
        addResourceManager(mSceneManager.get());
        addResourceManager(mImageManager.get());
        addResourceManager(mAnimBlendRulesManager.get());
    }

    ResourceSystem::~ResourceSystem()
    {
        if (mVisualCacheHandoff)
        {
            auto& s=*mVisualCacheHandoff;
            s.owner.detach(); // Join before any manager/ledger lifetime ends.
            if (s.policy.enabled())
                Log(Debug::Info) << "OptimizedMW unused visual handoff: enabled=1 queued=" << s.queued.load()
                    << " completed=" << s.completed.load() << " skipped=" << s.skipped.load() << " errors=" << s.errors.load()
                    << " enqueue_errors=" << s.enqueueErrors.load()
                    << " templates=" << s.templates.load() << " shared_states=" << s.states.load()
                    << " images=" << s.images.load() << " scanned=" << s.scanned.load()
                    << " scope=cache_only_owner_counts_not_process_RAM_savings";
        }
        // this has to be defined in the .cpp file as we can't delete incomplete types

        // Drain only this owner's task before any borrowed manager/ledger dies.
        detachLoadingNifMaintenanceQueue();
        if (mLoadingNifMaintenance)
        {
            const auto& s = *mLoadingNifMaintenance;
            Log(Debug::Info) << "OptimizedMW loading NIF maintenance: enabled=" << s.policy.enabled()
                << " requests=" << s.requests.load() << " queued=" << s.queued.load() << " completed=" << s.completed.load()
                << " skipped=" << s.skipped.load() << " errors=" << s.errors.load()
                << " scanned=" << s.scanned.load() << " removed=" << s.removed.load()
                << " released_charge_bytes=" << s.releasedCharges.load()
                << " enqueue_diagnostic_failures_all_queues=" << s.queueDiagnosticFailures
                << " scope=parsed_cache_only_not_process_RAM";
        }
        // Engine teardown has already joined preload/maintenance workers.
        mDeferredRelease.clear();
        mResourceManagers.clear();
    }

    void ResourceSystem::configureLoadingNifMaintenance(bool enabled)
    {
        // Called once before loader workers/queue attachment. Reconfiguration
        // must not replace a state referenced by a queued item.
        if (mLoadingNifMaintenance)
            throw std::logic_error("Loading NIF maintenance configured twice");
        mLoadingNifMaintenance = std::make_unique<LoadingNifMaintenanceState>(enabled);
        mNifFileManager->setLoadingTrimEnabled(enabled);
        Log(Debug::Info) << "OptimizedMW loading NIF maintenance policy: enabled=" << enabled
            << " pressure_only=1 existing_idle_worker=1 sample_max_age_ms="
            << LoadingNifMaintenancePolicy::MaximumSampleAgeMilliseconds;
    }

    void ResourceSystem::attachLoadingNifMaintenanceQueue(SceneUtil::WorkQueue* queue)
    {
        if (visualCacheHandoffEnabled() && queue) mVisualCacheHandoff->owner.attach(queue);
        if (mLoadingNifMaintenance && mLoadingNifMaintenance->policy.enabled() && mSpeculativeBudget && queue)
        {
            mLoadingNifMaintenance->owner.attach(queue);
            mLoadingNifMaintenance->queueAttached.store(true);
        }
    }

    void ResourceSystem::detachLoadingNifMaintenanceQueue(SceneUtil::WorkQueue* queue)
    {
        if (mVisualCacheHandoff) mVisualCacheHandoff->owner.detach(queue);
        if (!mLoadingNifMaintenance) return;
        auto& state = *mLoadingNifMaintenance;
        if (state.owner.detach(queue))
        {
            state.queueAttached.store(false);
            state.loadingRequested.store(false);
            state.queueDiagnosticFailures = SceneUtil::workQueuePublicationDiagnosticFailures();
        }
    }

    void ResourceSystem::setLoadingNifMaintenanceActive(bool active) noexcept
    {
        if (visualCacheHandoffEnabled())
            mVisualCacheHandoff->policy.setLoading(active, OpenGlPressureMonitor::milliseconds());
        if (!mLoadingNifMaintenance) return;
        auto& state = *mLoadingNifMaintenance;
        state.policy.setLoading(active);
        const bool previous = state.loadingRequested.exchange(active);
        if (previous != active) reportLoadingNifObservation(state, active ? "loading_on" : "loading_off");
    }

    void ResourceSystem::configureVisualCacheHandoff(bool enabled)
    {
        if (mVisualCacheHandoff) throw std::logic_error("Visual cache handoff configured twice");
        mVisualCacheHandoff=std::make_unique<VisualCacheHandoffState>(enabled);
        Log(Debug::Info) << "OptimizedMW unused visual handoff policy: enabled=" << enabled
            << " existing_idle_worker=1 max_passes=" << VisualCacheHandoffPolicy::MaximumPasses
            << " window_ms=" << VisualCacheHandoffPolicy::WindowMilliseconds
            << " live_clones_protected=1 prepared_instances_preserved=1";
    }
    bool ResourceSystem::visualCacheHandoffEnabled() const noexcept
    { return mVisualCacheHandoff && mVisualCacheHandoff->policy.enabled(); }
    void ResourceSystem::requestVisualCacheHandoff() noexcept
    {
        if (!visualCacheHandoffEnabled()) return;
        auto& s=*mVisualCacheHandoff;
        const auto token=s.policy.tryBegin(OpenGlPressureMonitor::milliseconds(),s.owner.available() && !s.owner.busy());
        if (!token) return;
        try { s.owner.publish(new VisualCacheHandoffItem(*this,s,token)); ++s.queued; }
        catch (...) { s.policy.finish(token); ++s.enqueueErrors; }
    }

    void ResourceSystem::reportLoadingRequiredOwners(double referenceTime) noexcept
    {
        if (!Debug::RuntimeDiagnostics::enabled() || !mLoadingNifMaintenance) return;
        if (!std::isfinite(referenceTime) || referenceTime < 0
            || referenceTime >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) / 1000000.0) return;
        auto& count = mLoadingNifMaintenance->loadingOwnerObservations;
        auto observed = count.load(std::memory_order_relaxed);
        do
        {
            if (observed >= 4) return; // Startup plus early transitions only.
        } while (!count.compare_exchange_weak(observed, observed + 1, std::memory_order_relaxed));
        const auto start = Debug::RuntimeDiagnostics::nowUs();
        Debug::RuntimeDiagnostics::recordEvent("loading_required_owner_census", "images_and_scene",
            "loading_off_requests_only_not_complete_allocation_attribution", {
                {"observation", observed + 1}, {"report_requests", 2},
                {"reference_clock_us", static_cast<std::uint64_t>(referenceTime * 1000000.0)} });
        // Reporters have atomic cadence admission and bounded cache try-locks.
        // Busy/limited/suppressed/lost output remains incomplete. Do not spend
        // the release allowance or mutate a cache to obtain this observation.
        mSceneManager->reportRuntimeDiagnostics(referenceTime);
        mImageManager->reportRuntimeDiagnostics(referenceTime);
        Debug::RuntimeDiagnostics::recordEvent("probe_cost", "loading_required_owners",
            "Bounded loading boundary census requests; not a normal frame performance comparison", {
                {"observation", observed + 1}, {"elapsed_us", Debug::RuntimeDiagnostics::nowUs() - start} });
    }

    void ResourceSystem::requestLoadingNifMaintenance(double referenceTime) noexcept
    {
        if (!mLoadingNifMaintenance || !mLoadingNifMaintenance->policy.enabled()) return;
        auto& state = *mLoadingNifMaintenance;
        ++state.requests;
        if (state.owner.busy())
        {
            ++state.refusals[static_cast<unsigned>(LoadingNifMaintenancePolicy::Refusal::Busy)];
            return;
        }
        LoadingNifMaintenancePolicy::Admission admission;
        try
        {
            admission = state.policy.tryBegin(speculativeSample(this), OpenGlPressureMonitor::milliseconds(),
                state.owner.available());
            if (!admission)
            {
                ++state.refusals[static_cast<unsigned>(admission.refusal)];
                return;
            }
            state.owner.publish(new LoadingNifMaintenanceItem(*this, state, admission.phase, referenceTime));
            ++state.queued;
        }
        catch (...)
        {
            // A failed sample read or pre-publication allocation drops only
            // this admission. Post-publication queue diagnostics do not throw
            // and never make the published item ambiguous or re-enqueued.
            state.policy.finish(admission.phase);
            ++state.errors;
        }
    }

    SceneManager* ResourceSystem::getSceneManager()
    {
        return mSceneManager.get();
    }

    ImageManager* ResourceSystem::getImageManager()
    {
        return mImageManager.get();
    }

    BgsmFileManager* ResourceSystem::getBgsmFileManager()
    {
        return mBgsmFileManager.get();
    }

    NifFileManager* ResourceSystem::getNifFileManager()
    {
        return mNifFileManager.get();
    }

    KeyframeManager* ResourceSystem::getKeyframeManager()
    {
        return mKeyframeManager.get();
    }

    AnimBlendRulesManager* ResourceSystem::getAnimBlendRulesManager()
    {
        return mAnimBlendRulesManager.get();
    }

    void ResourceSystem::configureVisualCacheRetention(double imageSeconds, double sceneSeconds)
    {
        if (mVisualCacheRetentionConfigured)
            throw std::logic_error("Visual cache retention configured twice");
        if (!std::isfinite(imageSeconds) || !std::isfinite(sceneSeconds)
            || imageSeconds < 0 || sceneSeconds < 0 || imageSeconds > 3600 || sceneSeconds > 3600)
            throw std::invalid_argument("Invalid unused visual cache residence time");
        mUnusedImageExpiry = imageSeconds;
        mUnusedSceneExpiry = sceneSeconds;
        mVisualCacheRetentionConfigured = true;
        if (mUnusedImageExpiry > 0)
            mImageManager->setExpiryDelay(std::min(mImageManager->getExpiryDelay(), mUnusedImageExpiry));
        if (mUnusedSceneExpiry > 0)
            mSceneManager->setExpiryDelay(std::min(mSceneManager->getExpiryDelay(), mUnusedSceneExpiry));
        Log(Debug::Info) << "OptimizedMW unused visual cache residence: image_seconds="
            << mImageManager->getExpiryDelay() << " scene_seconds=" << mSceneManager->getExpiryDelay()
            << " image_override=" << (mUnusedImageExpiry > 0)
            << " scene_override=" << (mUnusedSceneExpiry > 0)
            << " scope=unused_cache_residence_not_live_owner_or_process_memory_cap";
    }

    void ResourceSystem::setExpiryDelay(double expiryDelay)
    {
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->setExpiryDelay(expiryDelay);

        // Upstream drops parsed NIFs immediately. Overdrive intentionally retains them so paging and
        // collision paths can reuse already-parsed source data during long sessions.
        mNifFileManager->setExpiryDelay(mRetainNifFiles ? expiryDelay : 0.0);
        // A later common-cache setting change must not silently erase either
        // explicitly configured visual residence policy. Preserve shorter
        // common cleanup requests, including the developer mass-cell test.
        if (mUnusedImageExpiry > 0) mImageManager->setExpiryDelay(std::min(expiryDelay, mUnusedImageExpiry));
        if (mUnusedSceneExpiry > 0) mSceneManager->setExpiryDelay(std::min(expiryDelay, mUnusedSceneExpiry));
    }

    void ResourceSystem::updateCache(double referenceTime)
    {
        if (mSpeculativeBudget) { updateBudgetedCache(referenceTime); return; }
        Debug::HitchTrace::Scope maintenance("resource.cache_maintenance", Debug::HitchTrace::CpuTiming::WallEnvelope);
        Debug::V3Diagnostics::TraceScope trace("resource", "resource_cache_update", "all_managers", 0.5);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::resourceWriter(), "resource_cache_update", "all_managers", 0.5);
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
        {
            {
                Debug::HitchTrace::Scope managerUpdate("resource.cache_manager_update",
                    Debug::HitchTrace::CpuTiming::WallEnvelope,
                    Debug::HitchTrace::requested() ? typeid(**it).name() : "");
                (*it)->updateCache(referenceTime);
            }
            (*it)->reportRuntimeDiagnostics(referenceTime);
        }
        const auto pressure = mHostMemoryBudget.pressure();
        if (pressure != HostMemoryPressure::Normal)
        {
            const std::size_t maximum = pressure == HostMemoryPressure::Critical ? 512 : 128;
            std::size_t removed = 0;
            const auto trim = [&](BaseResourceManager* manager) {
                Debug::HitchTrace::Scope pressureTrim("resource.cache_manager_pressure_trim",
                    Debug::HitchTrace::CpuTiming::WallEnvelope,
                    Debug::HitchTrace::requested() ? typeid(*manager).name() : "");
                const auto count = manager->trimCache(maximum);
                pressureTrim.setMetric(0, "removed", count);
                return count;
            };
            // Retaining owners (paging, collision, etc.) first, then templates
            // and shared state, and finally images. Never clear a live graph.
            for (BaseResourceManager* manager : mResourceManagers)
                if (manager != mSceneManager.get() && manager != mImageManager.get())
                    removed += trim(manager);
            removed += trim(mSceneManager.get());
            removed += trim(mImageManager.get());
            if (Debug::RuntimeDiagnostics::enabled())
            {
                const auto memory = mHostMemoryBudget.snapshot();
                auto limits = HostMemoryPolicy::limits(memory.physicalTotal);
                if (mHostMemoryBudget.openGlEnabled())
                {
                    const auto gl = mHostMemoryBudget.openGlSample();
                    limits.reserve = gl.decision.limits.physicalReserve;
                    limits.privateSoft = 0; // no automatic private/RAM cap in GL-P1A
                }
                Debug::RuntimeDiagnostics::recordEvent("host_memory_trim", "optional_resource_owners", {}, {
                    {"pressure", static_cast<std::uint64_t>(pressure)}, {"removed_entries", removed},
                    {"per_owner_limit", maximum}, {"physical_valid", memory.physicalValid},
                    {"physical_available_bytes", memory.physicalAvailable},
                    {"private_commit_bytes", memory.privateCommit}, {"process_valid", memory.processValid},
                    {"commit_available_bytes", memory.commitAvailable}, {"commit_valid", memory.commitValid},
                    {"reserve_bytes", limits.reserve}, {"private_soft_bytes", limits.privateSoft}});
            }
        }
    }

    void ResourceSystem::clearCache()
    {
        for (std::vector<BaseResourceManager*>::iterator it = mResourceManagers.begin(); it != mResourceManagers.end();
             ++it)
            (*it)->clearCache();
    }

    void ResourceSystem::addResourceManager(BaseResourceManager* resourceMgr)
    {
        mResourceManagers.push_back(resourceMgr);
        if (mSpeculativeBudget)
        { resourceMgr->setSpeculativeBudget(mSpeculativeBudget.get()); rebuildMaintenanceOrder(); }
    }

    void ResourceSystem::removeResourceManager(BaseResourceManager* resourceMgr)
    {
        std::vector<BaseResourceManager*>::iterator found
            = std::find(mResourceManagers.begin(), mResourceManagers.end(), resourceMgr);
        if (found != mResourceManagers.end())
        { mResourceManagers.erase(found); if (mSpeculativeBudget) rebuildMaintenanceOrder(); }
    }


    void ResourceSystem::enableOpenGlSpeculativeBudget(OpenGlPressureConfig pressure, SpeculativeBudget::Config config)
    {
        mSpeculativeBudget = std::make_unique<SpeculativeBudget>(config);
        mHostMemoryBudget.enableOpenGl(pressure, mSpeculativeBudget->watermark());
        for (auto* manager : mResourceManagers) manager->setSpeculativeBudget(mSpeculativeBudget.get());
        if (const char* flag=std::getenv("OPENMW_RAM_C5_COLD_IMAGES"); flag && std::strcmp(flag,"1")==0)
        {
            ColdImageCache::Config cold;
            cold.enabled=true;
            mImageManager->configureColdCache(cold,&ResourceSystem::speculativeSample,this);
        }
        rebuildMaintenanceOrder();
    }

    void ResourceSystem::rebuildMaintenanceOrder()
    {
        mMaintenanceOrder.clear();
        for (auto* manager : mResourceManagers)
            if (manager != mSceneManager.get() && manager != mImageManager.get()) mMaintenanceOrder.push_back(manager);
        mMaintenanceOrder.push_back(mSceneManager.get());
        mMaintenanceOrder.push_back(mImageManager.get());
        mMaintenanceCursor = 0;
    }

    std::size_t ResourceSystem::pendingReleases() const
    { return mDeferredRelease.stats().owners; }

    void ResourceSystem::updateBudgetedCache(double referenceTime)
    {
        Debug::HitchTrace::Scope maintenance("resource.cache_maintenance", Debug::HitchTrace::CpuTiming::WallEnvelope);
        CacheMaintenanceBudget budget;
        std::array<BaseResourceManager*, 16> observedManagers{};
        std::size_t observed = 0;
        {
            CacheMaintenanceScope scope(budget);
            // This executes on the existing dedicated resource worker. Never wait
            // for a running preload and never perform GL finish/wait here.
            {
                Debug::HitchTrace::Scope deferred("resource.deferred_owner_release");
                mDeferredRelease.drain(budget);
                deferred.setMetric(0, "scanned", budget.scanned);
                deferred.setMetric(1, "released", budget.released);
                deferred.setMetric(2, "estimated_bytes", budget.releasedBytes);
            }
            const auto pressure = mHostMemoryBudget.pressure();
            // Rotating start avoids starvation when a manager or a destructor uses
            // the remaining time. Within a cycle owners precede templates/images.
            for (std::size_t visited = 0; visited < mMaintenanceOrder.size() && budget.available(); ++visited)
            {
                auto* manager = mMaintenanceOrder[mMaintenanceCursor];
                mMaintenanceCursor = (mMaintenanceCursor + 1) % mMaintenanceOrder.size();
                {
                    Debug::HitchTrace::Scope managerUpdate("resource.cache_manager_update",
                        Debug::HitchTrace::CpuTiming::WallEnvelope,
                        Debug::HitchTrace::requested() ? typeid(*manager).name() : "");
                    manager->updateCache(referenceTime);
                }
                if (Debug::RuntimeDiagnostics::enabled() && observed < observedManagers.size())
                    observedManagers[observed++] = manager;
                if (pressure != HostMemoryPressure::Normal && budget.available())
                {
                    Debug::HitchTrace::Scope pressureTrim("resource.cache_manager_pressure_trim",
                        Debug::HitchTrace::CpuTiming::WallEnvelope,
                        Debug::HitchTrace::requested() ? typeid(*manager).name() : "");
                    pressureTrim.setMetric(0, "removed", manager->trimCache(8));
                }
            }
        }
        // Observe outside the release budget so diagnostic time cannot consume
        // the manager's remaining trim allowance. Each existing census is
        // sampled/bounded and reports its own cost/coverage separately.
        for (std::size_t i = 0; i < observed; ++i)
            observedManagers[i]->reportRuntimeDiagnostics(referenceTime);
        if (Debug::RuntimeDiagnostics::enabled() && mLoadingNifMaintenance)
        {
            const auto sample = speculativeSample(this);
            if (LoadingNifMaintenancePolicy::fresh(sample, OpenGlPressureMonitor::milliseconds())
                && LoadingNifMaintenancePolicy::observedPressure(sample)
                && !mLoadingNifMaintenance->pressureCensusClaimed.exchange(true))
            {
                // The ordinary release sweep can exhaust its budget before
                // visiting these required owners. Request each existing bounded
                // try-lock census independently; never evict or increase the
                // release allowance to obtain diagnostic coverage.
                const auto start = Debug::RuntimeDiagnostics::nowUs();
                reportLoadingNifObservation(*mLoadingNifMaintenance, "first_observed_pressure");
                for (BaseResourceManager* manager : { static_cast<BaseResourceManager*>(mSceneManager.get()),
                         static_cast<BaseResourceManager*>(mImageManager.get()) })
                    if (std::find(observedManagers.begin(), observedManagers.begin() + observed, manager)
                        == observedManagers.begin() + observed)
                        manager->reportRuntimeDiagnostics(referenceTime);
                Debug::RuntimeDiagnostics::recordEvent("probe_cost", "pressure_required_owners",
                    "One-shot sampled census requests; busy/limited/lost output remains incomplete", {
                        {"elapsed_us", Debug::RuntimeDiagnostics::nowUs() - start},
                        {"sample_generation", sample.generation} });
            }
        }
        maintenance.setMetric(0, "scanned", budget.scanned);
        maintenance.setMetric(1, "released", budget.released);
        maintenance.setMetric(2, "estimated_bytes", budget.releasedBytes);
        if (Debug::RuntimeDiagnostics::enabled())
        {
            const auto stats = mSpeculativeBudget->stats();
            Debug::RuntimeDiagnostics::recordEvent("opimizedmw_memory", "p1b", "Owner charges; not total process/VRAM bytes", {
                {"future_bytes", stats.futureBytes}, {"known_owner_bytes", stats.knownOwnerBytes},
                {"estimated_owner_bytes", stats.estimatedOwnerBytes}, {"unknown_owners", stats.unknownOwners},
                {"pending_owner_bytes", stats.pendingOwnerBytes}, {"jobs", stats.jobs}, {"denied", stats.denied},
                {"duplicate_defers", stats.duplicateRequests}, {"shared_claims", stats.sharedClaims},
                {"demand_hits", stats.demandHits}, {"prefetch_hits", stats.prefetchHits},
                {"scanned", budget.scanned}, {"released", budget.released}, {"pending_owners", pendingReleases()} });
            Debug::RuntimeDiagnostics::recordEvent("opimizedmw_priority", "p1b",
                "Near-future lane is reserved for active-grid strong upgrades", {
                    {"priority_jobs", stats.priorityJobs},
                    {"near_future_admitted", stats.nearFutureAdmitted},
                    {"near_future_denied", stats.nearFutureDenied} });
        }
    }

    const VFS::Manager* ResourceSystem::getVFS() const
    {
        return mVFS;
    }

    void ResourceSystem::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->reportStats(frameNumber, stats);
    }

    void ResourceSystem::releaseGLObjects(osg::State* state)
    {
        for (std::vector<BaseResourceManager*>::const_iterator it = mResourceManagers.begin();
             it != mResourceManagers.end(); ++it)
            (*it)->releaseGLObjects(state);
    }

}
