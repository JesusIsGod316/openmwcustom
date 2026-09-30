#ifndef OPENMW_RESOURCE_P9DISCRETIONARYADMISSION_H
#define OPENMW_RESOURCE_P9DISCRETIONARYADMISSION_H

#include <algorithm>
#include <cstdint>

namespace Resource
{
    // Owned by the existing ICO and used only on its single draw context. Both
    // preparation and baking debit this account. A repeated traversal cannot
    // replenish it. A lower later estimate can reduce, never expand, the cap.
    // This is CPU submission admission, not a driver/GPU time guarantee.
    class P9DiscretionaryAdmission
    {
    public:
        void begin(const void* context, unsigned frame, double budgetMs)
        {
            budgetMs = std::max(0.0, budgetMs);
            if (context != mContext || frame != mFrame || !mStarted)
            {
                mContext = context;
                mFrame = frame;
                mBudgetMs = budgetMs;
                mChargedMs = 0.0;
                mProgressUsed = false;
                mStarted = true;
            }
            else
                mBudgetMs = std::min(mBudgetMs, budgetMs);
        }
        double remainingMs() const { return std::max(0.0, mBudgetMs - mChargedMs); }
        double chargedMs() const { return mChargedMs; }
        bool fits(double predictedMs) const { return predictedMs <= remainingMs(); }
        bool takeProgress(unsigned age, unsigned maxAge)
        {
            if (mProgressUsed || age < std::max(1u, maxAge))
                return false;
            mProgressUsed = true;
            return true;
        }
        void charge(double actualMs) { mChargedMs += std::max(0.0, actualMs); }

    private:
        const void* mContext = nullptr;
        unsigned mFrame = 0;
        double mBudgetMs = 0.0;
        double mChargedMs = 0.0;
        bool mProgressUsed = false;
        bool mStarted = false;
    };
}
#endif
