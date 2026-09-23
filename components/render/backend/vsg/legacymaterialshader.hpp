#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYMATERIALSHADER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYMATERIALSHADER_H

#include <components/rendercore/records.hpp>
#include <cstddef>
#include <type_traits>

#include <vsg/core/Value.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/io/Options.h>
#include <vsg/maths/vec4.h>

#include <string_view>

namespace vsg
{
    class ShaderSet;
}

namespace RenderVsg
{
    // Backend-private uniform contract for the legacy/Gamebryo compatibility
    // shader family. Ten vec4 slots keep the CPU/GPU layout explicit and
    // stable: colors, scalar parameters, semantic selectors, and material fog.
    // RenderCore stays renderer agnostic and later material families may differ.
    // std140 constrains byte offsets in the uploaded buffer, not host object alignment.
    // Pinned VSG Object::operator new does not promise over-aligned allocations.
    // Keep ordinary host alignment and prove every vec4 upload offset explicitly.
    struct LegacyMaterialUniform
    {
        vsg::vec4 ambientColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        vsg::vec4 diffuseColor{ 1.0f, 1.0f, 1.0f, 1.0f };
        vsg::vec4 specularColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        vsg::vec4 emissiveColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        // x=shininess, y=alpha cutoff, z=specular strength,
        // w=emissive multiplier. The latter two remain separate because legacy
        // specular maps and emissive vertex colors replace the source color but
        // do not replace these authored scalar multipliers.
        vsg::vec4 parameters{ 0.0f, 0.5f, 1.0f, 1.0f };
        // x=VertexColorMode, y=alpha-test enabled, z=CompareOp,
        // w=two-sided lighting enabled. Values are exact small integers encoded
        // as floats to keep the std140 layout in tightly-defined vec4 slots.
        vsg::vec4 semantics{ 0.0f, 0.0f, 7.0f, 0.0f };
        // xyz = NiFogProperty override color; w = TextureApplyMode. The apply
        // selector shares this slot because fog only consumes RGB and the
        // values are exact small integers under std140.
        vsg::vec4 fogColor{ 0.0f, 0.0f, 0.0f, 2.0f };
        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior,
        // w = unlit bit 1 and preview force-opaque bit 2.
        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };
        // xyz = per-draw replacement for the global/sun ambient term;
        // w = override enabled. Local point-light ambient is intentionally separate.
        vsg::vec4 ambientOverride{ 1.0f, 1.0f, 1.0f, 0.0f };
        // x=DarkTexture UV set, y=DecalTexture UV set, z=GlossTexture UV set.
        // Values are exact small integers encoded as floats. w packs LAND flags:
        // terrain=1, diffuse-alpha specular=2, normal-alpha height=4, RG normal=8, legacy tangent control=16.
        vsg::vec4 textureCoordSets{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static_assert(sizeof(LegacyMaterialUniform) == sizeof(vsg::vec4) * 10u);
    static_assert(std::is_standard_layout_v<LegacyMaterialUniform>);
    static_assert(offsetof(LegacyMaterialUniform, ambientColor) == 0u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, diffuseColor) == 1u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, specularColor) == 2u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, emissiveColor) == 3u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, parameters) == 4u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, semantics) == 5u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, fogColor) == 6u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, effects) == 7u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, ambientOverride) == 8u * sizeof(vsg::vec4));
    static_assert(offsetof(LegacyMaterialUniform, textureCoordSets) == 9u * sizeof(vsg::vec4));

    using LegacyMaterialUniformValue = vsg::Value<LegacyMaterialUniform>;

    [[nodiscard]] vsg::ref_ptr<LegacyMaterialUniformValue> makeLegacyCompatibilityMaterial(
        const RenderCore::MaterialRecord& source);

    // Builds an OpenMW-owned compatibility ShaderSet from VSG 1.1.15's Phong
    // vertex/descriptor/view contract, replacing the fragment material semantics
    // that do not match V3.25. The returned ShaderSet owns no ModernPBR meaning.
    [[nodiscard]] vsg::ref_ptr<vsg::ShaderSet> createLegacyCompatibilityShaderSet(
        vsg::ref_ptr<const vsg::Options> options = {});

    // Exposed for source-level regression tests. Runtime code consumes the same
    // exact string through createLegacyCompatibilityShaderSet().
    [[nodiscard]] std::string_view legacyCompatibilityFragmentShaderSource() noexcept;
}

#endif
