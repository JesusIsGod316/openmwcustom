#include <components/render/backend/vsg/staticassetrealizer.hpp>
#include <components/render/backend/vsg/statictexturedecode.hpp>

#include <vsg/all.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
    void require(bool condition, std::string_view message)
    {
        if (!condition)
            throw std::runtime_error(std::string(message));
    }

    template <class State>
    [[nodiscard]] const State* findPipelineState(const vsg::StateGroup& stateGroup)
    {
        for (const auto& command : stateGroup.stateCommands)
        {
            const auto* bind = dynamic_cast<const vsg::BindGraphicsPipeline*>(command.get());
            if (bind == nullptr || !bind->pipeline)
                continue;
            for (const auto& state : bind->pipeline->pipelineStates)
            {
                if (const auto* typed = dynamic_cast<const State*>(state.get()))
                    return typed;
            }
        }
        return nullptr;
    }
}

int main()
{
    using namespace RenderCore;

    RenderWorld world;

    const auto texture = world.reserveTexture();
    require(texture.has_value(), "failed to reserve texture");
    TextureRecord textureRecord;
    textureRecord.sourceIdentity = "builtin:cp3b3-checker";
    textureRecord.contentIdentity = "cp3b3:checker:v1";
    textureRecord.width = 2u;
    textureRecord.height = 2u;
    textureRecord.mipmapped = false;
    require(world.commit(*texture, std::move(textureRecord)), "failed to commit texture");

    const auto material = world.reserveMaterial();
    require(material.has_value(), "failed to reserve material");
    MaterialRecord materialRecord;
    materialRecord.sourceIdentity = "cp3b3:material";
    materialRecord.decal = true;

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
    require(world.commit(*material, std::move(materialRecord)), "failed to commit material");

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.assign(3u, glm::vec3(0.0f, 0.0f, 1.0f));
    meshPayload->colors.assign(3u, glm::vec4(0.75f, 0.5f, 0.25f, 1.0f));
    meshPayload->texCoordSets.push_back({ { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 1.0f } });
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    require(validMeshPayload(*meshPayload), "synthetic mesh payload is invalid");

    const auto mesh = world.reserveMesh();
    require(mesh.has_value(), "failed to reserve mesh");
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "cp3b3:mesh";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    require(world.commit(*mesh, std::move(meshRecord)), "failed to commit mesh");

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord geometry;
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials.push_back(*material);
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });
    require(validModelPayloadStructure(*modelPayload), "synthetic model payload is invalid");

    const auto model = world.reserveModel();
    require(model.has_value(), "failed to reserve model");
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "cp3b3:synthetic-model";
    modelRecord.contentIdentity = "cp3b3:synthetic-model:v1";
    modelRecord.payload = modelPayload;
    require(world.commit(*model, std::move(modelRecord)), "failed to commit model");
    require(world.valid(), "RenderWorld is invalid after synthetic publication");

    const auto plan = RenderVsg::buildStaticAssetPlan(world, *model);
    require(plan.has_value(), "failed to build static asset plan");
    require(plan->draws.size() == 1u, "static asset plan must contain exactly one draw");

    std::uint32_t resolverCalls = 0u;
    const RenderVsg::StaticTextureResolver resolver = [&](const TextureRecord& record,
                                                        const TextureRealizationKey& key) -> vsg::ref_ptr<vsg::Data> {
        ++resolverCalls;
        require(record.contentIdentity == "cp3b3:checker:v1", "resolver received unexpected texture content");
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
    require(realized.valid(), "static VSG realization is invalid");
    require(realized.root->children.size() == 1u, "realized root must contain exactly one draw node");
    require(realized.stats.drawCount == 1u, "unexpected realized draw count");
    require(realized.stats.pipelineKeys == 1u, "unexpected pipeline-key count");
    require(realized.stats.materialKeys == 1u, "unexpected material-key count");
    require(realized.stats.textureViewKeys == 2u, "sRGB and data views must realize separately");
    require(realized.stats.samplerKeys == 1u, "identical samplers must share one realization key");
    require(realized.stats.textureLoads == 2u, "each texture view variant must be resolved exactly once");
    require(realized.stats.unsupportedTextureBindings == 0u, "synthetic supported bindings were rejected");
    require(realized.stats.runtimeContextEffects == 0u, "supported synthetic material unexpectedly failed closed");
    require(resolverCalls == 2u, "texture resolver call count does not match realization variants");

    const auto* transform = dynamic_cast<const vsg::MatrixTransform*>(realized.root->children.front().get());
    require(transform != nullptr && transform->children.size() == 1u,
        "realized draw must retain the expected MatrixTransform/StateGroup shape");
    const auto* stateGroup = dynamic_cast<const vsg::StateGroup*>(transform->children.front().get());
    require(stateGroup != nullptr, "realized draw is missing its StateGroup");

    const vsg::DepthStencilState* depth = findPipelineState<vsg::DepthStencilState>(*stateGroup);
    require(depth != nullptr, "realized pipeline is missing DepthStencilState");
    require(depth->depthTestEnable == VK_TRUE, "synthetic depth test unexpectedly disabled");
    require(depth->depthWriteEnable == VK_TRUE, "synthetic depth write unexpectedly disabled");
    require(depth->depthCompareOp == VK_COMPARE_OP_GREATER_OR_EQUAL,
        "CP3B3 pipeline must use VSG reverse-depth GREATER_OR_EQUAL comparison");

    const vsg::RasterizationState* raster = findPipelineState<vsg::RasterizationState>(*stateGroup);
    require(raster != nullptr, "realized pipeline is missing RasterizationState");
    require(raster->depthBiasEnable == VK_TRUE, "synthetic decal did not enable depth bias");
    require(raster->depthBiasConstantFactor > 0.0f && raster->depthBiasSlopeFactor > 0.0f,
        "reverse-depth decal bias must move toward the camera with positive factors");

    // The legacy compatibility path may publish only semantics it actually expresses.
    // Exercise the remaining fail-closed counters so complete=true cannot silently bless
    // static material behavior that still needs a dedicated compatibility variant.
    const auto unsupportedMaterial = world.reserveMaterial();
    require(unsupportedMaterial.has_value(), "failed to reserve unsupported semantic material");
    MaterialRecord unsupportedRecord;
    unsupportedRecord.sourceIdentity = "cp3b3:unsupported-material";
    unsupportedRecord.unlit = true;
    unsupportedRecord.vertexColorMode = VertexColorMode::Emissive;
    unsupportedRecord.textureApply = TextureApplyMode::Replace;
    TextureBinding transformedDiffuse = diffuse;
    transformedDiffuse.transform.offset.x = 0.25f;
    unsupportedRecord.textures.push_back(transformedDiffuse);
    require(world.commit(*unsupportedMaterial, std::move(unsupportedRecord)),
        "failed to commit unsupported semantic material");

    auto unsupportedModelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord unsupportedGeometry;
    unsupportedGeometry.kind = ModelNodeKind::Geometry;
    unsupportedGeometry.mesh = *mesh;
    unsupportedGeometry.materials.push_back(*unsupportedMaterial);
    unsupportedModelPayload->nodes.push_back(unsupportedGeometry);
    unsupportedModelPayload->roots.push_back(ModelNodeIndex{ 0u });
    require(validModelPayloadStructure(*unsupportedModelPayload), "unsupported semantic model payload is invalid");

    const auto unsupportedModel = world.reserveModel();
    require(unsupportedModel.has_value(), "failed to reserve unsupported semantic model");
    ModelRecord unsupportedModelRecord;
    unsupportedModelRecord.sourceIdentity = "cp3b3:unsupported-model";
    unsupportedModelRecord.contentIdentity = "cp3b3:unsupported-model:v1";
    unsupportedModelRecord.payload = unsupportedModelPayload;
    require(world.commit(*unsupportedModel, std::move(unsupportedModelRecord)),
        "failed to commit unsupported semantic model");

    const auto unsupportedPlan = RenderVsg::buildStaticAssetPlan(world, *unsupportedModel);
    require(unsupportedPlan.has_value() && unsupportedPlan->draws.size() == 1u,
        "failed to build unsupported semantic plan");
    const RenderVsg::StaticRealizationResult unsupportedRealized
        = realizer.realize(world, *unsupportedPlan, resolver);
    require(unsupportedRealized.valid(), "unsupported semantic realization should remain inspectable");
    require(unsupportedRealized.stats.runtimeContextEffects == 2u,
        "texture-apply/static-UV gaps must each fail closed while unlit and emissive vertex color remain supported");

    RenderVsg::StaticTextureDecoder decoder;
    TextureRecord warningRecord;
    warningRecord.sourceIdentity = std::string(RenderVsg::OpenMwWarningTextureSourceIdentity);
    warningRecord.contentIdentity = std::string(RenderVsg::OpenMwWarningTextureContentIdentity);
    warningRecord.width = 8u;
    warningRecord.height = 8u;
    warningRecord.mipmapped = false;
    const TextureRealizationKey warningKey = makeTextureRealizationKey(diffuse, warningRecord);
    std::uint32_t warningOpenCalls = 0u;
    auto warning = decoder.decode(warningRecord, warningKey, [&](std::string_view) -> Files::IStreamPtr {
        ++warningOpenCalls;
        return {};
    });
    require(static_cast<bool>(warning), "OpenMW warning texture failed to realize");
    require(warningOpenCalls == 0u, "built-in warning texture must not open VFS content");
    require(warning->width() == 8u && warning->height() == 8u, "warning texture dimensions changed");
    require(warning->properties.format == VK_FORMAT_R8G8B8_SRGB, "warning texture format must be sRGB RGB8");
    const auto* warningPixels = dynamic_cast<const vsg::ubvec3Array2D*>(warning.get());
    require(warningPixels != nullptr && warningPixels->size() == 64u, "warning texture payload shape changed");
    for (const vsg::ubvec3& pixel : *warningPixels)
        require(pixel == vsg::ubvec3(255u, 0u, 255u), "warning texture must remain solid magenta");

    TextureRecord streamRecord;
    streamRecord.sourceIdentity = "textures/cp3b3-stream.ppm";
    streamRecord.contentIdentity = "cp3b3:stream:v1";
    streamRecord.width = 1u;
    streamRecord.height = 1u;
    streamRecord.mipmapped = false;
    const std::string ppm("P6\n1 1\n255\n\x7f\x20\xe0", 14u);
    std::uint32_t streamOpenCalls = 0u;
    const RenderVsg::StaticTextureStreamOpener opener = [&](std::string_view path) -> Files::IStreamPtr {
        require(path == streamRecord.sourceIdentity, "decoder requested an unexpected VFS path");
        ++streamOpenCalls;
        return std::make_unique<std::istringstream>(ppm, std::ios::in | std::ios::binary);
    };

    const TextureRealizationKey srgbKey = makeTextureRealizationKey(diffuse, streamRecord);
    auto srgb = decoder.decode(streamRecord, srgbKey, opener);
    require(static_cast<bool>(srgb) && srgb->width() == 1u && srgb->height() == 1u,
        "sRGB stream decode failed");
    require(srgb->properties.format == VK_FORMAT_R8G8B8A8_SRGB, "sRGB stream view realized with wrong format");

    const TextureRealizationKey dataKey = makeTextureRealizationKey(normal, streamRecord);
    auto data = decoder.decode(streamRecord, dataKey, opener);
    require(static_cast<bool>(data) && data->width() == 1u && data->height() == 1u,
        "data stream decode failed");
    require(data->properties.format == VK_FORMAT_R8G8B8A8_UNORM, "data stream view realized with wrong format");
    require(streamOpenCalls == 2u, "stream should be opened once for each distinct interpretation variant");

    TextureRealizationKey staleKey = srgbKey;
    staleKey.revision = ResourceRevision{ streamRecord.revision.value() + 1u };
    require(!decoder.decode(streamRecord, staleKey, opener), "stale texture revision must fail closed");
    require(streamOpenCalls == 2u, "stale revision must be rejected before opening the stream");

    return 0;
}
