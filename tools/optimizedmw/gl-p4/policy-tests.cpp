#include <components/resource/p4compilepolicy.hpp>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    bool near(double a, double b, double eps = 1e-6)
    {
        return std::abs(a - b) <= eps;
    }
}

int main()
{
    Resource::P4CompilePolicyConfig config;
    config.mTargetFrameMs = 16.0;
    config.mMaxBudgetMs = 2.0;
    config.mCreditCapMs = 2.5;
    config.mHeadroomRatio = 0.25;
    config.mHandoffThresholdMs = 20.0;

    {
        Resource::P4CompilePolicyState state;
        const auto out = Resource::updateP4CompilePolicy(state, config, { 0, 8.0, 0.0 });
        require(near(out.mBudgetMs, 0.0), "mode0 must not create custom compile budget");
        require(near(state.mCreditMs, 0.0), "mode0 must clear custom credit");
    }

    {
        Resource::P4CompilePolicyState state;
        const auto out = Resource::updateP4CompilePolicy(state, config, { 1, 8.0, 0.0 });
        require(near(out.mHeadroomMs, 8.0), "mode1 headroom mismatch");
        require(near(out.mBudgetMs, 2.0), "mode1 budget should clamp at max budget");
        require(near(state.mCreditMs, 0.0), "mode1 must not retain compile credit");
    }

    {
        Resource::P4CompilePolicyState state;
        auto first = Resource::updateP4CompilePolicy(state, config, { 2, 12.0, 8.0 });
        require(near(first.mBudgetMs, 1.0), "mode2 first smooth frame should earn one millisecond");
        require(near(state.mCreditMs, 1.0), "mode2 credit accumulation mismatch");

        auto second = Resource::updateP4CompilePolicy(state, config, { 2, 12.0, 8.0 });
        require(near(second.mBudgetMs, 2.0), "mode2 second smooth frame should reach max per-frame budget");
        require(near(state.mCreditMs, 2.0), "mode2 second credit mismatch");

        Resource::consumeP4CompileCredit(state, 2, 1.25);
        require(near(state.mCreditMs, 0.75), "mode2 actual compile cost was not charged to credit");

        auto stalled = Resource::updateP4CompilePolicy(state, config, { 2, 10.0, 25.0 });
        require(stalled.mSuppressedByHandoff, "handoff threshold did not suppress compile budget");
        require(near(stalled.mBudgetMs, 0.0), "suppressed handoff frame must have zero compile budget");
        require(near(state.mCreditMs, 0.1875), "handoff suppression did not decay retained credit");
    }

    {
        Resource::P4CompilePolicyState state;
        state.mCreditMs = 2.0;
        const auto noHeadroom = Resource::updateP4CompilePolicy(state, config, { 2, 16.0, 5.0 });
        require(near(noHeadroom.mBudgetMs, 0.0), "mode2 must not spend saved credit without current headroom");
    }

    std::cout << "OptimizedMW GL-P4 compile policy tests passed\n";
    return 0;
}
