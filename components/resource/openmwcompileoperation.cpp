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
        if (dynamic_cast<const CompileDrawableOp*>(op))
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

    double OpenMWIncrementalCompileOperation::predictedMs(
        CompileOp* op, CompileInfo& info, CompileKind kind) const
    {
        double osgEstimateMs = 0.0;
        if (op)
        {
            const double seconds = op->estimatedTimeForCompile(info);
            if (std::isfinite(seconds) && seconds > 0.0)
                osgEstimateMs = seconds * 1000.0;
        }

        const CostState& cost = mCosts.at(static_cast<std::size_t>(kind));
        const double measured = cost.mSamples > 0 ? cost.mEmaMs : 0.0;
        const double floor = cost.mSamples > 0 ? std::min(cost.mMaxMs, measured * 2.0) : 0.10;
        return std::max({ 0.10, osgEstimateMs, measured, floor });
    }

    void OpenMWIncrementalCompileOperation::observe(CompileKind kind, double actualMs)
    {
        CostState& cost = mCosts.at(static_cast<std::size_t>(kind));
        ++cost.mSamples;
        if (cost.mSamples == 1)
            cost.mEmaMs = actualMs;
        else
            cost.mEmaMs = cost.mEmaMs * 0.85 + actualMs * 0.15;
        cost.mMaxMs = std::max(actualMs, cost.mMaxMs * 0.985);
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
        if (mode <= 0 || !context || !context->getState())
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
        double compileActualMs = 0.0;
        unsigned int compiledObjects = 0;
        bool forcedOldest = false;

        while (compiledObjects < maxObjects && !queued.empty())
        {
            CompileSet* selected = nullptr;
            CompileOp* selectedOp = nullptr;
            CompileKind selectedKind = CompileKind::Other;
            double selectedPrediction = std::numeric_limits<double>::max();
            unsigned int selectedAge = 0;
            int selectedRank = std::numeric_limits<int>::max();

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
                const double prediction = predictedMs(op, estimateInfo, kind);
                const auto seenIt = mSeen.find(set);
                const unsigned int age = seenIt != mSeen.end() && frame >= seenIt->second.mFirstFrame
                    ? frame - seenIt->second.mFirstFrame : 0;
                const int rank = compileClassRank(set);
                const bool agedOut = age >= maxQueueAge;

                const bool fits = prediction <= std::max(0.10, remainingBudgetMs);
                if (!fits && !agedOut)
                    continue;

                const bool selectedAgedOut = selected && selectedAge >= maxQueueAge;
                bool better = !selected;
                if (!better && agedOut != selectedAgedOut)
                    better = agedOut;
                else if (!better && agedOut == selectedAgedOut)
                    better = rank < selectedRank || (rank == selectedRank && age > selectedAge);

                if (better)
                {
                    selected = set;
                    selectedOp = op;
                    selectedKind = kind;
                    selectedPrediction = prediction;
                    selectedAge = age;
                    selectedRank = rank;
                }
            }

            if (!selected || !selectedOp)
                break;

            const bool agedOut = selectedAge >= maxQueueAge;
            const bool hardAgedOut = selectedAge >= maxQueueAge * 2u;
            if (agedOut && !hardAgedOut && policy.mSuppressedByHandoff
                && lastHandoffMs >= policyConfig.mHandoffThresholdMs * 1.5)
                break;

            forcedOldest = forcedOldest || agedOut;

            CompileInfo compileInfo(context, this);
            compileInfo.maxNumObjectsToCompile = 1;
            // We gate each operation ourselves because the stock ICO time check
            // cannot preempt a GL call once it starts.
            compileInfo.allocatedTime = 3600.0;
            compileInfo.compileAll = false;

            const auto start = Debug::V3Diagnostics::Clock::now();
            const bool completedSet = selected->compile(compileInfo);
            const double actualMs = Debug::V3Diagnostics::elapsedMs(start);
            observe(selectedKind, actualMs);
            consumeP4CompileCredit(mPolicyState, mode, actualMs);
            compileActualMs += actualMs;
            ++compiledObjects;
            remainingBudgetMs = std::max(0.0, remainingBudgetMs - actualMs);

            if (actualMs >= diagnosticThresholdMs || agedOut)
            {
                writeP4CompileRow(frame, "op", compileKindName(selectedKind),
                    compileClassName(selected), queued.size(), oldestAge,
                    policy.mBudgetMs, mPolicyState.mCreditMs, selectedPrediction, actualMs,
                    policy.mHeadroomMs, lastHandoffMs, 1,
                    hardAgedOut ? "forced_by_hard_queue_age"
                                : (agedOut ? "forced_by_queue_age" : "budgeted"));
            }

            if (completedSet)
            {
                finishCompileSet(selected);
                mSeen.erase(selected);
                queued.remove_if([&](const osg::ref_ptr<CompileSet>& value) {
                    return value.get() == selected;
                });
            }

            // An age override exists only to guarantee eventual progress. Never
            // let several oversized/starved GL calls collapse into one frame.
            if (agedOut || remainingBudgetMs <= 0.0)
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
                    deleteBudgetMs, mPolicyState.mCreditMs, 0.0, deleteActualMs,
                    policy.mHeadroomMs, lastHandoffMs, 0, "separate_delete_budget");
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
                policy.mBudgetMs, mPolicyState.mCreditMs, 0.0, compileActualMs + deleteActualMs,
                policy.mHeadroomMs, lastHandoffMs, compiledObjects, detail.str());
        }

        mLastQueueDepth = queueDepthAfter;
    }
}
