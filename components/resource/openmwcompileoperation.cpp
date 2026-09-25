#include "openmwcompileoperation.hpp"

#include "v321classifiedcompileset.hpp"
#include "p4compileops.hpp"

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
#include <vector>

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
        double budgetMs, double creditMs, double predictedMs, double osgEstimateMs,
        double costEmaMs, double estimateScale, double actualMs,
        double headroomMs, double lastHandoffMs, unsigned int objects,
        bool fitsBudget, bool forced, std::string_view detail)
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
            << budgetMs << ',' << creditMs << ',' << predictedMs << ',' << osgEstimateMs << ','
            << costEmaMs << ',' << estimateScale << ',' << actualMs << ','
            << headroomMs << ',' << lastHandoffMs << ',' << objects << ','
            << (fitsBudget ? 1 : 0) << ',' << (forced ? 1 : 0) << ','
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
        if (dynamic_cast<const CompileDrawableOp*>(op))
            return CompileKind::Drawable;
        if (dynamic_cast<const CompileTextureOp*>(op))
            return CompileKind::Texture;
        if (dynamic_cast<const CompileProgramOp*>(op))
            return CompileKind::Program;
        if (dynamic_cast<const P4CompileBufferOp*>(op))
            return CompileKind::Buffer;
        return CompileKind::Other;
    }

    const char* OpenMWIncrementalCompileOperation::compileKindName(CompileKind kind)
    {
        switch (kind)
        {
            case CompileKind::Drawable: return "drawable";
            case CompileKind::Texture: return "texture";
            case CompileKind::Program: return "program";
            case CompileKind::Buffer: return "buffer";
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

    OpenMWIncrementalCompileOperation::Prediction OpenMWIncrementalCompileOperation::predictedCost(
        CompileOp* op, CompileInfo& info, CompileKind kind, const CompileSet* set) const
    {
        Prediction result;
        if (op)
        {
            const double seconds = op->estimatedTimeForCompile(info);
            if (std::isfinite(seconds) && seconds > 0.0)
                result.mOsgEstimateMs = seconds * 1000.0;
        }

        const CostState& cost
            = mCosts.at(compileClassIndex(set)).at(static_cast<std::size_t>(kind));
        result.mEmaMs = cost.mSamples > 0 ? cost.mEmaMs : 0.0;
        result.mEstimateScale = cost.mSamples > 0 ? std::clamp(cost.mEstimateScale, 1.0, 256.0) : 1.0;

        const double scaledEstimate = result.mOsgEstimateMs * result.mEstimateScale;
        const double learnedFloor = cost.mSamples > 0 ? result.mEmaMs * 0.65 : 0.10;
        const double highWaterGuard = cost.mSamples > 1
            ? std::min(cost.mHighWaterMs * 0.75, std::max(learnedFloor * 2.0, scaledEstimate * 1.5))
            : 0.0;

        result.mPredictedMs = std::max({ 0.10, scaledEstimate, learnedFloor, highWaterGuard });
        return result;
    }

    void OpenMWIncrementalCompileOperation::observe(
        CompileKind kind, const CompileSet* set, double osgEstimateMs, double actualMs)
    {
        CostState& cost
            = mCosts.at(compileClassIndex(set)).at(static_cast<std::size_t>(kind));
        ++cost.mSamples;
        if (cost.mSamples == 1)
            cost.mEmaMs = actualMs;
        else
            cost.mEmaMs = cost.mEmaMs * 0.82 + actualMs * 0.18;

        cost.mHighWaterMs = std::max(actualMs, cost.mHighWaterMs * 0.985);

        if (osgEstimateMs > 0.01)
        {
            const double ratio = std::clamp(actualMs / osgEstimateMs, 1.0, 256.0);
            if (cost.mSamples == 1)
                cost.mEstimateScale = ratio;
            else
                cost.mEstimateScale = cost.mEstimateScale * 0.85 + ratio * 0.15;
        }
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
    }

    void OpenMWIncrementalCompileOperation::operator()(osg::GraphicsContext* context)
    {
        const int mode = mConfig.mMode;
        // P4 owns one mutable cost/credit history. OpenMW's normal Windows
        // renderer has one graphics context; fail open to stock ICO for
        // multi-context/stereo configurations rather than sharing that state
        // across concurrent graphics threads.
        if (mode <= 0 || !context || !context->getState() || _contexts.size() != 1)
        {
            osgUtil::IncrementalCompileOperation::operator()(context);
            return;
        }

        // Preserve OSG's explicit "compile all" semantics used by loading paths.
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

        CompileSets queued;
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_toCompileMutex);
            std::copy(_toCompile.begin(), _toCompile.end(), std::back_inserter(queued));
        }

        pruneSeen(queued);

        unsigned int oldestAge = 0;
        for (const osg::ref_ptr<CompileSet>& set : queued)
        {
            auto [it, inserted] = mSeen.try_emplace(set.get(), SeenState{ frame });
            if (!inserted && frame >= it->second.mFirstFrame)
                oldestAge = std::max(oldestAge, frame - it->second.mFirstFrame);
        }

        const unsigned int maxQueueAge = mConfig.mMaxQueueAgeFrames;
        const unsigned int maxObjects = mConfig.mMaxObjectsPerFrame;
        const double diagnosticThresholdMs = mConfig.mDiagnosticThresholdMs;

        double remainingBudgetMs = policy.mBudgetMs;
        if (!policy.mSuppressedByHandoff)
        {
            // The first P4 build starved the queue because the render thread
            // frequently reached ICO with near-zero nominal headroom. Permit a
            // very small cheap-work floor, while keeping expensive work gated.
            double minimumDrain = mConfig.mMinimumDrainBudgetMs;
            if (queued.size() >= 1024)
                minimumDrain = std::max(minimumDrain, 0.75);
            else if (queued.size() >= 256)
                minimumDrain = std::max(minimumDrain, 0.50);
            remainingBudgetMs = std::max(remainingBudgetMs, minimumDrain);
        }

        double compileActualMs = 0.0;
        unsigned int compiledObjects = 0;
        bool forcedOldest = false;

        while (compiledObjects < maxObjects && !queued.empty())
        {
            struct Candidate
            {
                CompileSet* mSet = nullptr;
                CompileOp* mOp = nullptr;
                CompileKind mKind = CompileKind::Other;
                Prediction mPrediction;
                unsigned int mAge = 0;
                int mRank = std::numeric_limits<int>::max();
            };

            Candidate bestFit;
            Candidate bestForced;

            for (const osg::ref_ptr<CompileSet>& setRef : queued)
            {
                CompileSet* set = setRef.get();
                if (!set)
                    continue;

                auto mapIt = set->_compileMap.find(context);
                if (mapIt == set->_compileMap.end() || mapIt->second._compileOps.empty())
                    continue;

                CompileOp* op = mapIt->second._compileOps.front().get();
                CompileInfo estimateInfo(context, this);
                const CompileKind kind = classify(op);
                const Prediction prediction = predictedCost(op, estimateInfo, kind, set);
                const auto seenIt = mSeen.find(set);
                const unsigned int age = seenIt != mSeen.end() && frame >= seenIt->second.mFirstFrame
                    ? frame - seenIt->second.mFirstFrame : 0;
                const int rank = compileClassRank(set);
                const bool fits = prediction.mPredictedMs <= std::max(0.10, remainingBudgetMs);
                const bool agedOut = age >= maxQueueAge;

                auto betterFit = [&](const Candidate& current) {
                    if (!current.mSet)
                        return true;
                    if (rank != current.mRank)
                        return rank < current.mRank;
                    if (age != current.mAge)
                        return age > current.mAge;
                    return prediction.mPredictedMs < current.mPrediction.mPredictedMs;
                };

                auto betterForced = [&](const Candidate& current) {
                    if (!current.mSet)
                        return true;
                    if (age != current.mAge)
                        return age > current.mAge;
                    if (rank != current.mRank)
                        return rank < current.mRank;
                    return prediction.mPredictedMs < current.mPrediction.mPredictedMs;
                };

                if (fits && betterFit(bestFit))
                    bestFit = Candidate{ set, op, kind, prediction, age, rank };
                else if (!fits && agedOut && betterForced(bestForced))
                    bestForced = Candidate{ set, op, kind, prediction, age, rank };
            }

            Candidate selected = bestFit.mSet ? bestFit : bestForced;
            if (!selected.mSet || !selected.mOp)
                break;

            const bool fitsBudget = bestFit.mSet != nullptr;
            const bool forced = !fitsBudget;
            const bool agedOut = selected.mAge >= maxQueueAge;
            const bool hardAgedOut = selected.mAge >= maxQueueAge * 2u;

            if (forced && !hardAgedOut && policy.mSuppressedByHandoff
                && lastHandoffMs >= policyConfig.mHandoffThresholdMs * 1.5)
                break;

            forcedOldest = forcedOldest || forced;

            CompileInfo compileInfo(context, this);
            compileInfo.maxNumObjectsToCompile = 1;
            // We gate each operation ourselves because a GL call cannot be
            // preempted after entry.
            compileInfo.allocatedTime = 3600.0;
            compileInfo.compileAll = false;

            const auto start = Debug::V3Diagnostics::Clock::now();
            const bool completedSet = selected.mSet->compile(compileInfo);
            const double actualMs = Debug::V3Diagnostics::elapsedMs(start);
            observe(selected.mKind, selected.mSet, selected.mPrediction.mOsgEstimateMs, actualMs);
            consumeP4CompileCredit(mPolicyState, mode, actualMs);
            compileActualMs += actualMs;
            ++compiledObjects;
            remainingBudgetMs = std::max(0.0, remainingBudgetMs - actualMs);

            if (actualMs >= diagnosticThresholdMs || agedOut || forced)
            {
                std::string detail;
                if (forced)
                    detail = hardAgedOut ? "forced_by_hard_queue_age_over_budget"
                                       : "forced_by_queue_age_over_budget";
                else if (selected.mPrediction.mPredictedMs >= mConfig.mHeavyOpThresholdMs)
                    detail = "budgeted_heavy";
                else if (agedOut)
                    detail = "budgeted_aged_cheap";
                else
                    detail = "budgeted";

                writeP4CompileRow(frame, "op", compileKindName(selected.mKind),
                    compileClassName(selected.mSet), queued.size(), oldestAge,
                    policy.mBudgetMs, mPolicyState.mCreditMs,
                    selected.mPrediction.mPredictedMs, selected.mPrediction.mOsgEstimateMs,
                    selected.mPrediction.mEmaMs, selected.mPrediction.mEstimateScale, actualMs,
                    policy.mHeadroomMs, lastHandoffMs, 1, fitsBudget, forced, detail);
            }

            if (completedSet)
            {
                finishCompileSet(selected.mSet);
                mSeen.erase(selected.mSet);
                queued.remove_if([&](const osg::ref_ptr<CompileSet>& value) {
                    return value.get() == selected.mSet;
                });
            }

            // Only an actually over-budget age override is limited to one per
            // frame. Aged work that fits is ordinary cheap work and may keep
            // draining until the time/object budget is exhausted.
            if (forced || remainingBudgetMs <= 0.0)
                break;
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
                    deleteBudgetMs, mPolicyState.mCreditMs, 0.0, 0.0, 0.0, 1.0, deleteActualMs,
                    policy.mHeadroomMs, lastHandoffMs, 0, true, false, "separate_delete_budget");
            }
        }

        std::size_t queueDepthAfter = 0;
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lock(_toCompileMutex);
            queueDepthAfter = _toCompile.size();
        }

        if (queueDepthAfter > 0 || mLastQueueDepth > 0 || compiledObjects > 0 || deleteActualMs >= diagnosticThresholdMs)
        {
            std::ostringstream detail;
            detail << "mode=" << mode
                   << " suppressed=" << (policy.mSuppressedByHandoff ? 1 : 0)
                   << " forced_oldest=" << (forcedOldest ? 1 : 0)
                   << " elapsed_before_ms=" << std::fixed << std::setprecision(3) << currentElapsedMs;
            writeP4CompileRow(frame, "summary", "", "", queueDepthAfter, oldestAge,
                policy.mBudgetMs, mPolicyState.mCreditMs, 0.0, 0.0, 0.0, 1.0,
                compileActualMs + deleteActualMs, policy.mHeadroomMs, lastHandoffMs,
                compiledObjects, true, false, detail.str());
        }

        mLastQueueDepth = queueDepthAfter;
    }
}
