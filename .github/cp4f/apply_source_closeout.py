from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# OpenMW actor VFX can override sun.ambient on the effect subtree while still
# receiving directional and local lights. Preserve that as neutral material
# state; treating it as unlit would suppress real lighting and is not equivalent.
replace_once(
    "components/rendercore/records.hpp",
    """        Color environmentMapColor{ 1.0f, 1.0f, 1.0f, 1.0f };\n        float shininess = 0.0f;\n""",
    """        Color environmentMapColor{ 1.0f, 1.0f, 1.0f, 1.0f };\n        bool ambientLightOverrideEnabled = false;\n        Color ambientLightOverride{ 1.0f, 1.0f, 1.0f, 1.0f };\n        float shininess = 0.0f;\n""",
)
replace_once(
    "components/rendercore/renderworld.hpp",
    """                || !semantic_detail::finite(record.emission) || !semantic_detail::finite(record.environmentMapColor)\n                || !std::isfinite(record.shininess) || !std::isfinite(record.emissiveMultiplier)\n""",
    """                || !semantic_detail::finite(record.emission) || !semantic_detail::finite(record.environmentMapColor)\n                || !semantic_detail::finite(record.ambientLightOverride)\n                || !std::isfinite(record.shininess) || !std::isfinite(record.emissiveMultiplier)\n""",
)

replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            material.alpha = material.diffuse.a;\n            if (const osg::Uniform* alpha = state->getUniform(\"alpha\"))\n""",
    """            if (const osg::Uniform* ambient = state->getUniform(\"sun.ambient\"))\n            {\n                osg::Vec4f value;\n                if (ambient->get(value))\n                {\n                    material.ambientLightOverrideEnabled = true;\n                    material.ambientLightOverride = toGlm(value);\n                }\n            }\n\n            material.alpha = material.diffuse.a;\n            if (const osg::Uniform* alpha = state->getUniform(\"alpha\"))\n""",
)

# Add one vec4 to the backend-private legacy material ABI. xyz carries the
# replacement ambient color and w is the enable bit. Local-light ambient terms
# remain untouched; only the view/global ambient light is replaced.
replace_once(
    "components/render/backend/vsg/legacymaterialshader.hpp",
    """        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior,\n        // w = legacy unlit/no-lighting material.\n        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };\n    };\n\n    static_assert(sizeof(LegacyMaterialUniform) == sizeof(vsg::vec4) * 8u);\n""",
    """        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior,\n        // w = legacy unlit/no-lighting material.\n        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };\n        // xyz = per-draw replacement for the global/sun ambient term;\n        // w = override enabled. Local point-light ambient is intentionally separate.\n        vsg::vec4 ambientOverride{ 1.0f, 1.0f, 1.0f, 0.0f };\n    };\n\n    static_assert(sizeof(LegacyMaterialUniform) == sizeof(vsg::vec4) * 9u);\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """    vec4 fogColor;\n    vec4 effects;\n} material;\n""",
    """    vec4 fogColor;\n    vec4 effects;\n    vec4 ambientOverride;\n} material;\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """        vec4 lightColor = lightData.values[lightDataIndex++];\n        color += surfaceColor.rgb * effectiveAmbient.rgb * lightColor.rgb * lightColor.a;\n""",
    """        vec4 lightColor = lightData.values[lightDataIndex++];\n        vec3 ambientLightColor = material.ambientOverride.w > 0.5\n            ? material.ambientOverride.rgb\n            : lightColor.rgb;\n        color += surfaceColor.rgb * effectiveAmbient.rgb * ambientLightColor * lightColor.a;\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """        uniform.effects = vsg::vec4(static_cast<float>(source.fog.mode), source.fog.depth,\n            additiveFog ? 1.0f : 0.0f, source.unlit ? 1.0f : 0.0f);\n        return result;\n""",
    """        uniform.effects = vsg::vec4(static_cast<float>(source.fog.mode), source.fog.depth,\n            additiveFog ? 1.0f : 0.0f, source.unlit ? 1.0f : 0.0f);\n        uniform.ambientOverride = vsg::vec4(source.ambientLightOverride.r, source.ambientLightOverride.g,\n            source.ambientLightOverride.b, source.ambientLightOverrideEnabled ? 1.0f : 0.0f);\n        return result;\n""",
)

checks = {
    "components/rendercore/records.hpp": ["ambientLightOverrideEnabled", "ambientLightOverride"],
    "apps/openmw/mwrender/v4effectcapture.hpp": ["getUniform(\"sun.ambient\")", "ambientLightOverrideEnabled = true"],
    "components/render/backend/vsg/legacymaterialshader.hpp": ["ambientOverride", "sizeof(vsg::vec4) * 9u"],
    "components/render/backend/vsg/legacymaterialshader.cpp": ["material.ambientOverride.w", "uniform.ambientOverride"],
}
for path, needles in checks.items():
    text = Path(path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing effect ambient override token {needle!r}")

print("CP4F per-effect ambient override semantics applied")
