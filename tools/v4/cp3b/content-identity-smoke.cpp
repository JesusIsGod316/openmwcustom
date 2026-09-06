#include <components/nifrender/translationidentity.hpp>
#include <components/rendercore/realizationkeys.hpp>

#include <cassert>
#include <limits>
#include <unordered_map>

int main()
{
    using namespace NifRender;

    TranslationSourceIdentity firstSource;
    firstSource.sourceIdentity = "meshes/a/example.nif";
    firstSource.contentIdentity = "sha256:0123456789abcdef";
    assert(firstSource.valid());

    TranslationSourceIdentity aliasSource;
    aliasSource.sourceIdentity = "meshes/b/alias.nif";
    aliasSource.contentIdentity = firstSource.contentIdentity;
    assert(aliasSource.valid());
    assert(firstSource.sourceIdentity != aliasSource.sourceIdentity);

    const TranslationContentKey firstKey = makeTranslationContentKey(firstSource.contentIdentity, false);
    const TranslationContentKey aliasKey = makeTranslationContentKey(aliasSource.contentIdentity, false);
    assert(firstKey.valid());
    assert(firstKey == aliasKey);
    assert(stableTranslationKeyFingerprint(firstKey) == stableTranslationKeyFingerprint(aliasKey));

    const TranslationContentKey markerKey = makeTranslationContentKey(firstSource.contentIdentity, true);
    assert(markerKey.valid());
    assert(markerKey != firstKey);
    assert(stableTranslationKeyFingerprint(markerKey) != stableTranslationKeyFingerprint(firstKey));

    TranslationContentKey nextSchema = firstKey;
    ++nextSchema.semanticSchemaRevision;
    assert(nextSchema != firstKey);
    assert(stableTranslationKeyFingerprint(nextSchema) != stableTranslationKeyFingerprint(firstKey));

    std::unordered_map<TranslationContentKey, unsigned int, TranslationContentKeyHash> dedup;
    dedup.emplace(firstKey, 1u);
    dedup.emplace(aliasKey, 2u);
    dedup.emplace(markerKey, 3u);
    assert(dedup.size() == 2u);
    assert(dedup.at(firstKey) == 1u);
    assert(dedup.at(markerKey) == 3u);

    TranslationContentKey invalid;
    assert(!invalid.valid());

    using namespace RenderCore;

    MeshPayload mesh;
    mesh.positions.emplace_back(0.0f, 0.0f, 0.0f);
    mesh.normals.emplace_back(0.0f, 0.0f, 1.0f);
    mesh.texCoordSets.resize(2u);
    mesh.texCoordSets[0].emplace_back(0.0f, 0.0f);
    mesh.texCoordSets[1].emplace_back(1.0f, 1.0f);

    TextureBinding diffuse;
    diffuse.texture = TextureHandle::fromParts(3u, 1u);
    diffuse.role = TextureRole::Diffuse;
    diffuse.colorSpace = TextureColorSpace::Srgb;
    diffuse.formatClass = TextureFormatClass::Color;
    diffuse.transform.uvSet = 0u;
    diffuse.transform.offset = { 0.25f, 0.5f };
    diffuse.sampler.wrapU = TextureWrap::Repeat;
    diffuse.sampler.wrapV = TextureWrap::Clamp;

    TextureBinding normal;
    normal.texture = TextureHandle::fromParts(4u, 1u);
    normal.role = TextureRole::Normal;
    normal.colorSpace = TextureColorSpace::Data;
    normal.formatClass = TextureFormatClass::Normal;
    normal.transform.uvSet = 1u;
    normal.sampler.wrapU = TextureWrap::Clamp;
    normal.sampler.wrapV = TextureWrap::Clamp;

    MaterialRecord material;
    material.sourceIdentity = "meshes/a/example.nif#material:7";
    material.diffuse = { 0.2f, 0.3f, 0.4f, 1.0f };
    material.alphaCutoff = 0.25f;
    material.textures = { diffuse, normal };

    const MaterialRealizationKey materialKey = makeMaterialRealizationKey(material);
    const GraphicsPipelineKey pipelineKey
        = makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, material);

    // Source provenance, uniform values, logical image identity, sampler state,
    // and numeric texture transforms must not fragment material/pipeline caches.
    MaterialRecord aliasMaterial = material;
    aliasMaterial.sourceIdentity = "meshes/b/alias.nif#material:2";
    aliasMaterial.diffuse = { 0.9f, 0.1f, 0.7f, 0.5f };
    aliasMaterial.alphaCutoff = 0.9f;
    aliasMaterial.refractionStrength = 13.0f;
    aliasMaterial.softEffectDepth = 42.0f;
    aliasMaterial.falloffParams = { 8.0f, 7.0f, 6.0f, 5.0f };
    aliasMaterial.textures[0].texture = TextureHandle::fromParts(90u, 7u);
    aliasMaterial.textures[0].transform.offset = { 8.0f, 9.0f };
    aliasMaterial.textures[0].transform.scale = { 3.0f, 4.0f };
    aliasMaterial.textures[0].sampler.wrapU = TextureWrap::Mirror;
    aliasMaterial.textures[0].sampler.maxAnisotropy = 8.0f;
    assert(makeMaterialRealizationKey(aliasMaterial) == materialKey);
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, aliasMaterial) == pipelineKey);
    assert(stableMaterialRealizationFingerprint(makeMaterialRealizationKey(aliasMaterial))
        == stableMaterialRealizationFingerprint(materialKey));
    assert(stableGraphicsPipelineFingerprint(
               makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, aliasMaterial))
        == stableGraphicsPipelineFingerprint(pipelineKey));

    // Texture descriptor layout is canonicalized, so source ordering alone does
    // not create a new shader or graphics pipeline.
    MaterialRecord reordered = material;
    std::swap(reordered.textures[0], reordered.textures[1]);
    assert(makeMaterialRealizationKey(reordered) == materialKey);
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, reordered) == pipelineKey);

    MaterialRecord uvVariant = material;
    uvVariant.textures[0].transform.uvSet = 1u;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, uvVariant) != pipelineKey);

    MaterialRecord interpretationVariant = material;
    interpretationVariant.textures[0].colorSpace = TextureColorSpace::Data;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, interpretationVariant) != pipelineKey);

    MaterialRecord blendVariant = material;
    blendVariant.alphaBlendEnabled = true;
    blendVariant.sourceBlend = BlendFactor::SourceAlpha;
    blendVariant.destinationBlend = BlendFactor::OneMinusSourceAlpha;
    blendVariant.transparentSort = TransparentSortPolicy::Sorted;
    assert(makeMaterialRealizationKey(blendVariant) != materialKey);
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, blendVariant) != pipelineKey);

    MaterialRecord sortOnlyVariant = material;
    sortOnlyVariant.transparentSort = TransparentSortPolicy::Unsorted;
    assert(makeMaterialRealizationKey(sortOnlyVariant) != materialKey);
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, sortOnlyVariant) == pipelineKey);

    MaterialRecord disabledBlendNoise = material;
    disabledBlendNoise.sourceBlend = BlendFactor::DestinationColor;
    disabledBlendNoise.destinationBlend = BlendFactor::SourceAlphaSaturate;
    disabledBlendNoise.blendEquation = BlendEquation::Maximum;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, disabledBlendNoise) == pipelineKey);

    MaterialRecord alphaTestVariant = material;
    alphaTestVariant.alphaTestEnabled = true;
    alphaTestVariant.alphaCompare = CompareOp::GreaterEqual;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, alphaTestVariant) != pipelineKey);

    MaterialRecord decalVariant = material;
    decalVariant.decal = true;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, decalVariant) != pipelineKey);

    MaterialRecord stencilVariant = material;
    stencilVariant.stencil.enabled = true;
    stencilVariant.stencil.compare = CompareOp::Equal;
    stencilVariant.stencil.reference = 3u;
    stencilVariant.stencil.compareMask = 0xffu;
    stencilVariant.stencil.pass = StencilOp::Replace;
    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, stencilVariant) != pipelineKey);

    assert(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Lines, material) != pipelineKey);
    MeshPayload colorMesh = mesh;
    colorMesh.colors.emplace_back(1.0f, 1.0f, 1.0f, 1.0f);
    assert(makeGraphicsPipelineKey(colorMesh, PrimitiveTopology::Triangles, material) != pipelineKey);

    const TextureViewKey srgbView = makeTextureViewKey(diffuse);
    TextureBinding dataViewBinding = diffuse;
    dataViewBinding.colorSpace = TextureColorSpace::Data;
    const TextureViewKey dataView = makeTextureViewKey(dataViewBinding);
    assert(srgbView.valid());
    assert(dataView.valid());
    assert(srgbView != dataView);

    const auto diffuseSampler = makeSamplerRealizationKey(diffuse.sampler);
    assert(diffuseSampler);
    TextureBinding sameSamplerOtherTexture = diffuse;
    sameSamplerOtherTexture.texture = TextureHandle::fromParts(99u, 4u);
    const auto sharedSampler = makeSamplerRealizationKey(sameSamplerOtherTexture.sampler);
    assert(sharedSampler && *sharedSampler == *diffuseSampler);

    SamplerSemantic differentSampler = diffuse.sampler;
    differentSampler.wrapU = TextureWrap::Clamp;
    const auto differentSamplerKey = makeSamplerRealizationKey(differentSampler);
    assert(differentSamplerKey && *differentSamplerKey != *diffuseSampler);

    SamplerSemantic negativeZeroSampler = diffuse.sampler;
    negativeZeroSampler.maxAnisotropy = -0.0f;
    const auto negativeZeroKey = makeSamplerRealizationKey(negativeZeroSampler);
    SamplerSemantic positiveZeroSampler = diffuse.sampler;
    positiveZeroSampler.maxAnisotropy = 0.0f;
    const auto positiveZeroKey = makeSamplerRealizationKey(positiveZeroSampler);
    assert(negativeZeroKey && positiveZeroKey && *negativeZeroKey == *positiveZeroKey);

    SamplerSemantic invalidSampler = diffuse.sampler;
    invalidSampler.maxAnisotropy = std::numeric_limits<float>::quiet_NaN();
    assert(!makeSamplerRealizationKey(invalidSampler));

    std::unordered_map<GraphicsPipelineKey, unsigned int, GraphicsPipelineKeyHash> pipelines;
    pipelines.emplace(pipelineKey, 1u);
    pipelines.emplace(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Triangles, aliasMaterial), 2u);
    pipelines.emplace(makeGraphicsPipelineKey(mesh, PrimitiveTopology::Lines, material), 3u);
    assert(pipelines.size() == 2u);
    assert(pipelines.at(pipelineKey) == 1u);

    return 0;
}
