#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_PIPELINEAUDITGATE_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_PIPELINEAUDITGATE_H

#include <cstdint>
#include <limits>

namespace RenderVsg
{
    // Correctness-preserving steady-state gate for the expensive per-view
    // pipeline census. A compile/topology event invalidates the accepted
    // generation; a view-set signature change independently forces another
    // census. Strict QC deliberately bypasses the fast path.
    class PipelineAuditGate final
    {
    public:
        void invalidate() noexcept
        {
            if (mTopologySerial == std::numeric_limits<std::uint64_t>::max())
            {
                mTopologySerial = 1;
                mAcceptedSerial = 0;
                mAcceptedViewSignature = 0;
                mPrimed = false;
                return;
            }
            ++mTopologySerial;
        }

        [[nodiscard]] bool needsAudit(std::uint64_t viewSignature, bool strictQc = false) const noexcept
        {
            return strictQc || !mPrimed || mAcceptedSerial != mTopologySerial
                || mAcceptedViewSignature != viewSignature;
        }

        void accept(std::uint64_t viewSignature) noexcept
        {
            mAcceptedSerial = mTopologySerial;
            mAcceptedViewSignature = viewSignature;
            mPrimed = true;
        }

        [[nodiscard]] std::uint64_t topologySerial() const noexcept { return mTopologySerial; }
        [[nodiscard]] std::uint64_t acceptedSerial() const noexcept { return mAcceptedSerial; }
        [[nodiscard]] bool primed() const noexcept { return mPrimed; }

    private:
        std::uint64_t mTopologySerial = 1;
        std::uint64_t mAcceptedSerial = 0;
        std::uint64_t mAcceptedViewSignature = 0;
        bool mPrimed = false;
    };
}

#endif
