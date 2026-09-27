#ifndef OPENMW_RENDER_VSG_POPULATIONPLACEMENTREUSE_H
#define OPENMW_RENDER_VSG_POPULATIONPLACEMENTREUSE_H

#include "staticworldplan.hpp"
#include <unordered_map>
#include <unordered_set>

namespace RenderVsg
{
    // Match by stable reference identity, never by a slot that removal/reordering
    // may have reassigned. A hit proves the complete placement contract and all
    // asset dependencies unchanged. Ambiguous identities use the rebuild path.
    inline std::vector<std::optional<std::size_t>> reusablePopulationPlacements(
        const StaticPopulationPlan& previous, const StaticPopulationPlan& next)
    {
        std::vector<std::optional<std::size_t>> result(next.placements.size());
        if (!reusablePopulationAsset(previous, next)) return result;
        std::unordered_map<std::string_view, std::optional<std::size_t>> indices;
        indices.reserve(previous.placements.size());
        for (std::size_t i = 0; i < previous.placements.size(); ++i)
        {
            const auto& identity = previous.placements[i].sourceIdentity;
            if (identity.empty()) continue;
            auto [entry, inserted] = indices.emplace(identity, i);
            if (!inserted) entry->second.reset();
        }
        std::unordered_set<std::string_view> seen, duplicates;
        for (const auto& placement : next.placements)
            if (!seen.insert(placement.sourceIdentity).second) duplicates.insert(placement.sourceIdentity);
        for (std::size_t i = 0; i < next.placements.size(); ++i)
        {
            const auto& placement = next.placements[i];
            const auto found = indices.find(placement.sourceIdentity);
            if (found != indices.end() && found->second && !duplicates.contains(placement.sourceIdentity)
                && equivalentPopulationPlacement(previous.placements[*found->second], placement))
                result[i] = found->second;
        }
        return result;
    }
}
#endif
