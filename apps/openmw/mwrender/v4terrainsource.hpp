#ifndef OPENMW_MWRENDER_V4TERRAINSOURCE_H
#define OPENMW_MWRENDER_V4TERRAINSOURCE_H

#include <components/rendercore/terrainchunkproducer.hpp>
#include <components/rendercore/terrainpreparationservice.hpp>

#include <optional>
#include <string>

namespace MWWorld
{
    class Cell;
}

namespace MWRender
{
    class RenderingManager;
    class TerrainStorage;

    [[nodiscard]] std::string makeV4TerrainChunkIdentity(const MWWorld::Cell& cell);
    [[nodiscard]] RenderCore::TerrainPreparationRequest makeV4TerrainChunkRequest(
        const MWWorld::Cell& cell, std::int32_t gridX, std::int32_t gridY, bool required);

    // Builds one backend-neutral LAND chunk from the authoritative ESM terrain
    // storage. OSG arrays are temporary source-adapter scratch only; no OSG or
    // VSG object crosses into RenderCore ownership.
    [[nodiscard]] std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        const RenderingManager& rendering, const MWWorld::Cell& cell);
    [[nodiscard]] std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        TerrainStorage& storage, const RenderCore::TerrainPreparationRequest& request);
}

#endif
