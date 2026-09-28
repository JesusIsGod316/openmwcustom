#ifndef OPENMW_MWRENDER_V4PRODUCERCLASS_H
#define OPENMW_MWRENDER_V4PRODUCERCLASS_H

#include <cstdint>

namespace MWRender
{
    // Scheduling ownership is deliberately separate from renderer/native status.
    // A supported continuous producer is understood and explicitly scheduled,
    // even when its simulation/pose must still advance every frame.
    enum class V4ProducerClass : std::uint8_t
    {
        CompatibilityContinuous = 0,
        DemandDrivenObject,
        SupportedContinuousActor,
        SupportedContinuousParticle,
    };

    [[nodiscard]] constexpr bool v4ProducerClassIsSupported(V4ProducerClass value) noexcept
    {
        return value != V4ProducerClass::CompatibilityContinuous;
    }

    [[nodiscard]] constexpr bool v4ProducerClassIsSupportedContinuous(V4ProducerClass value) noexcept
    {
        return value == V4ProducerClass::SupportedContinuousActor
            || value == V4ProducerClass::SupportedContinuousParticle;
    }

    // Only unknown/compatibility work belongs to ProducerQueue's generic
    // continuous set. Supported actors and particles are scheduled explicitly
    // by V4ObjectQueue so they cannot inflate or rediscover that fallback lane.
    [[nodiscard]] constexpr bool v4ProducerClassUsesCompatibilityContinuousQueue(
        V4ProducerClass value) noexcept
    {
        return value == V4ProducerClass::CompatibilityContinuous;
    }

    [[nodiscard]] constexpr bool v4ProducerClassIsContinuous(V4ProducerClass value) noexcept
    {
        return value == V4ProducerClass::CompatibilityContinuous
            || v4ProducerClassIsSupportedContinuous(value);
    }
}

#endif
