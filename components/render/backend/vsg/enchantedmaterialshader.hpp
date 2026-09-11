#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_ENCHANTEDMATERIALSHADER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_ENCHANTEDMATERIALSHADER_H

#include "legacymaterialshader.hpp"

#include <components/rendercore/records.hpp>

#include <vsg/core/Value.h>
#include <vsg/io/Options.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/ShaderModule.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderSet.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace RenderVsg
{
    inline constexpr std::size_t EnchantedEnvironmentFrameCount = 32u;
    inline constexpr std::uint32_t EnchantedEnvironmentTextureBinding = 14u;
    inline constexpr std::uint32_t EnchantedEnvironmentUniformBinding = 15u;

    struct alignas(16) EnchantedEnvironmentUniform
    {
        // xyz is the realized legacy envMapColor multiplied by the neutral
        // environment-map strength. Alpha is intentionally unused by V3.25's
        // environment contribution.
        vsg::vec4 colorStrength{ 1.0f, 1.0f, 1.0f, 1.0f };
    };

    static_assert(sizeof(EnchantedEnvironmentUniform) == sizeof(vsg::vec4));
    static_assert(alignof(EnchantedEnvironmentUniform) >= 16u);

    using EnchantedEnvironmentUniformValue = vsg::Value<EnchantedEnvironmentUniform>;

    namespace enchanted_material_shader_detail
    {
        [[nodiscard]] inline bool replaceOnce(
            std::string& source, std::string_view needle, std::string_view replacement)
        {
            const std::size_t offset = source.find(needle);
            if (offset == std::string::npos)
                return false;
            source.replace(offset, needle.size(), replacement);
            return true;
        }

        [[nodiscard]] inline bool insertAfterOnce(
            std::string& source, std::string_view needle, std::string_view insertion)
        {
            const std::size_t offset = source.find(needle);
            if (offset == std::string::npos)
                return false;
            source.insert(offset + needle.size(), insertion);
            return true;
        }

        [[nodiscard]] inline bool patchVertexSource(std::string& source)
        {
            if (!replaceOnce(source, "VSG_POINT_SPRITE)",
                    "VSG_POINT_SPRITE, OPENMW_ENCHANTED_ENVIRONMENT)"))
                return false;
            if (!insertAfterOnce(source, "layout(location = 4) out vec2 texCoord[VSG_TEXCOORD_COUNT];\n",
                    "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                    "layout(location = 8) out vec2 openmwEnvironmentUv;\n"
                    "#endif\n"))
                return false;
            return insertAfterOnce(source, "    normalDir = (mv * normal).xyz;\n",
                "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                "    // Match compatibility/objects.vert exactly for the legacy\n"
                "    // non-normal-mapped environment path: sphere reflection is\n"
                "    // calculated per vertex and interpolated for the fragment.\n"
                "    vec3 openmwViewNormal = normalize(normalDir);\n"
                "    vec3 openmwViewVector = normalize(eyePos);\n"
                "    vec3 openmwReflection = reflect(openmwViewVector, openmwViewNormal);\n"
                "    float openmwM = 2.0 * sqrt(openmwReflection.x * openmwReflection.x\n"
                "        + openmwReflection.y * openmwReflection.y\n"
                "        + (openmwReflection.z + 1.0) * (openmwReflection.z + 1.0));\n"
                "    openmwEnvironmentUv = vec2(openmwReflection.x / openmwM + 0.5,\n"
                "        openmwReflection.y / openmwM + 0.5);\n"
                "#endif\n");
        }

        [[nodiscard]] inline bool patchFragmentSource(std::string& source)
        {
            if (!replaceOnce(source, "SHADOWMAP_DEBUG)",
                    "SHADOWMAP_DEBUG, OPENMW_ENCHANTED_ENVIRONMENT)"))
                return false;
            if (!insertAfterOnce(source, "#ifdef VSG_SPECULAR_MAP\nlayout(set = MATERIAL_DESCRIPTOR_SET, binding = 5) uniform sampler2D specularMap;\n#endif\n",
                    "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                    "layout(set = MATERIAL_DESCRIPTOR_SET, binding = 14) uniform sampler2D openmwEnvironmentMaps[32];\n"
                    "layout(set = MATERIAL_DESCRIPTOR_SET, binding = 15) uniform OpenMwEnvironmentEffectData\n"
                    "{\n"
                    "    vec4 colorStrength;\n"
                    "} openmwEnvironmentEffect;\n"
                    "#endif\n"))
                return false;
            if (!insertAfterOnce(source, "    vec4 clipPlane;\n",
                    "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                    "    // x is the exact legacy 16 Hz caustic frame index,\n"
                    "    // computed from VSG's authoritative FrameStamp time.\n"
                    "    vec4 temporalEffects;\n"
                    "#endif\n"))
                return false;
            if (!insertAfterOnce(source, "layout(location = 4) in vec2 texCoord[VSG_TEXCOORD_COUNT];\n",
                    "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                    "layout(location = 8) in vec2 openmwEnvironmentUv;\n"
                    "#endif\n"))
                return false;
            return insertAfterOnce(source,
                "    outColor.rgb = color * ambientOcclusion + surfaceColor.rgb * effectiveEmission.rgb * emissiveMultiplier;\n",
                "#ifdef OPENMW_ENCHANTED_ENVIRONMENT\n"
                "    vec2 openmwEnvUv = openmwEnvironmentUv;\n"
                "#ifdef VSG_NORMAL_MAP\n"
                "    // Legacy normal mapping recomputes reflection coordinates\n"
                "    // per fragment from the perturbed normal.\n"
                "    vec3 openmwViewVector = normalize(eyePos);\n"
                "    vec3 openmwReflection = reflect(openmwViewVector, nd);\n"
                "    float openmwM = 2.0 * sqrt(openmwReflection.x * openmwReflection.x\n"
                "        + openmwReflection.y * openmwReflection.y\n"
                "        + (openmwReflection.z + 1.0) * (openmwReflection.z + 1.0));\n"
                "    openmwEnvUv = vec2(openmwReflection.x / openmwM + 0.5,\n"
                "        openmwReflection.y / openmwM + 0.5);\n"
                "#endif\n"
                "    int openmwGlowFrame = clamp(int(openmwEnvironment.temporalEffects.x + 0.5), 0, 31);\n"
                "    outColor.rgb += texture(openmwEnvironmentMaps[openmwGlowFrame], openmwEnvUv).rgb\n"
                "        * openmwEnvironmentEffect.colorStrength.rgb;\n"
                "#endif\n");
        }
    }

    [[nodiscard]] inline vsg::ref_ptr<EnchantedEnvironmentUniformValue> makeEnchantedEnvironmentMaterial(
        const RenderCore::MaterialRecord& source)
    {
        auto result = EnchantedEnvironmentUniformValue::create();
        const glm::vec4 color = source.environmentMapColor * source.environmentMapStrength;
        result->value().colorStrength = vsg::vec4(color.r, color.g, color.b, color.a);
        return result;
    }

    // Build a strict derivative of the already-validated OpenMW legacy shader.
    // Only the enchanted 32-frame environment path is added; ordinary legacy
    // draws continue using createLegacyCompatibilityShaderSet() unchanged.
    [[nodiscard]] inline vsg::ref_ptr<vsg::ShaderSet> createEnchantedLegacyCompatibilityShaderSet(
        vsg::ref_ptr<const vsg::Options> options = {})
    {
        auto result = createLegacyCompatibilityShaderSet(std::move(options));
        if (!result)
            return {};

        // Stock Phong owns the rest of material set 1. Keep the CP4F facet on
        // dedicated high bindings and fail rather than constructing duplicate
        // Vulkan descriptor bindings if the pinned contract ever expands there.
        for (const vsg::DescriptorBinding& binding : result->descriptorBindings)
        {
            if (binding.set == 1u
                && (binding.binding == EnchantedEnvironmentTextureBinding
                    || binding.binding == EnchantedEnvironmentUniformBinding))
                return {};
        }

        bool vertexPatched = false;
        bool fragmentPatched = false;
        for (vsg::ref_ptr<vsg::ShaderStage>& stage : result->stages)
        {
            if (!stage || !stage->module)
                continue;
            std::string source = stage->module->source;
            bool patched = false;
            if (stage->stage == VK_SHADER_STAGE_VERTEX_BIT)
            {
                patched = enchanted_material_shader_detail::patchVertexSource(source);
                vertexPatched = patched;
            }
            else if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
            {
                patched = enchanted_material_shader_detail::patchFragmentSource(source);
                fragmentPatched = patched;
            }
            else
                continue;
            if (!patched)
                return {};

            auto replacement = vsg::ShaderStage::create(*stage);
            replacement->module = vsg::ShaderModule::create(
                std::move(source), stage->module->hints);
            stage = std::move(replacement);
        }
        if (!vertexPatched || !fragmentPatched)
            return {};

        result->addDescriptorBinding("openmwEnvironmentMaps", "OPENMW_ENCHANTED_ENVIRONMENT", 1,
            EnchantedEnvironmentTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            static_cast<std::uint32_t>(EnchantedEnvironmentFrameCount), VK_SHADER_STAGE_FRAGMENT_BIT, {},
            vsg::CoordinateSpace::sRGB);
        result->addDescriptorBinding("openmwEnvironmentEffect", "OPENMW_ENCHANTED_ENVIRONMENT", 1,
            EnchantedEnvironmentUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
            EnchantedEnvironmentUniformValue::create());
        result->optionalDefines.insert("OPENMW_ENCHANTED_ENVIRONMENT");
        // The base compatibility builder deliberately has no inherited stock
        // Phong variants. Keep that invariant after replacing both stages.
        result->variants.clear();
        return result;
    }
}

#endif
