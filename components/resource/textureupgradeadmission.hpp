#ifndef OPENMW_RESOURCE_TEXTUREUPGRADEADMISSION_H
#define OPENMW_RESOURCE_TEXTUREUPGRADEADMISSION_H

#include "speculativebudget.hpp"
#include <cmath>

namespace Resource::TextureUpgradeAdmission
{
    struct VisibleCoarseUse
    {
        bool streaming = false, current = false, mainDraw = false, coarse = false;
        bool drawn = false, demoting = false;
        double now = 0, lastDrawTime = -1;
        std::uint64_t frame = 0, lastDrawFrame = 0;
    };

    inline bool recoveryPromotion(const VisibleCoarseUse& use, const OpenGlPressureSample& sample) noexcept
    {
        return sample.decision.state == OpenGlPressure::Recovering
            && sample.sampledAtMs && SpeculativeBudget::newOwnerWorkUsable(sample, SpeculativePriority::NearFuture)
            && use.streaming && use.current && use.mainDraw && use.coarse && use.drawn && !use.demoting
            && std::isfinite(use.now) && std::isfinite(use.lastDrawTime)
            && use.now >= use.lastDrawTime && use.now - use.lastDrawTime < 1.0
            && use.frame >= use.lastDrawFrame && use.frame - use.lastDrawFrame <= 1;
    }

    struct SampleChanged : std::exception
    {
        OpenGlPressureSample expected, observed;
        SampleChanged(OpenGlPressureSample a, OpenGlPressureSample b) : expected(a), observed(b) {}
        const char* what() const noexcept override { return "texture recovery admission sample changed"; }
    };

    // Job admission and every allocation-stage sample must refer to the same
    // published Recovering observation. Drift defers this optional upgrade;
    // it never rewrites Caution into another state or uses its lower floors.
    struct FrozenRecoverySample
    {
        OpenGlPressureSample selected;
        SpeculativeScope::Sample source = nullptr;
        void* owner = nullptr;
        static OpenGlPressureSample read(void* self)
        {
            auto& frozen = *static_cast<FrozenRecoverySample*>(self);
            const auto current = frozen.source ? frozen.source(frozen.owner) : OpenGlPressureSample{};
            if (current.decision.state != OpenGlPressure::Recovering
                || current.generation != frozen.selected.generation
                || current.sampledAtMs != frozen.selected.sampledAtMs)
                throw SampleChanged(frozen.selected, current);
            return current;
        }
    };
}
#endif
