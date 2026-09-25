#include "groundcoversource.hpp"

#include "referenceplacement.hpp"

#include "../groundcoverquery.hpp"

#include <components/rendercore/renderworld.hpp>

#include <algorithm>
#include <string>

namespace MWRender::VulkanMW
{
    GroundcoverPopulationSource makeGroundcoverPopulationSource(const MWWorld::GroundcoverStore& store,
        float density, RenderNative::NifAssetService& assets, const RenderCore::RenderWorld& world,
        std::string_view worldspaceIdentity, std::int32_t gridX, std::int32_t gridY, float renderingDistance)
    {
        GroundcoverPopulationSource result;
        if (worldspaceIdentity.empty())
        {
            result.diagnostic = "groundcover source requires a worldspace identity";
            return result;
        }

        result.cell.identity = "groundcover:" + std::string(worldspaceIdentity) + ":" + std::to_string(gridX) + ","
            + std::to_string(gridY);
        result.cell.worldspaceIdentity = std::string(worldspaceIdentity);
        result.cell.gridX = gridX;
        result.cell.gridY = gridY;
        result.cell.groundcover = true;

        const GroundcoverInstanceMap instances = collectGroundcoverInstances(
            store, density, 1.0f, static_cast<float>(gridX) + 0.5f, static_cast<float>(gridY) + 0.5f);
        for (const auto& [modelPath, entries] : instances)
        {
            const RenderNative::NifAssetResolveResult resolved = assets.resolve(modelPath);
            if (!resolved.available())
            {
                result.diagnostic = "groundcover model publication failed for " + modelPath.value();
                if (!resolved.diagnostic.empty())
                    result.diagnostic += ": " + resolved.diagnostic;
                return result;
            }
            const RenderCore::ModelRecord* model = world.get(resolved.model);
            if (!model)
            {
                result.diagnostic = "groundcover model cache returned a stale handle for " + modelPath.value();
                return result;
            }

            for (const GroundcoverEntry& entry : entries)
            {
                RenderCore::StaticPopulationInstanceSource source;
                source.identity = "groundcover:" + entry.mRefNum.toString();
                source.cellIdentity = result.cell.identity;
                source.model = resolved.model;
                source.transform.translation = { entry.mPos.pos[0], entry.mPos.pos[1], entry.mPos.pos[2] };
                source.transform.rotation = makeReferenceRotation(entry.mPos);
                source.transform.scale = { entry.mScale, entry.mScale, entry.mScale };
                source.localBounds = model->bounds;
                source.lod.maximumDistance = std::max(0.0f, renderingDistance);
                source.semanticFlags &= ~RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::ShadowCaster);
                result.instances.push_back(std::move(source));
            }
        }

        return result;
    }
}
