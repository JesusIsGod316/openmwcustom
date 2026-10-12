#ifndef OPENMW_RESOURCE_P9DISCRETIONARYADMISSION_H
#define OPENMW_RESOURCE_P9DISCRETIONARYADMISSION_H

#include <algorithm>
#include <cmath>
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
                mBootstrapUsed = false;
                mNoGlAttempts = 0;
                mNoGlChargedMs = 0.0;
                mCompositeMaintenanceVisits = 0;
                mCompositeMaintenanceMs = 0.0;
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
            if (age < std::max(1u, maxAge))
                return false;
            return takeExceptionalService();
        }
        bool takeExceptionalService()
        {
            if (!mStarted || mProgressUsed)
                return false;
            mProgressUsed = true;
            return true;
        }
        bool takeBootstrap(double predictedMs, double remainingHeadroomMs, bool handoffSuppressed)
        {
            if (handoffSuppressed || !std::isfinite(predictedMs) || predictedMs <= 0.0
                || !std::isfinite(remainingHeadroomMs) || !std::isfinite(mChargedMs)
                || predictedMs + mChargedMs > remainingHeadroomMs || !takeExceptionalService())
                return false;
            mBootstrapUsed = true;
            return true;
        }
        bool bootstrapUsed() const { return mBootstrapUsed; }
        unsigned compositeMaintenanceVisits() const { return mCompositeMaintenanceVisits; }
        bool takeCompositeMaintenanceVisit(unsigned maximum = 16)
        {
            if (!mStarted || mCompositeMaintenanceVisits >= maximum) return false;
            ++mCompositeMaintenanceVisits;
            return true;
        }
        double compositeMaintenanceMs() const { return mCompositeMaintenanceMs; }
        void chargeCompositeMaintenance(double actualMs, bool chargeBudget = true)
        {
            mCompositeMaintenanceMs += std::max(0.0, actualMs);
            if (chargeBudget) charge(actualMs);
        }
        // Attempts, including failed proofs, share a strict owner-frame cap.
        // One indivisible CPU proof can overrun the time estimate; no GL is
        // permitted on this lane, and its actual time is still charged.
        bool canAttemptNoGl(unsigned maxAttempts, double maxCpuMs) const
        {
            return mStarted && maxAttempts && std::isfinite(maxCpuMs) && maxCpuMs > 0.0
                && mNoGlAttempts < maxAttempts && mNoGlChargedMs < maxCpuMs;
        }
        bool takeNoGlAttempt(unsigned maxAttempts, double maxCpuMs)
        {
            if (!canAttemptNoGl(maxAttempts, maxCpuMs)) return false;
            ++mNoGlAttempts;
            return true;
        }
        void chargeNoGlAttempt(double actualMs)
        {
            mNoGlChargedMs += std::max(0.0, actualMs);
            charge(actualMs);
        }
        void charge(double actualMs) { mChargedMs += std::max(0.0, actualMs); }

    private:
        const void* mContext = nullptr;
        unsigned mFrame = 0;
        double mBudgetMs = 0.0;
        double mChargedMs = 0.0;
        double mNoGlChargedMs = 0.0;
        double mCompositeMaintenanceMs = 0.0;
        unsigned mNoGlAttempts = 0;
        unsigned mCompositeMaintenanceVisits = 0;
        bool mProgressUsed = false;
        bool mBootstrapUsed = false;
        bool mStarted = false;
    };
}
#endif
