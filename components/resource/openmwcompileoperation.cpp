#include "openmwcompileoperation.hpp"

#include "v321classifiedcompileset.hpp"

#include <components/debug/v3diagnostics.hpp>

#include <osg/Array>
#include <osg/Geometry>
#include <osg/GraphicsContext>
#include <osg/Image>
#include <osg/PrimitiveSet>
#include <osg/Texture>
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

    std::size_t OpenMWIncrementalCompileOperation::costIndex(
        const CompileSet* set, CompileKind kind, std::size_t sizeTier) const
    {
        return (compileClassIndex(set) * sCompileKindCount + static_cast<std::size_t>(kind))
            * sCompileSizeTierCount + std::min(sizeTier, sCompileSizeTierCount - 1);
    }

    std::size_t OpenMWIncrementalCompileOperation::resourceSizeBytes(const CompileOp* op)
    {
        if (const auto* customDrawable = dynamic_cast<const OpenMWDrawableCompileOp*>(op))
        {
            const std::size_t bytes = customDrawable->resourceBytes();
            if (bytes > 0)
                return bytes;
        }

        if (const auto* textureOp = dynamic_cast<const CompileTextureOp*>(op))
        {
            std::size_t bytes = 0;
            if (textureOp->_texture)
            {
                for (unsigned int i = 0; i < textureOp->_texture->getNumImages(); ++i)
                {
                    if (const osg::Image* image = textureOp->_texture->getImage(i))
                        bytes += image->getTotalDataSize();
                }
            }
            return bytes;
        }

        const osg::Drawable* drawable = nullptr;
        if (const auto* drawableOp = dynamic_cast<const CompileDrawableOp*>(op))
            drawable = drawableOp->_drawable.get();
        if (const osg::Geometry* geometry = drawable ? drawable->asGeometry() : nullptr)
        {
            std::size_t bytes = 0;
            auto addArray = [&bytes](const osg::Array* array) {
                if (array)
                    bytes += array->getTotalDataSize();
            };
            addArray(geometry->getVertexArray());
            addArray(geometry->getNormalArray());
            addArray(geometry->getColorArray());
            addArray(geometry->getSecondaryColorArray());
            addArray(geometry->getFogCoordArray());
            for (const osg::ref_ptr<osg::Array>& array : geometry->getTexCoordArrayList())
                addArray(array.get());
            for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); ++i)
            {
                if (const osg::PrimitiveSet* primitive = geometry->getPrimitiveSet(i))
                    bytes += primitive->getTotalDataSize();
            }
            return bytes;
        }
        return 0;
    }

    std::size_t OpenMWIncrementalCompileOperation::resourceSizeTier(std::size_t bytes)
    {
        if (bytes >= 8u * 1024u * 1024u)
            return 3;
        if (bytes >= 2u * 1024u * 1024u)
            return 2;
        if (bytes >= 512u * 1024u)
            return 1;
        return 0;
    }

    int OpenMWIncrementalCompileOperation::urgencyRank(const CompileSet* set)
    {
        switch (getV321CompileUrgency(set))
        {
            case V321CompileUrgency::Required: return 0;
            case V321CompileUrgency::NearFuture: return 1;
            case V321CompileUrgency::Background: return 2;
        }
        return 1;
    }

    const char* OpenMWIncrementalCompileOperation::urgencyName(const CompileSet* set)
    {
        switch (getV321CompileUrgency(set))
        {
            case V321CompileUrgency::Required: return "required";
            case V321CompileUrgency::NearFuture: return "near_future";
            case V321CompileUrgency::Background: return "background";
        }
        return "near_future";
    }

    double OpenMWIncrementalCompileOperation::staticPriorMs(
        const CompileSet* set, CompileKind kind, std::size_t sizeTier) const
    {
        double prior = 0.0;

        if ((mConfig.mHeavyLaneMode > 0 || mConfig.mResidencySchedulerMode > 0)
            && kind == CompileKind::Drawable && getV321CompileClass(set) == V321CompileClass::Terrain)
            prior = std::max(prior, mConfig.mTerrainDrawablePriorMs);

        if (kind == CompileKind::Texture)
        {
            if (sizeTier >= 3)
                prior = std::max(prior, 12.0);
            else if (sizeTier == 2)
                prior = std::max(prior, 6.0);
            else if (sizeTier == 1)
                prior = std::max(prior, 2.0);
        }
        return prior;
    }

    double OpenMWIncrementalCompileOperation::cachedOsgEstimateMs(CompileOp* op, CompileInfo& info,
        const CompileSet* set, std::uint64_t& estimateCalls, std::uint64_t& estimateCacheHits)
    {
        // OSG's static estimators can be surprisingly expensive (and the texture
        // estimator emits OSG_NOTICE). Cache this component by the current front
        // operation. Per-bucket measured history remains dynamic below.
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
        return osgEstimateMs;
    }

    double OpenMWIncrementalCompileOperation::predictedMs(
        double osgEstimateMs, std::size_t costBucket, double staticPrior) const
    {
        const CostState& cost = mCosts.at(costBucket);
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
        const double prior = cost.mSamples < 4 ? staticPrior : 0.0;
        return std::max({ 0.05, osgEstimateMs, measured, risk, prior });
    }

    void OpenMWIncrementalCompileOperation::observe(std::size_t costBucket, double actualMs)
    {
        CostState& cost = mCosts.at(costBucket);
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
        unsigned int quarantinedCandidates = 0;
        unsigned int predictionMisses = 0;
        std::uint64_t candidateScans = 0;
        std::uint64_t candidateBuilds = 0;
        std::uint64_t estimateCalls = 0;
        std::uint64_t estimateCacheHits = 0;
        unsigned int selectionPasses = 0;
        double candidateBuildActualMs = 0.0;
        double selectionActualMs = 0.0;
        bool stopAfterThisOperation = false;

        struct Candidate
        {
            CompileSet* mSet = nullptr;
            CompileOp* mOp = nullptr;
            CompileKind mKind = CompileKind::Other;
            double mOsgEstimateMs = 0.0;
            double mStaticPriorMs = 0.0;
            double mPredictionMs = 0.0;
            std::size_t mCostBucket = 0;
            std::size_t mResourceBytes = 0;
            std::size_t mSizeTier = 0;
            unsigned int mAge = 0;
            int mUrgencyRank = 1;
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
            if (candidate.mUrgencyRank != current.mUrgencyRank)
                return candidate.mUrgencyRank < current.mUrgencyRank;
            if (candidate.mRank != current.mRank)
                return candidate.mRank < current.mRank;
            return candidate.mAge > current.mAge;
        };

        auto betterOverflow = [](const Candidate& candidate, const Candidate& current) {
            if (!current)
                return true;
            if (candidate.mUrgencyRank != current.mUrgencyRank)
                return candidate.mUrgencyRank < current.mUrgencyRank;
            if (candidate.mAge != current.mAge)
                return candidate.mAge > current.mAge;
            return candidate.mRank < current.mRank;
        };

        // Build the expensive descriptor for each queued CompileSet once per
        // frame. Subsequent admission passes operate on this flat vector and
        // refresh only the selected set if its front operation advances.
        std::vector<Candidate> candidates;
        candidates.reserve(queued.size());
        std::size_t activeCandidates = 0;
        std::size_t localQueueDepth = queued.size();

        auto refreshCandidate = [&](Candidate& candidate) {
            if (!candidate.mSet)
                return false;
            auto mapIt = candidate.mSet->_compileMap.find(context);
            if (mapIt == candidate.mSet->_compileMap.end() || mapIt->second._compileOps.empty())
            {
                candidate.mOp = nullptr;
                return false;
            }

            CompileOp* op = mapIt->second._compileOps.front().get();
            CompileInfo estimateInfo(context, this);
            const CompileKind kind = classify(op);
            candidate.mOp = op;
            candidate.mKind = kind;
            candidate.mResourceBytes = resourceSizeBytes(op);
            candidate.mSizeTier = resourceSizeTier(candidate.mResourceBytes);
            candidate.mCostBucket = costIndex(candidate.mSet, kind, candidate.mSizeTier);
            candidate.mStaticPriorMs = staticPriorMs(candidate.mSet, kind, candidate.mSizeTier);
            candidate.mUrgencyRank = urgencyRank(candidate.mSet);
            candidate.mOsgEstimateMs
                = cachedOsgEstimateMs(op, estimateInfo, candidate.mSet, estimateCalls, estimateCacheHits);
            candidate.mPredictionMs
                = predictedMs(candidate.mOsgEstimateMs, candidate.mCostBucket, candidate.mStaticPriorMs);
            candidate.mRank = compileClassRank(candidate.mSet);
            ++candidateBuilds;
            return true;
        };

        const auto candidateBuildStart = Debug::V3Diagnostics::Clock::now();
        for (const osg::ref_ptr<CompileSet>& setRef : queued)
        {
            CompileSet* set = setRef.get();
            if (!set)
                continue;
            const auto seenIt = mSeen.find(set);
            const unsigned int age = seenIt != mSeen.end() && frame >= seenIt->second.mFirstFrame
                ? frame - seenIt->second.mFirstFrame : 0;
            Candidate candidate;
            candidate.mSet = set;
            candidate.mAge = age;
            if (refreshCandidate(candidate))
            {
                candidates.push_back(candidate);
                ++activeCandidates;
            }
        }
        candidateBuildActualMs = Debug::V3Diagnostics::elapsedMs(candidateBuildStart);

        while (compiledObjects < maxObjects && activeCandidates > 0 && !stopAfterThisOperation)
        {
            Candidate fitting;
            Candidate agedOverflow;
            Candidate heavyReady;
            std::size_t fittingIndex = std::numeric_limits<std::size_t>::max();
            std::size_t agedOverflowIndex = std::numeric_limits<std::size_t>::max();
            std::size_t heavyReadyIndex = std::numeric_limits<std::size_t>::max();

            const auto selectionStart = Debug::V3Diagnostics::Clock::now();
            ++selectionPasses;
            for (std::size_t i = 0; i < candidates.size(); ++i)
            {
                Candidate& candidate = candidates[i];
                if (!candidate)
                    continue;
                ++candidateScans;

                // Dynamic measured risk is intentionally refreshed every pass,
                // but the OSG estimator, map lookup, RTTI classification and age
                // lookup are not repeated for every candidate.
                candidate.mPredictionMs
                    = predictedMs(candidate.mOsgEstimateMs, candidate.mCostBucket, candidate.mStaticPriorMs);
                const bool fits = candidate.mPredictionMs <= std::max(0.05, remainingBudgetMs);
                const bool heavy = candidate.mPredictionMs >= mConfig.mHeavyThresholdMs;
                const V321CompileUrgency urgency = getV321CompileUrgency(candidate.mSet);
                const bool quarantineHeavy = mConfig.mResidencySchedulerMode > 0 && heavy
                    && (urgency == V321CompileUrgency::Background
                        || (mConfig.mResidencySchedulerMode >= 2
                            && urgency == V321CompileUrgency::NearFuture));

                if (quarantineHeavy)
                {
                    ++quarantinedCandidates;
                    continue;
                }

                // P4R repair: age never converts a cheap fitting operation into a
                // forced one. Cheap work keeps draining until budget/object cap.
                if (fits)
                {
                    if (betterFitting(candidate, fitting))
                    {
                        fitting = candidate;
                        fittingIndex = i;
                    }
                    continue;
                }

                if (candidate.mAge >= maxQueueAge && betterOverflow(candidate, agedOverflow))
                {
                    agedOverflow = candidate;
                    agedOverflowIndex = i;
                }

                const bool heavyLaneReady = mConfig.mHeavyLaneMode > 0 && heavy
                    && mSmoothFrames >= mConfig.mHeavyMinSmoothFrames
                    && policy.mHeadroomMs >= mConfig.mHeavyMinHeadroomMs
                    && !policy.mSuppressedByHandoff;
                if (heavyLaneReady && betterOverflow(candidate, heavyReady))
                {
                    heavyReady = candidate;
                    heavyReadyIndex = i;
                }
            }
            selectionActualMs += Debug::V3Diagnostics::elapsedMs(selectionStart);

            Candidate selected;
            std::size_t selectedIndex = std::numeric_limits<std::size_t>::max();
            std::string_view reason = "budgeted";
            bool forced = false;
            bool heavyPrewarm = false;

            if (fitting)
            {
                selected = fitting;
                selectedIndex = fittingIndex;
            }
            else if (heavyReady)
            {
                selected = heavyReady;
                selectedIndex = heavyReadyIndex;
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
                selectedIndex = agedOverflowIndex;
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
            observe(selected.mCostBucket, actualMs);
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
                    compileClassName(selected.mSet), localQueueDepth, oldestAge,
                    policy.mBudgetMs, mPolicyState.mCreditMs, selected.mPredictionMs, actualMs,
                    policy.mHeadroomMs, lastHandoffMs, 1,
                    std::string(reason) + " urgency=" + urgencyName(selected.mSet)
                        + " bytes=" + std::to_string(selected.mResourceBytes)
                        + " tier=" + std::to_string(selected.mSizeTier));
            }

            if (completedSet)
            {
                CompileSet* completed = selected.mSet;
                finishCompileSet(completed);
                mSeen.erase(completed);
                mPredictionCache.erase(completed);
                if (selectedIndex < candidates.size() && candidates[selectedIndex])
                {
                    candidates[selectedIndex] = Candidate{};
                    --activeCandidates;
                    if (localQueueDepth > 0)
                        --localQueueDepth;
                }
            }
            else if (selectedIndex < candidates.size())
            {
                // Only the selected CompileSet can advance its front operation.
                // Refresh that one descriptor instead of rebuilding the entire
                // queue before the next cheap-drain admission pass.
                if (!refreshCandidate(candidates[selectedIndex]))
                {
                    candidates[selectedIndex] = Candidate{};
                    --activeCandidates;
                }
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
                writeP4CompileRow(frame, "delete_flush", "delete", "n/a", localQueueDepth, oldestAge,
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
                   << " residency_mode=" << mConfig.mResidencySchedulerMode
                   << " suppressed=" << (policy.mSuppressedByHandoff ? 1 : 0)
                   << " smooth_frames=" << mSmoothFrames
                   << " budgeted=" << budgetedObjects
                   << " forced=" << ageForcedObjects
                   << " heavy=" << heavyObjects
                   << " quarantined_candidates=" << quarantinedCandidates
                   << " prediction_miss=" << predictionMisses
                   << " candidate_build_ms=" << std::fixed << std::setprecision(3) << candidateBuildActualMs
                   << " selection_ms=" << selectionActualMs
                   << " scheduler_total_ms=" << schedulerTotalMs
                   << " selection_passes=" << selectionPasses
                   << " candidate_builds=" << candidateBuilds
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
