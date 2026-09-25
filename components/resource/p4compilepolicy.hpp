#ifndef OPENMW_COMPONENTS_RESOURCE_P4COMPILEPOLICY_H
#define OPENMW_COMPONENTS_RESOURCE_P4COMPILEPOLICY_H

#include <algorithm>

namespace Resource
{
    struct P4CompilePolicyState
    {
        double mCreditMs = 0.0;
    };

    struct P4CompilePolicyConfig
    {
        double mTargetFrameMs = 16.6667;
        double mMaxBudgetMs = 2.0;
        double mCreditCapMs = 2.5;
        double mHeadroomRatio = 0.25;
        double mHandoffThresholdMs = 20.0;
    };

    struct P4CompilePolicyInput
    {
        int mMode = 0;
        double mCurrentElapsedMs = 0.0;
        double mLastHandoffMs = 0.0;
    };

    struct P4CompilePolicyOutput
    {
        double mHeadroomMs = 0.0;
        double mBudgetMs = 0.0;
        double mCreditMs = 0.0;
        bool mSuppressedByHandoff = false;
    };

    inline P4CompilePolicyOutput updateP4CompilePolicy(
        P4CompilePolicyState& state, const P4CompilePolicyConfig& config, const P4CompilePolicyInput& input)
    {
        P4CompilePolicyOutput result;
        result.mHeadroomMs = std::max(0.0, config.mTargetFrameMs - input.mCurrentElapsedMs);

        if (input.mMode <= 0)
        {
            state.mCreditMs = 0.0;
            return result;
        }

        if (input.mMode == 1)
        {
            result.mBudgetMs = std::min(config.mMaxBudgetMs, result.mHeadroomMs * config.mHeadroomRatio);
            result.mCreditMs = result.mBudgetMs;
            state.mCreditMs = 0.0;
            return result;
        }

        if (input.mLastHandoffMs >= config.mHandoffThresholdMs)
        {
            state.mCreditMs *= 0.25;
            result.mSuppressedByHandoff = true;
        }
        else
        {
            double replenish = result.mHeadroomMs * config.mHeadroomRatio;
            if (input.mLastHandoffMs >= config.mTargetFrameMs * 0.9)
                replenish *= 0.25;
            state.mCreditMs = std::min(config.mCreditCapMs, state.mCreditMs + replenish);
        }

        if (!result.mSuppressedByHandoff && result.mHeadroomMs >= 0.25)
            result.mBudgetMs = std::min(config.mMaxBudgetMs, state.mCreditMs);

        result.mCreditMs = state.mCreditMs;
        return result;
    }

    inline void consumeP4CompileCredit(P4CompilePolicyState& state, int mode, double actualMs)
    {
        if (mode >= 2)
            state.mCreditMs = std::max(0.0, state.mCreditMs - std::max(0.0, actualMs));
    }
}

#endif
