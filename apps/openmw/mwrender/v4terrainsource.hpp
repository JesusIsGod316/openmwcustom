#ifndef OPENMW_MWRENDER_V4TERRAINSOURCE_H
#define OPENMW_MWRENDER_V4TERRAINSOURCE_H

#include <components/rendercore/terrainchunkproducer.hpp>

#include <optional>
#include <string>

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    class RenderingManager;

    [[nodiscard]] std::string makeV4TerrainChunkIdentity(const MWWorld::Cell& cell);

    // Builds one backend-neutral LAND chunk from the authoritative ESM terrain
    // storage. OSG arrays are temporary source-adapter scratch only; no OSG or
    // VSG object crosses into RenderCore ownership.
    [[nodiscard]] std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        const RenderingManager& rendering, const MWWorld::Cell& cell);
}

#endif
