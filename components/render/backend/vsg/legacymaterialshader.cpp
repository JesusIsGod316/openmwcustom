#include "legacymaterialshader.hpp"

#include "locallightbuffer.hpp"

#include <vsg/state/ShaderModule.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderSet.h>

#include <algorithm>
#include <string>
#include <utility>

namespace RenderVsg
{
    namespace
    {
        constexpr std::string_view LegacyCompatibilityFragmentShader = R"glsl(#version 450
#extension GL_ARB_separate_shader_objects : enable
#pragma import_defines (VSG_TEXTURECOORD_0, VSG_TEXTURECOORD_1, VSG_TEXTURECOORD_2, VSG_TEXTURECOORD_3, VSG_POINT_SPRITE, VSG_DIFFUSE_MAP, VSG_GREYSCALE_DIFFUSE_MAP, VSG_DETAIL_MAP, VSG_EMISSIVE_MAP, VSG_LIGHTMAP_MAP, VSG_NORMAL_MAP, VSG_SPECULAR_MAP, SHADOWMAP_DEBUG)

#if defined(VSG_SHADOWS_PCSS) || defined(VSG_SHADOWS_SOFT)
#error OpenMW legacy compatibility shader currently supports VSG hard shadows only; never silently downgrade a requested shadow mode
#endif

#ifndef VSG_SHADOWS_HARD
#define VSG_SHADOWS_HARD
#endif

#if defined(VSG_TEXTURECOORD_3)
#define VSG_TEXCOORD_COUNT 4
#elif defined(VSG_TEXTURECOORD_2)
#define VSG_TEXCOORD_COUNT 3
#elif defined(VSG_TEXTURECOORD_1)
#define VSG_TEXCOORD_COUNT 2
#else
#define VSG_TEXCOORD_COUNT 1
#endif

#define VIEW_DESCRIPTOR_SET 0
#define MATERIAL_DESCRIPTOR_SET 1

#ifdef VSG_DIFFUSE_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 0) uniform sampler2D diffuseMap;
#endif
#ifdef VSG_DETAIL_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 1) uniform sampler2D detailMap;
#endif
#ifdef VSG_NORMAL_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 2) uniform sampler2D normalMap;
#endif
#ifdef VSG_LIGHTMAP_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 3) uniform sampler2D aoMap;
#endif
#ifdef VSG_EMISSIVE_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 4) uniform sampler2D emissiveMap;
#endif
#ifdef VSG_SPECULAR_MAP
layout(set = MATERIAL_DESCRIPTOR_SET, binding = 5) uniform sampler2D specularMap;
#endif

layout(set = MATERIAL_DESCRIPTOR_SET, binding = 10) uniform LegacyMaterialData
{
    vec4 ambientColor;
    vec4 diffuseColor;
    vec4 specularColor;
    vec4 emissiveColor;
    vec4 parameters;
    vec4 semantics;
    vec4 fogColor;
    vec4 effects;
    vec4 ambientOverride;
} material;

layout(set = MATERIAL_DESCRIPTOR_SET, binding = 11) uniform TexCoordIndices
{
    int diffuseMap;
    int detailMap;
    int normalMap;
    int aoMap;
    int emissiveMap;
    int specularMap;
    int mrMap;
} texCoordIndices;

layout(constant_id = 3) const int lightDataSize = 256;
layout(set = VIEW_DESCRIPTOR_SET, binding = 0) uniform LightData
{
    vec4 values[lightDataSize];
} lightData;
layout(set = VIEW_DESCRIPTOR_SET, binding = 2) uniform texture2DArray shadowMaps;
layout(set = VIEW_DESCRIPTOR_SET, binding = 4) uniform sampler shadowMapShadowSampler;

layout(std430, set = VIEW_DESCRIPTOR_SET, binding = 5) readonly buffer OpenMwLocalLightData
{
    // header: x=count, y=radius-fade enabled, z=vec4 stride.
    vec4 header;
    // Per light: position/radius, diffuse, specular, ambient,
    // constant/linear/quadratic attenuation plus enabled actor fade.
    vec4 values[];
} openmwLocalLights;

layout(set = VIEW_DESCRIPTOR_SET, binding = 6) uniform OpenMwEnvironmentData
{
    vec4 fogColor;
    // x=start, y=end, z=enabled, w=FogDistanceMode.
    vec4 fogRangeModes;
    // x=near, y=far, z=FogFalloffMode.
    vec4 projectionFog;
    vec4 skyColor;
    // x=night factor, y=cloud blend, z=cloud speed, w=precipitation intensity.
    vec4 weatherFactors;
    // xyz=wind direction, w=wind speed.
    vec4 wind;
    // x=precipitation enabled, y=storm, z=sky enabled, w=shadows enabled.
    vec4 weatherFlags;
    // Eye-space retained half-space; a zero normal disables clipping.
    vec4 clipPlane;
} openmwEnvironment;

layout(location = 0) in vec3 eyePos;
layout(location = 1) in vec3 normalDir;
layout(location = 2) in vec4 vertexColor;
layout(location = 3) in vec3 viewDir;
layout(location = 4) in vec2 texCoord[VSG_TEXCOORD_COUNT];
layout(location = 0) out vec4 outColor;

float calculateShadowCoverageForDirectionalLightHard(int lightDataIndex, int shadowMapIndex, inout vec3 color)
{
    vec4 shadowMapSettings = lightData.values[lightDataIndex++];
    int shadowMapCount = int(shadowMapSettings.r);
    while (shadowMapCount > 0)
    {
        mat4 smMatrix = mat4(lightData.values[lightDataIndex], lightData.values[lightDataIndex + 1],
            lightData.values[lightDataIndex + 2], lightData.values[lightDataIndex + 3]);
        vec4 smTc = smMatrix * vec4(eyePos, 1.0);
        smTc = vec4(smTc.xyz / smTc.w, 1.0);
        if (smTc.x >= 0.0 && smTc.x <= 1.0 && smTc.y >= 0.0 && smTc.y <= 1.0 && smTc.z >= 0.0
            && smTc.z <= 1.0)
        {
            float coverage = texture(sampler2DArrayShadow(shadowMaps, shadowMapShadowSampler),
                vec4(smTc.st, shadowMapIndex, smTc.z)).r;
#ifdef SHADOWMAP_DEBUG
            if (shadowMapIndex == 0) color = vec3(1.0, 0.0, 0.0);
            else if (shadowMapIndex == 1) color = vec3(0.0, 1.0, 0.0);
            else if (shadowMapIndex == 2) color = vec3(0.0, 0.0, 1.0);
            else if (shadowMapIndex == 3) color = vec3(1.0, 1.0, 0.0);
            else if (shadowMapIndex == 4) color = vec3(0.0, 1.0, 1.0);
            else color = vec3(1.0);
#endif
            return coverage;
        }
        lightDataIndex += 8;
        ++shadowMapIndex;
        --shadowMapCount;
    }
    return 0.0;
}

float calculateShadowCoverageForSpotLightHard(int lightDataIndex, int shadowMapIndex, inout vec3 color)
{
    return calculateShadowCoverageForDirectionalLightHard(lightDataIndex, shadowMapIndex, color);
}

float calculateShadowCoverageForDirectionalLight(int lightDataIndex, int shadowMapIndex, vec3 T, vec3 B, inout vec3 color)
{
    return calculateShadowCoverageForDirectionalLightHard(lightDataIndex, shadowMapIndex, color);
}

float calculateShadowCoverageForSpotLight(
    int lightDataIndex, int shadowMapIndex, vec3 T, vec3 B, float lightDist, inout vec3 color)
{
    return calculateShadowCoverageForSpotLightHard(lightDataIndex, shadowMapIndex, color);
}

bool alphaComparisonPass(float value, float reference, int compareOp)
{
    switch (compareOp)
    {
        case 0: return false;
        case 1: return value < reference;
        case 2: return value == reference;
        case 3: return value <= reference;
        case 4: return value > reference;
        case 5: return value != reference;
        case 6: return value >= reference;
        case 7: return true;
    }
    return false;
}

vec3 getNormal()
{
    vec3 result;
#ifdef VSG_NORMAL_MAP
    vec3 tangentNormal = texture(normalMap, texCoord[texCoordIndices.normalMap]).xyz * 2.0 - 1.0;
    vec3 q1 = dFdx(eyePos);
    vec3 q2 = dFdy(eyePos);
    vec2 st1 = dFdx(texCoord[texCoordIndices.normalMap]);
    vec2 st2 = dFdy(texCoord[texCoordIndices.normalMap]);
    vec3 N = normalize(normalDir);
    vec3 T = normalize(q1 * st2.t - q2 * st1.t);
    vec3 B = -normalize(cross(N, T));
    result = normalize(mat3(T, B, N) * tangentNormal);
#else
    result = normalize(normalDir);
#endif
    if (material.semantics.w > 0.5 && !gl_FrontFacing)
        result = -result;
    return result;
}
)glsl"
        // Keep each token below MSVC's 16,380-byte string-literal limit. Adjacent
        // literals are concatenated without changing the shader source.
        R"glsl(
void main()
{
    const int vertexColorMode = int(material.semantics.x + 0.5);
    vec4 effectiveDiffuse = material.diffuseColor;
    vec4 effectiveAmbient = material.ambientColor;
    vec4 effectiveEmission = material.emissiveColor;

    // Matches SceneUtil::get*Color semantics: AmbientDiffuse replaces both
    // ambient and diffuse with the authored vertex RGBA; Emissive replaces only
    // emission. In particular AmbientDiffuse vertex alpha replaces material
    // diffuse alpha rather than being multiplied by it.
    if (vertexColorMode == 2)
    {
        effectiveDiffuse = vertexColor;
        effectiveAmbient = vertexColor;
    }
    else if (vertexColorMode == 1)
        effectiveEmission = vertexColor;

vec2 diffuseUv = vec2(0.0);
#ifdef VSG_POINT_SPRITE
    diffuseUv = gl_PointCoord.xy;
#elif defined(VSG_DIFFUSE_MAP)
    diffuseUv = texCoord[texCoordIndices.diffuseMap].st;
#endif

    if (dot(openmwEnvironment.clipPlane.xyz, openmwEnvironment.clipPlane.xyz) > 0.25
        && dot(openmwEnvironment.clipPlane, vec4(eyePos, 1.0)) < 0.0)
        discard;

    vec4 surfaceColor = vec4(1.0);
#ifdef VSG_DIFFUSE_MAP
#ifdef VSG_GREYSCALE_DIFFUSE_MAP
    float diffuseValue = texture(diffuseMap, diffuseUv).s;
    surfaceColor *= vec4(diffuseValue, diffuseValue, diffuseValue, 1.0);
#else
    surfaceColor *= texture(diffuseMap, diffuseUv);
#endif
#endif

    surfaceColor.a *= effectiveDiffuse.a;
    if (material.semantics.y > 0.5
        && !alphaComparisonPass(surfaceColor.a, material.parameters.y, int(material.semantics.z + 0.5)))
        discard;

#ifdef VSG_DETAIL_MAP
    // OpenMW/V3.25 legacy detail stage: RGB modulation around neutral 0.5.
    // Stock VSG Phong alpha-mixes the detail sample, which is not equivalent.
    surfaceColor.rgb *= texture(detailMap, texCoord[texCoordIndices.detailMap].st).rgb * 2.0;
#endif

    vec3 specularColor = material.specularColor.rgb;
    float shininess = max(material.parameters.x, 0.0);
    float specularStrength = material.parameters.z;
    float emissiveMultiplier = material.parameters.w;
#ifdef VSG_SPECULAR_MAP
    // V3.25 specular maps replace material specular RGB and source shininess,
    // but the independent material specular-strength multiplier still applies.
    vec4 specularSample = texture(specularMap, texCoord[texCoordIndices.specularMap].st);
    specularColor = specularSample.rgb;
    shininess = specularSample.a * 255.0;
#endif

    float ambientOcclusion = 1.0;
#ifdef VSG_LIGHTMAP_MAP
    ambientOcclusion *= texture(aoMap, texCoord[texCoordIndices.aoMap].st).r;
#endif

    vec3 nd = getNormal();
    vec3 vd = normalize(viewDir);
    vec3 color = vec3(0.0);
    const float intensityMinimum = 0.001;
    const bool materialUnlit = material.effects.w > 0.5;

    vec4 lightNums = materialUnlit ? vec4(0.0) : lightData.values[0];
    int numAmbientLights = int(lightNums[0]);
    int numDirectionalLights = int(lightNums[1]);
    int numPointLights = int(lightNums[2]);
    int numSpotLights = int(lightNums[3]);
    int lightDataIndex = 1;

    for (int i = 0; i < numAmbientLights; ++i)
    {
        vec4 lightColor = lightData.values[lightDataIndex++];
        vec3 ambientLightColor = material.ambientOverride.w > 0.5
            ? material.ambientOverride.rgb
            : lightColor.rgb;
        color += surfaceColor.rgb * effectiveAmbient.rgb * ambientLightColor * lightColor.a;
    }

    int shadowMapIndex = 0;
    for (int i = 0; i < numDirectionalLights; ++i)
    {
        vec4 lightColor = lightData.values[lightDataIndex++];
        vec3 direction = -lightData.values[lightDataIndex++].xyz;
        float intensity = lightColor.a;
        float unclampedLdotN = dot(direction, nd);
        float diffuseFactor = max(unclampedLdotN, 0.0);
        intensity *= diffuseFactor;

        int shadowMapCount = int(lightData.values[lightDataIndex].r);
        if (shadowMapCount > 0)
        {
            if (intensity > intensityMinimum)
            {
                vec3 q1 = dFdx(eyePos);
                vec3 q2 = dFdy(eyePos);
                vec2 st1 = dFdx(texCoord[0]);
                vec2 st2 = dFdy(texCoord[0]);
                vec3 N = normalize(normalDir);
                vec3 T = normalize(q1 * st2.t - q2 * st1.t);
                vec3 B = -normalize(cross(N, T));
                intensity *= 1.0 - calculateShadowCoverageForDirectionalLight(
                    lightDataIndex, shadowMapIndex, T, B, color);
            }
            lightDataIndex += 1 + 8 * shadowMapCount;
            shadowMapIndex += shadowMapCount;
        }
        else
            ++lightDataIndex;

        if (intensity < intensityMinimum)
            continue;
        color += surfaceColor.rgb * effectiveDiffuse.rgb * lightColor.rgb * intensity;
        if (shininess > 0.0 && diffuseFactor > 0.0)
        {
            vec3 halfDir = normalize(direction + vd);
            color += specularColor * specularStrength * pow(max(dot(halfDir, nd), 0.0), shininess) * intensity;
        }
    }

    for (int i = 0; i < numPointLights; ++i)
    {
        vec4 lightColor = lightData.values[lightDataIndex++];
        vec3 position = lightData.values[lightDataIndex++].xyz;
        float intensity = lightColor.a;
        if (intensity < intensityMinimum)
            continue;
        vec3 delta = position - eyePos;
        float distance2 = dot(delta, delta);
        vec3 direction = delta / sqrt(distance2);
        float scale = intensity / distance2;
        float diffuseFactor = scale * max(dot(direction, nd), 0.0);
        color += surfaceColor.rgb * effectiveDiffuse.rgb * lightColor.rgb * diffuseFactor;
        if (shininess > 0.0 && diffuseFactor > 0.0)
        {
            vec3 halfDir = normalize(direction + vd);
            color += specularColor * specularStrength * pow(max(dot(halfDir, nd), 0.0), shininess) * scale;
        }
    }

    int openmwPointLightCount = materialUnlit
        ? 0
        : min(int(openmwLocalLights.header.x), openmwLocalLights.values.length() / 5);
    for (int i = 0; i < openmwPointLightCount; ++i)
    {
        int base = i * 5;
        vec4 positionRadius = openmwLocalLights.values[base];
        vec4 diffuse = openmwLocalLights.values[base + 1];
        vec4 specular = openmwLocalLights.values[base + 2];
        vec4 ambient = openmwLocalLights.values[base + 3];
        vec4 attenuationFade = openmwLocalLights.values[base + 4];
        if (attenuationFade.w < intensityMinimum)
            continue;

        vec3 delta = positionRadius.xyz - eyePos;
        float lightDistance = length(delta);
        if (openmwLocalLights.header.y > 0.5)
        {
            if (positionRadius.w <= 0.0 || lightDistance > positionRadius.w)
                continue;
        }
        vec3 direction = lightDistance > 0.0 ? delta / lightDistance : nd;
        float denominator = attenuationFade.x + attenuationFade.y * lightDistance
            + attenuationFade.z * lightDistance * lightDistance;
        if (denominator <= 0.0)
            continue;
        float scale = attenuationFade.w / denominator;
        if (openmwLocalLights.header.y > 0.5)
        {
            float radiusFade = clamp((lightDistance / positionRadius.w - 0.75) / 0.25, 0.0, 1.0);
            radiusFade = 1.0 - radiusFade * radiusFade;
            radiusFade = 1.0 - radiusFade * radiusFade;
            scale *= 1.0 - radiusFade;
        }
        float diffuseFactor = scale * max(dot(direction, nd), 0.0);
        color += surfaceColor.rgb * effectiveDiffuse.rgb * diffuse.rgb * diffuseFactor;
        color += surfaceColor.rgb * effectiveAmbient.rgb * ambient.rgb * scale;
        if (shininess > 0.0 && diffuseFactor > 0.0)
        {
            vec3 halfDir = normalize(direction + vd);
            color += specularColor * specularStrength * specular.rgb
                * pow(max(dot(halfDir, nd), 0.0), shininess) * scale;
        }
    }

    for (int i = 0; i < numSpotLights; ++i)
    {
        vec4 lightColor = lightData.values[lightDataIndex++];
        vec4 positionCosInnerAngle = lightData.values[lightDataIndex++];
        vec4 lightDirectionCosOuterAngle = lightData.values[lightDataIndex++];
        float intensity = lightColor.a;
        vec3 delta = positionCosInnerAngle.xyz - eyePos;
        float distance2 = dot(delta, delta);
        float lightDistance = sqrt(distance2);
        vec3 direction = delta / lightDistance;
        float dotLightDirection = dot(lightDirectionCosOuterAngle.xyz, -direction);

        int shadowMapCount = int(lightData.values[lightDataIndex].r);
        if (shadowMapCount > 0)
        {
            if (lightDirectionCosOuterAngle.w < dotLightDirection)
            {
                vec3 q1 = dFdx(eyePos);
                vec3 q2 = dFdy(eyePos);
                vec2 st1 = dFdx(texCoord[0]);
                vec2 st2 = dFdy(texCoord[0]);
                vec3 N = normalize(normalDir);
                vec3 T = normalize(q1 * st2.t - q2 * st1.t);
                vec3 B = -normalize(cross(N, T));
                intensity *= 1.0 - calculateShadowCoverageForSpotLight(
                    lightDataIndex, shadowMapIndex, T, B, lightDistance, color);
            }
            lightDataIndex += 1 + 8 * shadowMapCount;
            shadowMapIndex += shadowMapCount;
        }
        else
            ++lightDataIndex;

        if (intensity < intensityMinimum)
            continue;
        float scale = intensity
            * smoothstep(lightDirectionCosOuterAngle.w, positionCosInnerAngle.w, dotLightDirection) / distance2;
        float diffuseFactor = scale * max(dot(direction, nd), 0.0);
        color += surfaceColor.rgb * effectiveDiffuse.rgb * lightColor.rgb * diffuseFactor;
        if (shininess > 0.0 && diffuseFactor > 0.0)
        {
            vec3 halfDir = normalize(direction + vd);
            color += specularColor * specularStrength * pow(max(dot(halfDir, nd), 0.0), shininess) * scale;
        }
    }

    // Material/vertex emission is part of the legacy textured lighting equation
    // and retains the authored emissive multiplier. Glow/emissive-map RGB is a
    // separate additive stage in V3.25.
    if (materialUnlit)
    {
        vec3 unlitColor = vertexColorMode == 1
            ? effectiveEmission.rgb * emissiveMultiplier
            : effectiveDiffuse.rgb;
        outColor.rgb = surfaceColor.rgb * unlitColor;
    }
    else
        outColor.rgb = color * ambientOcclusion + surfaceColor.rgb * effectiveEmission.rgb * emissiveMultiplier;
#ifdef VSG_EMISSIVE_MAP
    outColor.rgb += texture(emissiveMap, texCoord[texCoordIndices.emissiveMap].st).rgb;
#endif

    bool fogEnabled = openmwEnvironment.fogRangeModes.z > 0.5;
    vec3 fogColor = openmwEnvironment.fogColor.rgb;
    float fogStart = openmwEnvironment.fogRangeModes.x;
    float fogEnd = openmwEnvironment.fogRangeModes.y;
    const int materialFogMode = int(material.effects.x + 0.5);
    if (materialFogMode == 1)
        fogEnabled = false;
    else if (materialFogMode == 2)
    {
        fogColor = material.fogColor.rgb;
        if (material.effects.y >= 0.0)
        {
            fogStart = openmwEnvironment.projectionFog.x * material.effects.y
                + openmwEnvironment.projectionFog.y * (1.0 - material.effects.y);
            fogEnd = openmwEnvironment.projectionFog.y;
            fogEnabled = true;
        }
    }
    if (fogEnabled)
    {
        float fogDistance = openmwEnvironment.fogRangeModes.w > 0.5 ? length(eyePos) : abs(eyePos.z);
        float fogValue;
        if (openmwEnvironment.projectionFog.z > 0.5)
            fogValue = 1.0 - exp(-2.0 * max(0.0, fogDistance - fogStart / 2.0)
                / (fogEnd - fogStart / 2.0));
        else
            fogValue = clamp((fogDistance - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
        if (material.effects.z > 0.5)
            outColor.rgb *= 1.0 - fogValue;
        else
            outColor.rgb = mix(outColor.rgb, fogColor, fogValue);
    }
    outColor.a = surfaceColor.a;
}
)glsl";

        [[nodiscard]] vsg::vec4 toVsg(const glm::vec4& value) noexcept
        {
            return { value.x, value.y, value.z, value.w };
        }
    }

    vsg::ref_ptr<LegacyMaterialUniformValue> makeLegacyCompatibilityMaterial(
        const RenderCore::MaterialRecord& source)
    {
        auto result = LegacyMaterialUniformValue::create();
        LegacyMaterialUniform& uniform = result->value();

        uniform.ambientColor = toVsg(source.ambient);

        glm::vec4 diffuse = source.diffuse;
        diffuse.a = source.alpha;
        uniform.diffuseColor = toVsg(diffuse);

        uniform.specularColor = toVsg(source.specular);
        uniform.emissiveColor = toVsg(source.emission);
        uniform.parameters = vsg::vec4(std::max(0.0f, source.shininess), source.alphaCutoff, source.specularStrength,
            source.emissiveMultiplier);
        uniform.semantics = vsg::vec4(static_cast<float>(source.vertexColorMode),
            source.alphaTestEnabled ? 1.0f : 0.0f, static_cast<float>(source.alphaCompare),
            source.cullMode == RenderCore::CullMode::None ? 1.0f : 0.0f);
        uniform.fogColor = toVsg(source.fog.color);
        const bool additiveFog = source.alphaBlendEnabled
            && source.sourceBlend == RenderCore::BlendFactor::SourceAlpha
            && source.destinationBlend == RenderCore::BlendFactor::One;
        uniform.effects = vsg::vec4(static_cast<float>(source.fog.mode), source.fog.depth,
            additiveFog ? 1.0f : 0.0f, source.unlit ? 1.0f : 0.0f);
        uniform.ambientOverride = vsg::vec4(source.ambientLightOverride.r, source.ambientLightOverride.g,
            source.ambientLightOverride.b, source.ambientLightOverrideEnabled ? 1.0f : 0.0f);
        return result;
    }

    vsg::ref_ptr<vsg::ShaderSet> createLegacyCompatibilityShaderSet(vsg::ref_ptr<const vsg::Options> options)
    {
        auto base = vsg::createPhongShaderSet(options);
        if (!base)
            return {};

        auto result = vsg::ShaderSet::create();
        result->stages = base->stages;
        result->attributeBindings = base->attributeBindings;
        result->descriptorBindings = base->descriptorBindings;
        result->pushConstantRanges = base->pushConstantRanges;
        result->definesArrayStates = base->definesArrayStates;
        result->optionalDefines = base->optionalDefines;
        result->defaultGraphicsPipelineStates = base->defaultGraphicsPipelineStates;
        result->customDescriptorSetBindings = base->customDescriptorSetBindings;
        result->defaultShaderHints = base->defaultShaderHints;

        // Until explicitly implemented, non-hard shadow variants must not become
        // selectable through this family and silently render as something else.
        result->optionalDefines.erase("VSG_SHADOWS_PCSS");
        result->optionalDefines.erase("VSG_SHADOWS_SOFT");
        result->optionalDefines.erase("VSG_ALPHA_TEST");
        result->optionalDefines.erase("VSG_TWO_SIDED_LIGHTING");

        bool replacedFragment = false;
        for (vsg::ref_ptr<vsg::ShaderStage>& stage : result->stages)
        {
            if (!stage || stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                continue;

            auto replacement = vsg::ShaderStage::create(*stage);
            replacement->module = vsg::ShaderModule::create(std::string(LegacyCompatibilityFragmentShader),
                stage->module ? stage->module->hints : vsg::ref_ptr<vsg::ShaderCompileSettings>{});
            stage = std::move(replacement);
            replacedFragment = true;
            break;
        }
        if (!replacedFragment)
            return {};

        bool replacedMaterial = false;
        for (vsg::DescriptorBinding& binding : result->descriptorBindings)
        {
            if (binding.name != "material")
                continue;
            binding.data = LegacyMaterialUniformValue::create();
            replacedMaterial = true;
            break;
        }
        if (!replacedMaterial)
            return {};

        // Deliberately do not copy base->variants. They contain stock Phong
        // fragment stages and would bypass the OpenMW compatibility source.
        return result;
    }

    std::string_view legacyCompatibilityFragmentShaderSource() noexcept
    {
        return LegacyCompatibilityFragmentShader;
    }
}
