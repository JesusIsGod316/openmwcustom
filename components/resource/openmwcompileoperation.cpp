#include "openmwcompileoperation.hpp"

#include "v321classifiedcompileset.hpp"

#include <components/debug/v3diagnostics.hpp>

#include <osg/GraphicsContext>
#include <osg/Timer>

#include <OpenThreads/ScopedLock>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace
{
    unsigned int frameNumberFor(osg::GraphicsContext* context)
    {
        if (context && context->getState() && context->getState()->getFrameStamp())
            return context->getState()->getFrameStamp()->getFrameNumber();
        return 0;
    }

    void writeP4CompileRow(unsigned int frame, std::string_view event, std::string_view kind,
        std::string_view compileClass, std::size_t queueDepth, unsigned int oldestAgeFrames,
        double budgetMs, double creditMs, double predictedMs, double actualMs,
        double headroomMs, double lastHandoffMs, unsigned int objects, std::string_view detail)
    {
        auto& writer = Debug::V3Diagnostics::compileWriter();
        if (!writer.enabled())
            return;

        std::ostringstream row;
        row << frame << ',' << Debug::V3Diagnostics::epochMs() << ','
            << Debug::V3Diagnostics::csvQuote(event) << ','
            << Debug::V3Diagnostics::csvQuote(kind) << ','
            << Debug::V3Diagnostics::csvQuote(compileClass) << ','
            << queueDepth << ',' << oldestAgeFrames << ','
            << std::fixed << std::setprecision(3)
            << budgetMs << ',' << creditMs << ',' << predictedMs << ',' << actualMs << ','
            << headroomMs << ',' << lastHandoffMs << ',' << objects << ','
            << Debug::V3Diagnostics::csvQuote(detail);
        writer.writeLine(row.str());
    }
}

namespace Resource
{
    OpenMWIncrementalCompileOperation::OpenMWIncrementalCompileOperation(OpenMWCompileSchedulerConfig config)
        : mConfig(std::move(config))
    {
    }

    void OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(double value) noexcept
    {
        sLastRenderingTraversalMs.store(std::max(0.0, value), std::memory_order_release);
    }

    double OpenMWIncrementalCompileOperation::lastRenderingTraversalMs() noexcept
    {
        return sLastRenderingTraversalMs.load(std::memory_order_acquire);
    }

    OpenMWIncrementalCompileOperation::CompileKind OpenMWIncrementalCompileOperation::classify(const CompileOp* op) const
    {
        if (dynamic_cast<const CompileDrawableOp*>(op)
            || dynamic_cast<const OpenMWDrawableCompileOp*>(op))
            return CompileKind::Drawable;
        if (dynamic_cast<const CompileTextureOp*>(op))
            return CompileKind::Texture;
        if (dynamic_cast<const CompileProgramOp*>(op))
            return CompileKind::Program;
        return CompileKind::Other;
    }

    const char* OpenMWIncrementalCompileOperation::compileKindName(CompileKind kind)
    {
        switch (kind)
        {
            case CompileKind::Drawable: return "drawable";
            case CompileKind::Texture: return "texture";
            case CompileKind::Program: return "program";
            case CompileKind::Other: return "other";
            case CompileKind::Count: break;
        }
        return "unknown";
    }

    std::size_t OpenMWIncrementalCompileOperation::compileClassIndex(const CompileSet* set)
    {
        switch (getV321CompileClass(set))
        {
            case V321CompileClass::ObjectPaging: return 1;
            case V321CompileClass::Terrain: return 2;
            case V321CompileClass::GenericModel: return 3;
            case V321CompileClass::Unknown:
            default: return 0;
        }
    }

    int OpenMWIncrementalCompileOperation::compileClassRank(const CompileSet* set)
    {
        switch (getV321CompileClass(set))
        {
            case V321CompileClass::ObjectPaging: return 0;
            case V321CompileClass::Terrain: return 1;
            case V321CompileClass::GenericModel: return 2;
            case V321CompileClass::Unknown:
            default: return 3;
        }
    }

    const char* OpenMWIncrementalCompileOperation::compileClassName(const CompileSet* set)
    {
        switch (getV321CompileClass(set))
        {
            case V321CompileClass::ObjectPaging: return "object_paging";
            case V321CompileClass::Terrain: return "terrain";
            case V321CompileClass::GenericModel: return "generic_model";
            case V321CompileClass::Unknown:
            default: return "unknown";
        }
    }

    std::size_t OpenMWIncrementalCompileOperation::costIndex(const CompileSet* set, CompileKind kind) const
    {
        return compileClassIndex(set) * sCompileKindCount + static_cast<std::size_t>(kind);
    }

    double OpenMWIncrementalCompileOperation::staticPriorMs(const CompileSet* set, CompileKind kind) const
    {
        if (mConfig.mHeavyLaneMode > 0 && kind == CompileKind::Drawable
            && getV321CompileClass(set) == V321CompileClass::Terrain)
            return mConfig.mTerrainDrawablePriorMs;
        return 0.0;
    }

    double OpenMWIncrementalCompileOperation::predictedMs(CompileOp* op, CompileInfo& info,
        const CompileSet* set, CompileKind kind, std::uint64_t& estimateCalls, std::uint64_t& estimateCacheHits)
    {
        // The OSG estimators are not free: texture estimation emits OSG_NOTICE,
        // and the old P4R selector called them again for every full-queue rescan.
        // Cache only the static OSG component for the current front operation.
        // Dynamic EMA/risk history below is still recomputed on every selection
        // pass, so recent measured cost immediately affects admission.
        double osgEstimateMs = 0.0;
        auto [cacheIt, inserted] = mPredictionCache.try_emplace(set);
        if (inserted || cacheIt->second.mOp != op)
        {
            if (op)
            {
                const double seconds = op->estimatedTimeForCompile(info);
                if (std::isfinite(seconds) && seconds > 0.0)
                    osgEstimateMs = seconds * 1000.0;
            }
            cacheIt->second = PredictionCacheEntry{ op, osgEstimateMs };
            ++estimateCalls;
        }
        else
        {
            osgEstimateMs = cacheIt->second.mOsgEstimateMs;
            ++estimateCacheHits;
        }

        const CostState& cost = mCosts.at(costIndex(set, kind));
        const double measured = cost.mSamples > 0 ? cost.mEmaMs * 1.25 : 0.0;
        // Risk decays on every successful operation in the same producer/type
        // bucket. Cap it relative to the EMA so one pathological call does not
        // quarantine thousands of cheap operations forever.
        const double risk = cost.mSamples > 0
            ? std::min(cost.mRiskMs, std::max(cost.mEmaMs * 4.0, 0.25))
            : 0.0;
        // Empirical terrain prior protects only the cold-start samples. Once the
        // class/type bucket has four observations, measured history owns the risk
        // estimate so ordinary cheap terrain cannot be quarantined forever.
        const double prior = cost.mSamples < 4 ? staticPriorMs(set, kind) : 0.0;
        return std::max({ 0.05, osgEstimateMs, measured, risk, prior });
    }

    void OpenMWIncrementalCompileOperation::observe(const CompileSet* set, CompileKind kind, double actualMs)
    {
        CostState& cost = mCosts.at(costIndex(set, kind));
        ++cost.mSamples;
        if (cost.mSamples == 1)
            cost.mEmaMs = actualMs;
        else
            cost.mEmaMs = cost.mEmaMs * 0.85 + actualMs * 0.15;
        cost.mRiskMs = std::max(actualMs, cost.mRiskMs * 0.90);
    }

    void OpenMWIncrementalCompileOperation::finishCompileSet(CompileSet* set)
    {
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_toCompileMutex);
            auto it = std::find_if(_toCompile.begin(), _toCompile.end(),
                [set](const osg::ref_ptr<CompileSet>& value) { return value.get() == set; });
            if (it != _toCompile.end())
                _toCompile.erase(it);
        }

        if (set->_compileCompletedCallback.valid() && set->_compileCompletedCallback->compileCompleted(set))
            return;

        OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_compiledMutex);
        _compiled.push_back(set);
    }

    void OpenMWIncrementalCompileOperation::pruneSeen(const CompileSets& queued)
    {
        std::unordered_set<const CompileSet*> live;
        live.reserve(queued.size());
        for (const osg::ref_ptr<CompileSet>& set : queued)
            live.insert(set.get());

        for (auto it = mSeen.begin(); it != mSeen.end();)
        {
            if (!live.contains(it->first))
                it = mSeen.erase(it);
            else
                ++it;
        }

        for (auto it = mPredictionCache.begin(); it != mPredictionCache.end();)
        {
            if (!live.contains(it->first))
                it = mPredictionCache.erase(it);
            else
                ++it;
        }
    }

    void OpenMWIncrementalCompileOperation::operator()(osg::GraphicsContext* context)
    {
        const int mode = mConfig.mMode;
        if (mode <= 0 || !context || !context->getState() || _contexts.size() != 1)
        {
            osgUtil::IncrementalCompileOperation::operator()(context);
            return;
        }

        // Preserve loading-screen / explicit compile-all behavior exactly.
        if (_compileAllTillFrameNumber > _currentFrameNumber)
        {
            osgUtil::IncrementalCompileOperation::operator()(context);
            return;
        }

        const unsigned int frame = frameNumberFor(context);
        const double targetFrameRate = std::max(1.0, mConfig.mTargetFrameRate);
        const double targetFrameMs = 1000.0 / targetFrameRate;
        const double currentElapsedMs = std::max(0.0, context->getTimeSinceLastClear() * 1000.0);
        const double lastHandoffMs = lastRenderingTraversalMs();

        P4CompilePolicyConfig policyConfig;
        policyConfig.mTargetFrameMs = targetFrameMs;
        policyConfig.mMaxBudgetMs = mConfig.mMaxBudgetMs;
        policyConfig.mCreditCapMs = mConfig.mCreditCapMs;
        policyConfig.mHeadroomRatio = mConfig.mHeadroomRatio;
        policyConfig.mHandoffThresholdMs = mConfig.mHandoffThresholdMs;

        const P4CompilePolicyOutput policy = updateP4CompilePolicy(
            mPolicyState, policyConfig, { mode, currentElapsedMs, lastHandoffMs });

        const bool smoothForHeavy = !policy.mSuppressedByHandoff
            && policy.mHeadroomMs >= mConfig.mHeavyMinHeadroomMs
            && lastHandoffMs < mConfig.mHandoffThresholdMs * 0.75;
        if (smoothForHeavy)
            mSmoothFrames = std::min(mSmoothFrames + 1, mConfig.mHeavyMinSmoothFrames + 1);
        else
            mSmoothFrames = 0;

        const auto schedulerStart = Debug::V3Diagnostics::Clock::now();

        CompileSets queued;
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_toCompileMutex);
            std::copy(_toCompile.begin(), _toCompile.end(), std::back_inserter(queued));
        }

        pruneSeen(queued);

        unsigned int oldestAge = 0;
        std::array<unsigned int, sCompileClassCount> queueByClass{};
        for (const osg::ref_ptr<CompileSet>& set : queued)
        {
            auto [it, inserted] = mSeen.try_emplace(set.get(), SeenState{ frame });
            if (!inserted && frame >= it->second.mFirstFrame)
                oldestAge = std::max(oldestAge, frame - it->second.mFirstFrame);
            ++queueByClass[compileClassIndex(set.get())];
        }

        const unsigned int maxQueueAge = std::max(1u, mConfig.mMaxQueueAgeFrames);
        const unsigned int hardQueueAge = maxQueueAge > std::numeric_limits<unsigned int>::max() / 4u
            ? std::numeric_limits<unsigned int>::max()
            : maxQueueAge * 4u;
        const unsigned int maxObjects = std::max(1u, mConfig.mMaxObjectsPerFrame);
        const double diagnosticThresholdMs = mConfig.mDiagnosticThresholdMs;

        double remainingBudgetMs = policy.mBudgetMs;
        double compileActualMs = 0.0;
        unsigned int compiledObjects = 0;
        unsigned int budgetedObjects = 0;
        unsigned int ageForcedObjects = 0;
        unsigned int heavyObjects = 0;
        unsigned int predictionMisses = 0;
        std::uint64_t candidateScans = 0;
        std::uint64_t estimateCalls = 0;
        std::uint64_t estimateCacheHits = 0;
        unsigned int selectionPasses = 0;
        double selectionActualMs = 0.0;
        bool stopAfterThisOperation = false;

        struct Candidate
        {
            CompileSet* mSet = nullptr;
            CompileOp* mOp = nullptr;
            CompileKind mKind = CompileKind::Other;
            double mPredictionMs = 0.0;
            unsigned int mAge = 0;
            int mRank = std::numeric_limits<int>::max();

            explicit operator bool() const { return mSet != nullptr && mOp != nullptr; }
        };

        auto betterFitting = [maxQueueAge](const Candidate& candidate, const Candidate& current) {
            if (!current)
                return true;
            const bool candidateUrgent = candidate.mAge >= maxQueueAge / 2u;
            const bool currentUrgent = current.mAge >= maxQueueAge / 2u;
            if (candidateUrgent != currentUrgent)
                return candidateUrgent;
            if (candidateUrgent && candidate.mAge != current.mAge)
                return candidate.mAge > current.mAge;
            if (candidate.mRank != current.mRank)
                return candidate.mRank < current.mRank;
            return candidate.mAge > current.mAge;
        };

        auto betterOverflow = [](const Candidate& candidate, const Candidate& current) {
            if (!current)
                return true;
            if (candidate.mAge != current.mAge)
                return candidate.mAge > current.mAge;
            return candidate.mRank < current.mRank;
        };

        while (compiledObjects < maxObjects && !queued.empty() && !stopAfterThisOperation)
        {
            Candidate fitting;
            Candidate agedOverflow;
            Candidate heavyReady;

            const auto selectionStart = Debug::V3Diagnostics::Clock::now();
            ++selectionPasses;
            for (const osg::ref_ptr<CompileSet>& setRef : queued)
            {
                ++candidateScans;
                CompileSet* set = setRef.get();
                if (!set)
                    continue;

                auto mapIt = set->_compileMap.find(context);
                if (mapIt == set->_compileMap.end() || mapIt->second._compileOps.empty())
                    continue;

                CompileOp* op = mapIt->second._compileOps.front().get();
                CompileInfo estimateInfo(context, this);
                const CompileKind kind = classify(op);
                const double prediction
                    = predictedMs(op, estimateInfo, set, kind, estimateCalls, estimateCacheHits);
                const auto seenIt = mSeen.find(set);
                const unsigned int age = seenIt != mSeen.end() && frame >= seenIt->second.mFirstFrame
                    ? frame - seenIt->second.mFirstFrame : 0;
                const int rank = compileClassRank(set);
                const bool fits = prediction <= std::max(0.05, remainingBudgetMs);
                const bool heavy = prediction >= mConfig.mHeavyThresholdMs;

                Candidate candidate{ set, op, kind, prediction, age, rank };

                // P4R repair: age never converts a cheap fitting operation into a
                // forced one. Cheap work keeps draining until budget/object cap.
                if (fits)
                {
                    if (betterFitting(candidate, fitting))
                        fitting = candidate;
                    continue;
                }

                if (age >= maxQueueAge && betterOverflow(candidate, agedOverflow))
                    agedOverflow = candidate;

                const bool heavyLaneReady = mConfig.mHeavyLaneMode > 0 && heavy
                    && mSmoothFrames >= mConfig.mHeavyMinSmoothFrames
                    && policy.mHeadroomMs >= mConfig.mHeavyMinHeadroomMs
                    && !policy.mSuppressedByHandoff;
                if (heavyLaneReady && betterOverflow(candidate, heavyReady))
                    heavyReady = candidate;
            }
            selectionActualMs += Debug::V3Diagnostics::elapsedMs(selectionStart);

            Candidate selected;
            std::string_view reason = "budgeted";
            bool forced = false;
            bool heavyPrewarm = false;

            if (fitting)
                selected = fitting;
            else if (heavyReady)
            {
                selected = heavyReady;
                reason = "heavy_smooth_headroom";
                forced = true;
                heavyPrewarm = true;
            }
            else if (agedOverflow)
            {
                const bool hardAgedOut = agedOverflow.mAge >= hardQueueAge;
                if (policy.mSuppressedByHandoff && !hardAgedOut)
                    break;
                selected = agedOverflow;
                reason = hardAgedOut ? "forced_by_hard_queue_age" : "forced_by_queue_age";
                forced = true;
            }
            else
                break;

            CompileInfo compileInfo(context, this);
            compileInfo.maxNumObjectsToCompile = 1;
            compileInfo.allocatedTime = 3600.0;
            compileInfo.compileAll = false;

            const auto start = Debug::V3Diagnostics::Clock::now();
            const bool completedSet = selected.mSet->compile(compileInfo);
            const double actualMs = Debug::V3Diagnostics::elapsedMs(start);
            observe(selected.mSet, selected.mKind, actualMs);
            consumeP4CompileCredit(mPolicyState, mode, actualMs);
            compileActualMs += actualMs;
            ++compiledObjects;
            if (heavyPrewarm)
                ++heavyObjects;
            else if (forced)
                ++ageForcedObjects;
            else
                ++budgetedObjects;
            if (actualMs > std::max(selected.mPredictionMs * 4.0, mConfig.mHeavyThresholdMs))
                ++predictionMisses;

            remainingBudgetMs = std::max(0.0, remainingBudgetMs - actualMs);

            if (actualMs >= diagnosticThresholdMs || forced)
            {
                writeP4CompileRow(frame, "op", compileKindName(selected.mKind),
                    compileClassName(selected.mSet), queued.size(), oldestAge,
                    policy.mBudgetMs, mPolicyState.mCreditMs, selected.mPredictionMs, actualMs,
                    policy.mHeadroomMs, lastHandoffMs, 1, reason);
            }

            if (completedSet)
            {
                CompileSet* completed = selected.mSet;
                finishCompileSet(completed);
                mSeen.erase(completed);
                mPredictionCache.erase(completed);
                queued.remove_if([completed](const osg::ref_ptr<CompileSet>& value) {
                    return value.get() == completed;
                });
            }

            if (heavyPrewarm)
                mSmoothFrames = 0;

            // Only a genuinely over-budget forced operation consumes the
            // one-operation escape hatch. Fitting aged work is normal work.
            if (forced || remainingBudgetMs <= 0.0)
                stopAfterThisOperation = true;
        }

        const double configuredDeleteBudgetMs = mConfig.mDeleteBudgetMs;
        double deleteBudgetMs = configuredDeleteBudgetMs;
        if (mode >= 2 && policy.mSuppressedByHandoff)
            deleteBudgetMs *= 0.25;

        double deleteActualMs = 0.0;
        if (deleteBudgetMs > 0.0)
        {
            double seconds = deleteBudgetMs / 1000.0;
            const auto start = Debug::V3Diagnostics::Clock::now();
            const osg::FrameStamp* fs = context->getState()->getFrameStamp();
            osg::flushDeletedGLObjects(
                context->getState()->getContextID(), fs ? fs->getReferenceTime() : 0.0, seconds);
            deleteActualMs = Debug::V3Diagnostics::elapsedMs(start);
            if (deleteActualMs >= diagnosticThresholdMs)
            {
                writeP4CompileRow(frame, "delete_flush", "delete", "n/a", queued.size(), oldestAge,
                    deleteBudgetMs, mPolicyState.mCreditMs, 0.0, deleteActualMs,
                    policy.mHeadroomMs, lastHandoffMs, 0, "separate_delete_budget");
            }
        }

        std::size_t queueDepthAfter = 0;
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_toCompileMutex);
            queueDepthAfter = _toCompile.size();
        }

        const double schedulerTotalMs = Debug::V3Diagnostics::elapsedMs(schedulerStart);

        if (queueDepthAfter > 0 || mLastQueueDepth > 0 || compiledObjects > 0 || deleteActualMs >= diagnosticThresholdMs)
        {
            std::ostringstream detail;
            detail << "mode=" << mode
                   << " heavy_mode=" << mConfig.mHeavyLaneMode
                   << " suppressed=" << (policy.mSuppressedByHandoff ? 1 : 0)
                   << " smooth_frames=" << mSmoothFrames
                   << " budgeted=" << budgetedObjects
                   << " forced=" << ageForcedObjects
                   << " heavy=" << heavyObjects
                   << " prediction_miss=" << predictionMisses
                   << " selection_ms=" << std::fixed << std::setprecision(3) << selectionActualMs
                   << " scheduler_total_ms=" << schedulerTotalMs
                   << " selection_passes=" << selectionPasses
                   << " candidates_scanned=" << candidateScans
                   << " estimate_calls=" << estimateCalls
                   << " estimate_cache_hits=" << estimateCacheHits
                   << " q_unknown=" << queueByClass[0]
                   << " q_object=" << queueByClass[1]
                   << " q_terrain=" << queueByClass[2]
                   << " q_model=" << queueByClass[3]
                   << " elapsed_before_ms=" << std::fixed << std::setprecision(3) << currentElapsedMs;
            writeP4CompileRow(frame, "summary", "", "", queueDepthAfter, oldestAge,
                policy.mBudgetMs, mPolicyState.mCreditMs, 0.0, compileActualMs + deleteActualMs,
                policy.mHeadroomMs, lastHandoffMs, compiledObjects, detail.str());
        }

        mLastQueueDepth = queueDepthAfter;
    }
}
