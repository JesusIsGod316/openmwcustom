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
    class OpenMWIncrementalCompileOperation final : public osgUtil::IncrementalCompileOperation
    {
    public:
        OpenMWIncrementalCompileOperation();

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

        P4CompilePolicyState mPolicyState;
        std::array<CostState, static_cast<std::size_t>(CompileKind::Count)> mCosts{};
        std::unordered_map<const CompileSet*, SeenState> mSeen;
        std::size_t mLastQueueDepth = 0;

        inline static std::atomic<double> sLastRenderingTraversalMs{ 0.0 };
    };
}

#endif
