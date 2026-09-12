#ifndef OPENMW_COMPONENTS_RENDERCORE_NAMEDVISUALSEMANTICS_H
#define OPENMW_COMPONENTS_RENDERCORE_NAMEDVISUALSEMANTICS_H

#include <cstdint>

namespace RenderCore
{
    // Backend-neutral realization state for the authored night/day switch.
    // The numeric values preserve the shared authored-state ordering:
    // default/day = 0, exterior night = 1, and lit-interior day = 2.
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

    // These high semantic bits carry source capability/state across the neutral
    // instance/population contract. OpenMW only activates the named switches
    // when the NIF root has the matching user-description label; node names by
    // themselves are insufficient. Dense placements sharing a model therefore
    // carry the same capability bits, while harvested state remains per reference.
    inline constexpr std::uint64_t NightDaySwitchCapabilitySemanticFlag = std::uint64_t{ 1 } << 61u;
    inline constexpr std::uint64_t HerbalismSwitchCapabilitySemanticFlag = std::uint64_t{ 1 } << 62u;
    inline constexpr std::uint64_t HerbalismHarvestedSemanticFlag = std::uint64_t{ 1 } << 63u;
}

#endif