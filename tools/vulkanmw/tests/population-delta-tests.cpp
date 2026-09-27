#include <components/render/backend/vsg/populationplacementreuse.hpp>
#include <components/render/backend/vsg/landdepthoccluder.hpp>
#include <iostream>
#include <stdexcept>

namespace
{
    unsigned checks = 0;
    void check(bool value, const char* message)
    {
        ++checks;
        if (!value) throw std::runtime_error(message);
    }
}

int main()
{
    try
    {
        using namespace RenderVsg;
        StaticPopulationPlan previous;
        previous.sourceEpoch = RenderCore::WorldEpoch(1);
        previous.model = RenderCore::ModelHandle::fromParts(0, 1);
        previous.modelRevision = RenderCore::ResourceRevision(1);
        for (unsigned i = 0; i < 191; ++i)
        {
            RenderCore::PopulationInstanceRecord placement;
            placement.sourceIdentity = "bottle:" + std::to_string(i);
            placement.transform.translation.x = i;
            previous.placements.push_back(placement);
        }
        auto next = previous;
        next.chunkRevision = RenderCore::ResourceRevision(99);
        next.placements[17].transform.translation.y = 5;
        auto reuse = reusablePopulationPlacements(previous, next);
        std::size_t count = 0;
        for (std::size_t i = 0; i < reuse.size(); ++i)
        {
            check(i == 17 ? !reuse[i] : reuse[i] == i, "movement must replace only the changed placement");
            count += reuse[i].has_value();
        }
        check(count == 190, "191 placements must reuse 190 on one move");
        std::reverse(next.placements.begin(), next.placements.end());
        reuse = reusablePopulationPlacements(previous, next);
        check(reuse.front() == 190 && reuse.back() == 0 && !reuse[173], "reordering lost identity");
        next.placements.erase(next.placements.begin() + 4);
        reuse = reusablePopulationPlacements(previous, next);
        check(reuse[4] == 185, "removal reassigned a placement identity");
        next.placements[0].semanticFlags ^= 1;
        next.placements[1].lightingEnabled = !next.placements[1].lightingEnabled;
        next.placements[2].localBounds.maximum.x += 1;
        next.placements[3].lod.maximumDistance = 42;
        reuse = reusablePopulationPlacements(previous, next);
        for (std::size_t i = 0; i < 4; ++i) check(!reuse[i], "changed semantic/bounds/LOD reused");
        next = previous;
        next.placements[0].sourceIdentity.clear();
        next.placements[1].sourceIdentity = next.placements[2].sourceIdentity;
        reuse = reusablePopulationPlacements(previous, next);
        check(!reuse[0] && !reuse[1] && !reuse[2], "ambiguous new identities reused");
        next = previous;
        previous.placements[0].sourceIdentity = previous.placements[1].sourceIdentity;
        reuse = reusablePopulationPlacements(previous, next);
        check(!reuse[0] && !reuse[1], "ambiguous old identities reused");
        previous = next;
        const auto rejectAll = [&] {
            const auto values = reusablePopulationPlacements(previous, next);
            check(std::none_of(values.begin(), values.end(), [](const auto& value) { return bool(value); }),
                "changed asset dependency reused a node");
        };
        next.modelRevision = RenderCore::ResourceRevision(2); rejectAll(); next = previous;
        next.sourceEpoch = RenderCore::WorldEpoch(2); rejectAll(); next = previous;
        next.options.includeDeformableMeshes = !next.options.includeDeformableMeshes; rejectAll(); next = previous;
        next.meshes.push_back({RenderCore::MeshHandle::fromParts(0, 1), RenderCore::ResourceRevision(1)});
        rejectAll(); next = previous;
        next.materials.push_back({RenderCore::MaterialHandle::fromParts(0, 1), RenderCore::ResourceRevision(1)});
        rejectAll(); next = previous;
        next.textures.push_back({RenderCore::TextureHandle::fromParts(0, 1), RenderCore::ResourceRevision(1)});
        rejectAll();
        RenderCore::MaterialRecord land;
        land.terrainLayer = RenderCore::TerrainLayerSemantic{true, false, false};
        check(landDepthOccluder(land, false), "ordinary opaque LAND rejected");
        land.alphaBlendEnabled = true;
        land.destinationBlend = RenderCore::BlendFactor::Zero;
        check(!landDepthOccluder(land, false) && landDepthOccluder(land, true), "weighted LAND base eligibility");
        const auto base = land;
        land.terrainLayer->first = false;
        check(!landDepthOccluder(land, true), "blended overlay admitted"); land = base;
        land.destinationBlend = RenderCore::BlendFactor::One;
        check(!landDepthOccluder(land, true), "additive overlay admitted"); land = base;
        land.terrainLayer.reset();
        check(!landDepthOccluder(land, true), "ordinary blended object admitted"); land = base;
        for (auto field : {&RenderCore::MaterialRecord::alphaTestEnabled, &RenderCore::MaterialRecord::wireframe,
                 &RenderCore::MaterialRecord::decal, &RenderCore::MaterialRecord::treeAnimation,
                 &RenderCore::MaterialRecord::refraction, &RenderCore::MaterialRecord::softEffect})
        {
            land.*field = true; check(!landDepthOccluder(land, true), "coverage-changing material admitted"); land = base;
        }
        land.depthWrite = false; check(!landDepthOccluder(land, true), "non-depth-writing LAND admitted"); land = base;
        land.depthTest = false; check(!landDepthOccluder(land, true), "depth-disabled LAND admitted"); land = base;
        land.stencil.enabled = true; check(!landDepthOccluder(land, true), "stencil LAND admitted");
        std::cout << "PASS population delta reuse: " << checks << " checks\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
