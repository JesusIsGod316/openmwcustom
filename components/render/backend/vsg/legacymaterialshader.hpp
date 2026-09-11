#ifndef OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYMATERIALSHADER_H
#define OPENMW_COMPONENTS_RENDER_BACKEND_VSG_LEGACYMATERIALSHADER_H

#include <components/rendercore/records.hpp>

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
    // shader family. Eight vec4 slots keep the CPU/GPU layout explicit and
    // stable: colors, scalar parameters, semantic selectors, and material fog.
    // RenderCore stays renderer agnostic and later material families may differ.
    struct alignas(16) LegacyMaterialUniform
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
        // xyz = NiFogProperty override color; w is reserved.
        vsg::vec4 fogColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior,
        // w = legacy unlit/no-lighting material.
        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };
    };

    static_assert(sizeof(LegacyMaterialUniform) == sizeof(vsg::vec4) * 8u);
    static_assert(alignof(LegacyMaterialUniform) >= 16u);

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
