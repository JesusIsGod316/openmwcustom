#ifndef OPENMW_COMPONENTS_RESOURCE_OPENMWCOMPILEOPERATION_H
#define OPENMW_COMPONENTS_RESOURCE_OPENMWCOMPILEOPERATION_H

#include "p4compilepolicy.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <osgUtil/IncrementalCompileOperation>

namespace Resource
{
    struct OpenMWCompileSchedulerConfig
    {
        int mMode = 0;
        double mTargetFrameRate = 60.0;
        double mMaxBudgetMs = 2.0;
        double mCreditCapMs = 2.5;
        double mHeadroomRatio = 0.25;
        double mHandoffThresholdMs = 20.0;
        unsigned int mMaxQueueAgeFrames = 120;
        double mDeleteBudgetMs = 0.15;
        unsigned int mMaxObjectsPerFrame = 12;
        double mDiagnosticThresholdMs = 0.20;

        // P4R / P5: high-risk single GL calls are isolated from the normal
        // cheap-drain lane. This does not preempt a driver call; it only chooses
        // when an already-known/predicted heavy operation may begin.
        int mHeavyLaneMode = 0;
        double mHeavyThresholdMs = 6.0;
        unsigned int mHeavyMinSmoothFrames = 30;
        double mHeavyMinHeadroomMs = 5.0;
        double mTerrainDrawablePriorMs = 8.0;
    };

    class OpenMWIncrementalCompileOperation final : public osgUtil::IncrementalCompileOperation
    {
    public:
        explicit OpenMWIncrementalCompileOperation(OpenMWCompileSchedulerConfig config);

        void operator()(osg::GraphicsContext* context) override;

        static void publishRenderingTraversalMs(double value) noexcept;
        static double lastRenderingTraversalMs() noexcept;

    protected:
        ~OpenMWIncrementalCompileOperation() override = default;

    private:
        enum class CompileKind : unsigned char
        {
            Drawable = 0,
            Texture,
            Program,
            Other,
            Count,
        };

        static constexpr std::size_t sCompileClassCount = 4;
        static constexpr std::size_t sCompileKindCount = static_cast<std::size_t>(CompileKind::Count);

        struct CostState
        {
            double mEmaMs = 0.0;
            double mRiskMs = 0.0;
            std::uint64_t mSamples = 0;
        };

        struct SeenState
        {
            unsigned int mFirstFrame = 0;
        };

        CompileKind classify(const CompileOp* op) const;
        double predictedMs(CompileOp* op, CompileInfo& info, const CompileSet* set, CompileKind kind) const;
        void observe(const CompileSet* set, CompileKind kind, double actualMs);
        std::size_t costIndex(const CompileSet* set, CompileKind kind) const;
        double staticPriorMs(const CompileSet* set, CompileKind kind) const;

        void finishCompileSet(CompileSet* set);
        void pruneSeen(const CompileSets& queued);
        static int compileClassRank(const CompileSet* set);
        static std::size_t compileClassIndex(const CompileSet* set);
        static const char* compileClassName(const CompileSet* set);
        static const char* compileKindName(CompileKind kind);

        OpenMWCompileSchedulerConfig mConfig;
        P4CompilePolicyState mPolicyState;
        std::array<CostState, sCompileClassCount * sCompileKindCount> mCosts{};
        std::unordered_map<const CompileSet*, SeenState> mSeen;
        std::size_t mLastQueueDepth = 0;
        unsigned int mSmoothFrames = 0;

        inline static std::atomic<double> sLastRenderingTraversalMs{ 0.0 };
    };
}

#endif
