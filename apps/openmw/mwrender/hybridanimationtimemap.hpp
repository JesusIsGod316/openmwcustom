#ifndef OPENMW_MWRENDER_HYBRIDANIMATIONTIMEMAP_H
#define OPENMW_MWRENDER_HYBRIDANIMATIONTIMEMAP_H

#include <algorithm>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace MWRender
{
    // Gameplay time and visual-only first-person time at the same semantic
    // animation phase. Built when a group starts, then read without allocation.
    using HybridTimeAnchor = std::pair<float, float>;

    inline bool appendHybridTimeAnchor(std::vector<HybridTimeAnchor>& anchors, float gameplay, float visual)
    {
        if (!std::isfinite(gameplay) || !std::isfinite(visual)
            || (!anchors.empty() && (gameplay <= anchors.back().first || visual <= anchors.back().second)))
            return false;
        anchors.emplace_back(gameplay, visual);
        return true;
    }

    inline float mapHybridAnimationTime(std::span<const HybridTimeAnchor> anchors, float gameplay)
    {
        if (anchors.size() < 2 || !std::isfinite(gameplay))
            return gameplay;
        const auto upper = std::upper_bound(anchors.begin(), anchors.end(), gameplay,
            [](float value, const HybridTimeAnchor& anchor) { return value < anchor.first; });
        if (upper == anchors.begin())
            return anchors.front().second;
        if (upper == anchors.end())
            return anchors.back().second;
        const auto& before = *(upper - 1);
        const float fraction = (gameplay - before.first) / (upper->first - before.first);
        return before.second + fraction * (upper->second - before.second);
    }
}

#endif
