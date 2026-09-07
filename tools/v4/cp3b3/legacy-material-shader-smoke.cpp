#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <components/render/backend/vsg/staticassetplan.hpp>
#include <components/render/backend/vsg/staticassetrealizer.hpp>

#include <vsg/all.h>
#include <vsg/utils/ShaderCompiler.h>

#include <cmath>
#include <cstdint>
#include <memory>
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

    [[nodiscard]] bool near(float lhs, float rhs) noexcept
    {
        return std::abs(lhs - rhs) <= 0.00001f;
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

    MaterialRecord semanticMaterial;
    semanticMaterial.diffuse = { 0.2f, 0.3f, 0.4f, 0.9f };
    semanticMaterial.ambient = { 0.1f, 0.15f, 0.2f, 1.0f };
    semanticMaterial.specular = { 0.25f, 0.5f, 0.75f, 1.0f };
    semanticMaterial.emission = { 0.05f, 0.1f, 0.2f, 1.0f };
    semanticMaterial.specularStrength = 2.0f;
    semanticMaterial.emissiveMultiplier = 3.0f;
    semanticMaterial.shininess = 37.0f;
    semanticMaterial.alpha = 0.6f;
    semanticMaterial.alphaCutoff = 0.375f;
    semanticMaterial.alphaTestEnabled = true;
    semanticMaterial.alphaCompare = CompareOp::NotEqual;
    semanticMaterial.alphaBlendEnabled = true;
    semanticMaterial.sourceBlend = BlendFactor::SourceAlpha;
    semanticMaterial.destinationBlend = BlendFactor::OneMinusSourceAlpha;
    semanticMaterial.vertexColorMode = VertexColorMode::AmbientDiffuse;
    semanticMaterial.cullMode = CullMode::None;

    auto uniform = RenderVsg::makeLegacyCompatibilityMaterial(semanticMaterial);
    require(static_cast<bool>(uniform), "failed to create legacy material uniform");
    const RenderVsg::LegacyMaterialUniform& packed = uniform->value();
    require(near(packed.diffuseColor.a, 0.6f), "legacy material alpha packing changed");
    require(near(packed.specularColor.x, 0.5f) && near(packed.specularColor.y, 1.0f)
            && near(packed.specularColor.z, 1.5f),
        "legacy specular strength was not folded into the compatibility uniform");
    require(near(packed.emissiveColor.x, 0.15f) && near(packed.emissiveColor.y, 0.3f)
            && near(packed.emissiveColor.z, 0.6f),
        "legacy emissive multiplier was not folded into the compatibility uniform");
    require(near(packed.parameters.x, 37.0f) && near(packed.parameters.y, 0.375f),
        "legacy shininess/alpha cutoff packing changed");
    require(near(packed.semantics.x, static_cast<float>(VertexColorMode::AmbientDiffuse))
            && near(packed.semantics.y, 1.0f)
            && near(packed.semantics.z, static_cast<float>(CompareOp::NotEqual))
            && near(packed.semantics.w, 1.0f),
        "legacy semantic selectors changed");

    const std::string_view source = RenderVsg::legacyCompatibilityFragmentShaderSource();
    require(source.find("effectiveDiffuse = vertexColor") != std::string_view::npos,
        "AmbientDiffuse vertex RGB/alpha replacement is absent from the legacy shader");
    require(source.find("effectiveEmission = vertexColor") != std::string_view::npos,
        "emissive vertex-color mode is absent from the legacy shader");
    require(source.find("surfaceColor.a *= effectiveDiffuse.a") != std::string_view::npos,
        "legacy vertex/material alpha selection is absent from the shader");
    require(source.find("surfaceColor.rgb *= texture(detailMap, texCoord[texCoordIndices.detailMap].st).rgb * 2.0")
            != std::string_view::npos,
        "legacy detail modulation is absent from the shader");
    require(source.find("case 0: return false") != std::string_view::npos
            && source.find("case 7: return true") != std::string_view::npos,
        "complete legacy alpha comparison table is absent from the shader");
    require(source.find("specularColor = specularSample.rgb") != std::string_view::npos
            && source.find("shininess = specularSample.a * 255.0") != std::string_view::npos,
        "legacy specular-map RGB/shininess semantics are absent from the shader");
    require(source.find("outColor.rgb += texture(emissiveMap") != std::string_view::npos,
        "legacy additive emissive-map stage is absent from the shader");
    require(source.find("material.semantics.w > 0.5 && !gl_FrontFacing") != std::string_view::npos,
        "two-sided legacy lighting semantic is absent from the shader");

    auto shaderSet = RenderVsg::createLegacyCompatibilityShaderSet();
    require(static_cast<bool>(shaderSet), "failed to create legacy compatibility ShaderSet");
    const auto& materialBinding = shaderSet->getDescriptorBinding("material");
    require(static_cast<bool>(materialBinding), "legacy ShaderSet lost its material binding");
    require(dynamic_cast<const RenderVsg::LegacyMaterialUniformValue*>(materialBinding.data.get()) != nullptr,
        "legacy ShaderSet material binding does not use the OpenMW compatibility uniform");

    const vsg::ShaderStage* fragment = nullptr;
    for (const auto& stage : shaderSet->stages)
    {
        if (stage && stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
            fragment = stage.get();
    }
    require(fragment != nullptr && fragment->module,
        "legacy ShaderSet is missing its OpenMW fragment stage");
    require(fragment->module->source == source,
        "legacy ShaderSet does not compile the same fragment source covered by regression checks");

    vsg::ShaderCompiler compiler;
    require(compiler.supported(), "VSG was built without GLSL compiler support");

    auto plainHints = vsg::ShaderCompileSettings::create();
    auto plainStages = shaderSet->getShaderStages(plainHints);
    require(compiler.compile(plainStages), "plain legacy compatibility shader variant failed GLSL compilation");

    auto texturedHints = vsg::ShaderCompileSettings::create();
    texturedHints->defines = { "VSG_TEXTURECOORD_0", "VSG_DIFFUSE_MAP", "VSG_DETAIL_MAP", "VSG_EMISSIVE_MAP",
        "VSG_NORMAL_MAP", "VSG_SPECULAR_MAP" };
    auto texturedStages = shaderSet->getShaderStages(texturedHints);
    require(compiler.compile(texturedStages), "textured legacy compatibility shader variant failed GLSL compilation");

    RenderWorld world;
    const auto texture = world.reserveTexture();
    require(texture.has_value(), "failed to reserve legacy smoke texture");
    TextureRecord textureRecord;
    textureRecord.sourceIdentity = "cp3b3:legacy-smoke-texture";
    textureRecord.contentIdentity = "cp3b3:legacy-smoke-texture:v1";
    textureRecord.width = 2u;
    textureRecord.height = 2u;
    textureRecord.mipmapped = false;
    require(world.commit(*texture, std::move(textureRecord)), "failed to commit legacy smoke texture");

    const auto material = world.reserveMaterial();
    require(material.has_value(), "failed to reserve legacy smoke material");
    semanticMaterial.sourceIdentity = "cp3b3:legacy-smoke-material";
    TextureBinding diffuse;
    diffuse.texture = *texture;
    diffuse.role = TextureRole::Diffuse;
    diffuse.colorSpace = TextureColorSpace::Srgb;
    diffuse.formatClass = TextureFormatClass::Color;
    diffuse.sampler.mipmapMode = TextureMipmapMode::None;
    semanticMaterial.textures.push_back(diffuse);
    TextureBinding detail = diffuse;
    detail.role = TextureRole::Detail;
    semanticMaterial.textures.push_back(detail);
    require(world.commit(*material, std::move(semanticMaterial)), "failed to commit legacy smoke material");

    auto meshPayload = std::make_shared<MeshPayload>();
    meshPayload->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    meshPayload->normals.assign(3u, glm::vec3(0.0f, 0.0f, 1.0f));
    meshPayload->colors = { { 0.2f, 0.4f, 0.8f, 0.25f }, { 0.4f, 0.8f, 0.2f, 0.5f },
        { 0.8f, 0.2f, 0.4f, 0.75f } };
    meshPayload->texCoordSets.push_back({ { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.5f, 1.0f } });
    meshPayload->indices = { 0u, 1u, 2u };
    meshPayload->surfaces.push_back(MeshSurface{ PrimitiveTopology::Triangles, 0u, 3u, 0u });
    require(validMeshPayload(*meshPayload), "legacy smoke mesh payload is invalid");

    const auto mesh = world.reserveMesh();
    require(mesh.has_value(), "failed to reserve legacy smoke mesh");
    MeshRecord meshRecord;
    meshRecord.sourceIdentity = "cp3b3:legacy-smoke-mesh";
    meshRecord.surfaceCount = 1u;
    meshRecord.payload = meshPayload;
    require(world.commit(*mesh, std::move(meshRecord)), "failed to commit legacy smoke mesh");

    auto modelPayload = std::make_shared<ModelPayload>();
    ModelNodeRecord geometry;
    geometry.kind = ModelNodeKind::Geometry;
    geometry.mesh = *mesh;
    geometry.materials.push_back(*material);
    modelPayload->nodes.push_back(geometry);
    modelPayload->roots.push_back(ModelNodeIndex{ 0u });

    const auto model = world.reserveModel();
    require(model.has_value(), "failed to reserve legacy smoke model");
    ModelRecord modelRecord;
    modelRecord.sourceIdentity = "cp3b3:legacy-smoke-model";
    modelRecord.contentIdentity = "cp3b3:legacy-smoke-model:v1";
    modelRecord.payload = modelPayload;
    require(world.commit(*model, std::move(modelRecord)), "failed to commit legacy smoke model");

    const auto plan = RenderVsg::buildStaticAssetPlan(world, *model);
    require(plan.has_value() && plan->draws.size() == 1u, "failed to build legacy smoke static plan");

    std::uint32_t resolverCalls = 0u;
    RenderVsg::StaticTextureResolver resolver = [&](const TextureRecord&, const TextureRealizationKey& key) {
        ++resolverCalls;
        auto image = vsg::ubvec4Array2D::create(2u, 2u);
        image->set(0u, 0u, vsg::ubvec4(255u, 128u, 64u, 255u));
        image->set(1u, 0u, vsg::ubvec4(128u, 255u, 64u, 255u));
        image->set(0u, 1u, vsg::ubvec4(64u, 128u, 255u, 255u));
        image->set(1u, 1u, vsg::ubvec4(255u, 255u, 255u, 255u));
        image->properties.format = key.view.colorSpace == TextureColorSpace::Srgb
            ? VK_FORMAT_R8G8B8A8_SRGB
            : VK_FORMAT_R8G8B8A8_UNORM;
        image->properties.mipLevels = 1u;
        return vsg::ref_ptr<vsg::Data>(image);
    };

    RenderVsg::StaticAssetRealizer realizer;
    const auto realized = realizer.realize(world, *plan, resolver);
    require(realized.valid(), "legacy smoke realization failed");
    require(realized.stats.drawCount == 1u && realized.stats.legacyCompatibilityDraws == 1u
            && realized.stats.modernPbrDraws == 0u,
        "legacy smoke draw escaped the compatibility shader family");
    require(realized.stats.unsupportedTextureBindings == 0u,
        "supported diffuse/detail bindings were rejected");
    require(realized.stats.runtimeContextEffects == 0u,
        "supported vertex-alpha/alpha-test/blend/two-sided material unexpectedly failed closed");
    require(resolverCalls == 1u,
        "shared diffuse/detail texture realization should resolve one identical texture view");

    const auto* transform = dynamic_cast<const vsg::MatrixTransform*>(realized.root->children.front().get());
    require(transform != nullptr && transform->children.size() == 1u,
        "legacy smoke draw lost its MatrixTransform/StateGroup shape");
    const auto* stateGroup = dynamic_cast<const vsg::StateGroup*>(transform->children.front().get());
    require(stateGroup != nullptr, "legacy smoke draw is missing its StateGroup");

    const auto* raster = findPipelineState<vsg::RasterizationState>(*stateGroup);
    require(raster != nullptr && raster->cullMode == VK_CULL_MODE_NONE,
        "two-sided legacy material did not disable Vulkan culling");

    const auto* blend = findPipelineState<vsg::ColorBlendState>(*stateGroup);
    require(blend != nullptr && !blend->attachments.empty() && blend->attachments.front().blendEnable == VK_TRUE,
        "legacy alpha blending did not enable the Vulkan blend attachment");
    require(blend->attachments.front().srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA
            && blend->attachments.front().dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        "legacy source/destination blend factors changed");

    const auto* depth = findPipelineState<vsg::DepthStencilState>(*stateGroup);
    require(depth != nullptr && depth->depthCompareOp == VK_COMPARE_OP_GREATER_OR_EQUAL,
        "legacy material path regressed reverse-depth comparison");

    return 0;
}
