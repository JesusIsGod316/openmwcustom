#include <components/render/backend/vsg/staticassetrealizer.hpp>

#include <vsg/all.h>

#include <cassert>
#include <cstdint>
#include <memory>

int main()
{
    using namespace RenderCore;

    RenderWorld world;

    const auto texture = world.reserveTexture();
    assert(texture);
    TextureRecord textureRecord;
    textureRecord.sourceIdentity = "builtin:cp3b3-checker";
    textureRecord.contentIdentity = "cp3b3:checker:v1";
    textureRecord.width = 2u;
    textureRecord.height = 2u;
    textureRecord.mipmapped = false;
    assert(world.commit(*texture, std::move(textureRecord)));

    const auto material = world.reserveMaterial();
    assert(material);
    MaterialRecord materialRecord;
    materialRecord.sourceIdentity = "cp3b3:material";

    TextureBinding diffuse;
    diffuse.texture = *texture;
    diffuse.role = TextureRole::Diffuse;
    diffuse.colorSpace = TextureColorSpace::Srgb;
    diffuse.formatClass = TextureFormatClass::Color;
    diffuse.sampler.mipmapMode = TextureMipmapMode::None;
    materialRecord.textures.push_back(diffuse);

    TextureBinding normal = diffuse;
    normal.role = TextureRole::Normal;
    normal.colorSpace = TextureColorSpace::Data;
    normal.formatClass = TextureFormatClass::Normal;
    materialRecord.textures.push_back(normal);
    assert(world.commit(*material, std::move(materialRecord)));

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.assign(3u, glm::vec3(0.0f, 0.0f, 1.0f));
    meshPayload->texCoordSets.push_back({ { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 1.0f } });
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    assert(validMeshPayload(*meshPayload));

    const auto mesh = world.reserveMesh();
    assert(mesh);
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "cp3b3:mesh";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    assert(world.commit(*mesh, std::move(meshRecord)));

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord geometry;
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials.push_back(*material);
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });
    assert(validModelPayloadStructure(*modelPayload));

    const auto model = world.reserveModel();
    assert(model);
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "cp3b3:synthetic-model";
    modelRecord.contentIdentity = "cp3b3:synthetic-model:v1";
    modelRecord.payload = modelPayload;
    assert(world.commit(*model, std::move(modelRecord)));
    assert(world.valid());

    const auto plan = RenderVsg::buildStaticAssetPlan(world, *model);
    assert(plan);
    assert(plan->draws.size() == 1u);

    std::uint32_t resolverCalls = 0u;
    const RenderVsg::StaticTextureResolver resolver = [&](const TextureRecord& record,
                                                        const TextureRealizationKey& key) -> vsg::ref_ptr<vsg::Data> {
        ++resolverCalls;
        assert(record.contentIdentity == "cp3b3:checker:v1");
        auto image = vsg::ubvec4Array2D::create(2u, 2u);
        image->set(0u, 0u, vsg::ubvec4(255u, 0u, 255u, 255u));
        image->set(1u, 0u, vsg::ubvec4(32u, 32u, 32u, 255u));
        image->set(0u, 1u, vsg::ubvec4(32u, 32u, 32u, 255u));
        image->set(1u, 1u, vsg::ubvec4(255u, 0u, 255u, 255u));
        image->properties.format = key.view.colorSpace == TextureColorSpace::Srgb
            ? VK_FORMAT_R8G8B8A8_SRGB
            : VK_FORMAT_R8G8B8A8_UNORM;
        image->properties.mipLevels = 1u;
        return image;
    };

    RenderVsg::StaticAssetRealizer realizer;
    const RenderVsg::StaticRealizationResult realized = realizer.realize(world, *plan, resolver);
    assert(realized.valid());
    assert(realized.root->children.size() == 1u);
    assert(realized.stats.drawCount == 1u);
    assert(realized.stats.pipelineKeys == 1u);
    assert(realized.stats.materialKeys == 1u);
    assert(realized.stats.textureViewKeys == 2u);
    assert(realized.stats.samplerKeys == 1u);
    assert(realized.stats.textureLoads == 2u);
    assert(realized.stats.unsupportedTextureBindings == 0u);
    assert(resolverCalls == 2u);

    return 0;
}
