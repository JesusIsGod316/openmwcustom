#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYBUMPMATERIALSHADER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYBUMPMATERIALSHADER_H

#include "legacymaterialshader.hpp"

#include <components/rendercore/records.hpp>
#include <cstddef>
#include <type_traits>

#include <vsg/core/Value.h>
#include <vsg/io/Options.h>
#include <vsg/maths/vec4.h>
#include <vsg/state/ShaderModule.h>
#include <vsg/state/ShaderStage.h>
#include <vsg/utils/ShaderSet.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace RenderVsg
{
    // 12/13 belong to decal/gloss, 14/15 to environment, and 16 to LAND blend.
    inline constexpr std::uint32_t LegacyBumpTextureBinding = 17u;
    inline constexpr std::uint32_t LegacyBumpUniformBinding = 18u;

    // Exact backend representation of NiTexturingProperty's legacy bump facet.
    // matrix is the authored 2x2 value in NIF/OSG scalar order. lumaUv stores
    // x=luma scale, y=luma bias, z=texture-coordinate set, w=reserved.
    // std140 constrains byte offsets in the uploaded buffer, not host object alignment.
    // Pinned VSG Object::operator new does not promise over-aligned allocations.
    // Keep ordinary host alignment and prove every vec4 upload offset explicitly.
    struct LegacyBumpUniform
    {
        vsg::vec4 matrix{ 1.0f, 0.0f, 0.0f, 1.0f };
        vsg::vec4 lumaUv{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static_assert(sizeof(LegacyBumpUniform) == sizeof(vsg::vec4) * 2u);
    static_assert(std::is_standard_layout_v<LegacyBumpUniform>);
    static_assert(offsetof(LegacyBumpUniform, matrix) == 0u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyBumpUniform, lumaUv) == 1u * sizeof(vsg::vec4));

    using LegacyBumpUniformValue = vsg::Value<LegacyBumpUniform>;

    namespace legacy_bump_shader_detail
    {
        [[nodiscard]] inline bool insertAfterOnce(
            std::string& source, std::string_view needle, std::string_view insertion)
        {
            const std::size_t offset = source.find(needle);
            if (offset == std::string::npos)
                return false;
            source.insert(offset + needle.size(), insertion);
            return true;
        }

        [[nodiscard]] inline bool replaceOnce(
            std::string& source, std::string_view needle, std::string_view replacement)
        {
            const std::size_t offset = source.find(needle);
            if (offset == std::string::npos)
                return false;
            source.replace(offset, needle.size(), replacement);
            return true;
        }

        [[nodiscard]] inline bool addImportDefine(std::string& source)
        {
            constexpr std::string_view prefix = "#pragma import_defines (";
            const std::size_t begin = source.find(prefix);
            if (begin == std::string::npos)
                return false;
            const std::size_t close = source.find(')', begin + prefix.size());
            if (close == std::string::npos)
                return false;
            if (source.substr(begin, close - begin).find("OPENMW_LEGACY_BUMP_MAP") != std::string::npos)
                return true;
            source.insert(close, ", OPENMW_LEGACY_BUMP_MAP");
            return true;
        }

        [[nodiscard]] inline bool patchFragmentSource(std::string& source)
        {
            if (!addImportDefine(source))
                return false;

            if (!insertAfterOnce(source,
                    "#ifdef VSG_SPECULAR_MAP\n"
                    "layout(set = MATERIAL_DESCRIPTOR_SET, binding = 5) uniform sampler2D specularMap;\n"
                    "#endif\n",
                    "#ifdef OPENMW_LEGACY_BUMP_MAP\n"
                    "layout(set = MATERIAL_DESCRIPTOR_SET, binding = 17) uniform sampler2D openmwBumpMap;\n"
                    "layout(set = MATERIAL_DESCRIPTOR_SET, binding = 18) uniform OpenMwLegacyBumpData\n"
                    "{\n"
                    "    vec4 matrix;\n"
                    "    vec4 lumaUv;\n"
                    "} openmwLegacyBump;\n"
                    "#endif\n"))
                return false;

            // A NiTexturingProperty bump stage has no standalone surface-normal
            // meaning in V3.25. It perturbs the legacy environment lookup only.
            // Therefore the base compatibility shader deliberately binds and
            // preserves it without sampling it. When an exact environment path
            // is present (single sphere map or 32-frame enchanted path), apply
            // the same coordinate/luma equation as compatibility/objects.frag.
            if (source.find("OPENMW_ENCHANTED_ENVIRONMENT") == std::string::npos)
                return true;

            if (!insertAfterOnce(source,
                    "#endif\n"
                    "    int openmwGlowFrame = clamp(int(openmwEnvironment.temporalEffects.x + 0.5), 0, 31);\n",
                    "    float openmwEnvLuma = 1.0;\n"
                    "#ifdef OPENMW_LEGACY_BUMP_MAP\n"
                    "    int openmwBumpUvSet = clamp(int(openmwLegacyBump.lumaUv.z + 0.5), 0, VSG_TEXCOORD_COUNT - 1);\n"
                    "    vec4 openmwBumpSample = texture(openmwBumpMap, texCoord[openmwBumpUvSet].st);\n"
                    "    mat2 openmwBumpMatrix = mat2(openmwLegacyBump.matrix.x, openmwLegacyBump.matrix.y,\n"
                    "        openmwLegacyBump.matrix.z, openmwLegacyBump.matrix.w);\n"
                    "    openmwEnvUv += openmwBumpSample.rg * openmwBumpMatrix;\n"
                    "    openmwEnvLuma = clamp(openmwBumpSample.b * openmwLegacyBump.lumaUv.x\n"
                    "        + openmwLegacyBump.lumaUv.y, 0.0, 1.0);\n"
                    "#endif\n"))
                return false;

            return replaceOnce(source,
                "        * openmwEnvironmentEffect.colorStrength.rgb;\n",
                "        * openmwEnvironmentEffect.colorStrength.rgb * openmwEnvLuma;\n");
        }

        [[nodiscard]] inline vsg::ref_ptr<vsg::ShaderSet> cloneShaderSet(const vsg::ShaderSet& base)
        {
            auto result = vsg::ShaderSet::create();
            result->stages = base.stages;
            result->attributeBindings = base.attributeBindings;
            result->descriptorBindings = base.descriptorBindings;
            result->pushConstantRanges = base.pushConstantRanges;
            result->definesArrayStates = base.definesArrayStates;
            result->optionalDefines = base.optionalDefines;
            result->defaultGraphicsPipelineStates = base.defaultGraphicsPipelineStates;
            result->customDescriptorSetBindings = base.customDescriptorSetBindings;
            result->defaultShaderHints = base.defaultShaderHints;
            return result;
        }
    }

    [[nodiscard]] inline vsg::ref_ptr<LegacyBumpUniformValue> makeLegacyBumpMaterial(
        const RenderCore::MaterialRecord& source)
    {
        auto result = LegacyBumpUniformValue::create();
        result->value().matrix = vsg::vec4(source.bumpMapMatrix.x, source.bumpMapMatrix.y,
            source.bumpMapMatrix.z, source.bumpMapMatrix.w);
        std::uint32_t uvSet = 0u;
        const auto binding = std::find_if(source.textures.begin(), source.textures.end(),
            [](const RenderCore::TextureBinding& value) { return value.role == RenderCore::TextureRole::Bump; });
        if (binding != source.textures.end())
            uvSet = binding->transform.uvSet;
        result->value().lumaUv = vsg::vec4(source.environmentMapLumaBias.x,
            source.environmentMapLumaBias.y, static_cast<float>(uvSet), 0.0f);
        return result;
    }

    // Strict derivative of an already-validated OpenMW legacy ShaderSet. The
    // fail-closed boundary remains intact: only the exact NiTexturingProperty
    // bump facet is added here. Other unsupported texture roles/effects remain
    // unsupported until they get their own compatibility implementation.
    [[nodiscard]] inline vsg::ref_ptr<vsg::ShaderSet> createLegacyBumpCompatibilityShaderSet(
        vsg::ref_ptr<vsg::ShaderSet> base)
    {
        if (!base)
            return {};
        auto result = legacy_bump_shader_detail::cloneShaderSet(*base);
        if (!result)
            return {};

        for (const vsg::DescriptorBinding& binding : result->descriptorBindings)
        {
            if (binding.set == 1u
                && (binding.binding == LegacyBumpTextureBinding || binding.binding == LegacyBumpUniformBinding))
                return {};
        }

        bool fragmentPatched = false;
        for (vsg::ref_ptr<vsg::ShaderStage>& stage : result->stages)
        {
            if (!stage || !stage->module || stage->stage != VK_SHADER_STAGE_FRAGMENT_BIT)
                continue;
            std::string source = stage->module->source;
            if (!legacy_bump_shader_detail::patchFragmentSource(source))
                return {};
            auto replacement = vsg::ShaderStage::create(*stage);
            replacement->module = vsg::ShaderModule::create(std::move(source), stage->module->hints);
            stage = std::move(replacement);
            fragmentPatched = true;
            break;
        }
        if (!fragmentPatched)
            return {};

        result->addDescriptorBinding("openmwBumpMap", "OPENMW_LEGACY_BUMP_MAP", 1,
            LegacyBumpTextureBinding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, {}, vsg::CoordinateSpace::LINEAR);
        result->addDescriptorBinding("openmwLegacyBump", "OPENMW_LEGACY_BUMP_MAP", 1,
            LegacyBumpUniformBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
            VK_SHADER_STAGE_FRAGMENT_BIT, LegacyBumpUniformValue::create());
        result->optionalDefines.insert("OPENMW_LEGACY_BUMP_MAP");
        result->variants.clear();
        return result;
    }
}

#endif
