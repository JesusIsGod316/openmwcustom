#ifndef OPENMW_COMPONENTS_RENDERCORE_NAMEDVISUALSEMANTICS_H
#define OPENMW_COMPONENTS_RENDERCORE_NAMEDVISUALSEMANTICS_H

#include <cstdint>

namespace RenderCore
{
    // Backend-neutral realization state for OpenMW's authored NightDaySwitch.
    // The numeric values intentionally match MWWorld::NightDayMode and the
    // legacy osg::Switch child indices: default/day = 0, exterior night = 1,
    // and lit-interior day = 2.
    enum class NightDaySwitchState : std::uint8_t
    {
        Default = 0,
        ExteriorNight = 1,
        InteriorDay = 2,
    };

    [[nodiscard]] constexpr bool validNightDaySwitchState(NightDaySwitchState value) noexcept
    {
        return value == NightDaySwitchState::Default || value == NightDaySwitchState::ExteriorNight
            || value == NightDaySwitchState::InteriorDay;
    }

    // Per-reference graphic-herbalism state. InstanceRecord::semanticFlags is
    // already the neutral per-instance semantic extension field; reserve the
    // high bit here rather than conflating this visual state with view/caster
    // classification bits in InstanceSemanticFlag.
    inline constexpr std::uint64_t HerbalismHarvestedSemanticFlag = std::uint64_t{ 1 } << 63u;
}

#endif
