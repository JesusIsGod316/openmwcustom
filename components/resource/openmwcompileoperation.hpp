#ifndef OPENMW_COMPONENTS_RESOURCE_OPENMWCOMPILEOPERATION_H
#define OPENMW_COMPONENTS_RESOURCE_OPENMWCOMPILEOPERATION_H

#include "p4compilepolicy.hpp"

#include <array>
#include <atomic>
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
        unsigned int mMaxQueueAgeFrames = 12;
        double mDeleteBudgetMs = 0.15;
        unsigned int mMaxObjectsPerFrame = 4;
        double mDiagnosticThresholdMs = 0.20;
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

        struct CostState
        {
            double mEmaMs = 0.0;
            double mMaxMs = 0.0;
            std::uint64_t mSamples = 0;
        };

        struct SeenState
        {
            unsigned int mFirstFrame = 0;
        };

        CompileKind classify(const CompileOp* op) const;
        double predictedMs(CompileOp* op, CompileInfo& info, CompileKind kind) const;
        void observe(CompileKind kind, double actualMs);
        void finishCompileSet(CompileSet* set);
        void pruneSeen(const CompileSets& queued);
        static int compileClassRank(const CompileSet* set);
        static const char* compileClassName(const CompileSet* set);
        static const char* compileKindName(CompileKind kind);

        OpenMWCompileSchedulerConfig mConfig;
        P4CompilePolicyState mPolicyState;
        std::array<CostState, static_cast<std::size_t>(CompileKind::Count)> mCosts{};
        std::unordered_map<const CompileSet*, SeenState> mSeen;
        std::size_t mLastQueueDepth = 0;

        inline static std::atomic<double> sLastRenderingTraversalMs{ 0.0 };
    };
}

#endif
