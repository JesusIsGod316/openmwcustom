from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# The V4 route deliberately keeps OpenMW's source-side OSG update traversal
# alive, then snapshots already-evaluated controller/particle state. Tighten the
# new capture seam before wiring producers into it.
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    "#include <osg/Geometry>\n",
    "#include <osg/Geometry>\n#include <osg/GL>\n",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            auto result = osg::StateSet::create();\n""",
    """            osg::ref_ptr<osg::StateSet> result = new osg::StateSet;\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """        {\n            CapturedMaterial captured;\n            if (!captureMaterial(path, particles.getStateSet(), captured, diagnostic))\n""",
    """        {\n            if (particles.getUseShaders())\n            {\n                diagnostic = \"shader-evaluated particle system requires a CPU-equivalent V4 effect facet\";\n                return false;\n            }\n            if (particles.getSortMode() == osgParticle::ParticleSystem::SORT_FRONT_TO_BACK)\n            {\n                diagnostic = \"front-to-back particle sorting is outside the evaluated V4 effect contract\";\n                return false;\n            }\n            if (particles.getVisibilityDistance() > 0.0)\n            {\n                diagnostic = \"particle visibility-distance culling requires an explicit V4 effect facet\";\n                return false;\n            }\n            CapturedMaterial captured;\n            if (!captureMaterial(path, particles.getStateSet(), captured, diagnostic))\n""",
)
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            const osg::ref_ptr<osg::StateSet> state = effectiveState(path, particles.getStateSet());\n            const glm::mat4 systemWorld = toGlm(osg::computeLocalToWorld(path));\n""",
    """            if (particles.getSortMode() == osgParticle::ParticleSystem::NO_SORT)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;\n            else if (captured.material.alphaBlendEnabled)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Sorted;\n            const osg::ref_ptr<osg::StateSet> state = effectiveState(path, particles.getStateSet());\n            const glm::mat4 systemWorld = toGlm(osg::computeLocalToWorld(path));\n""",
)

# Expose only the evaluated glow color. OSG texture/controller objects remain
# source-side and never cross the neutral renderer boundary.
replace_once(
    "components/sceneutil/util.hpp",
    """        bool isDone();\n\n        void setColor(const osg::Vec4f& color);\n""",
    """        bool isDone();\n\n        [[nodiscard]] const osg::Vec4f& getColor() const noexcept { return mColor; }\n\n        void setColor(const osg::Vec4f& color);\n""",
)
replace_once(
    "apps/openmw/mwrender/animation.hpp",
    """        [[nodiscard]] bool hasV4EffectAttachments() const noexcept\n        {\n            return mHasMagicEffects || mGlowUpdater.valid();\n        }\n""",
    """        [[nodiscard]] bool hasV4EffectAttachments() const noexcept\n        {\n            return mHasMagicEffects || mGlowUpdater.valid();\n        }\n        [[nodiscard]] bool hasV4UpdateVfxAttachments() const noexcept { return mHasMagicEffects; }\n        [[nodiscard]] std::optional<osg::Vec4f> getV4GlowColor() const noexcept\n        {\n            if (!mGlowUpdater || mGlowUpdater->isDone())\n                return std::nullopt;\n            return mGlowUpdater->getColor();\n        }\n""",
)

# Root-level temporary spell glow may override an actor model that already owns
# equipment environment bindings. Preserve the old strict behavior by default;
# only the explicit actor-root override path can replace existing environment
# bindings. Make that variant revision-aware because composed NPC models mutate
# in place as equipment changes.
replace_once(
    "components/nifrender/enchantedglow.hpp",
    """        const RenderCore::Color& color, bool applyLightingToEnvironmentMaps = false)\n""",
    """        const RenderCore::Color& color, bool applyLightingToEnvironmentMaps = false,\n        bool replaceExistingEnvironment = false)\n""",
)
replace_once(
    "components/nifrender/enchantedglow.hpp",
    """        const std::string suffix = applyLightingToEnvironmentMaps ? \"#openmw-enchanted-glow-prelight:\"\n                                                                   : \"#openmw-enchanted-glow-postlight:\";\n        const std::string variantSuffix = suffix + enchanted_glow_detail::colorIdentity(color);\n""",
    """        std::string suffix = applyLightingToEnvironmentMaps ? \"#openmw-enchanted-glow-prelight:\"\n                                                            : \"#openmw-enchanted-glow-postlight:\";\n        if (replaceExistingEnvironment)\n            suffix += \"override-r\" + std::to_string(source->revision.value()) + \":\";\n        const std::string variantSuffix = suffix + enchanted_glow_detail::colorIdentity(color);\n""",
)
replace_once(
    "components/nifrender/enchantedglow.hpp",
    """            if (std::any_of(baseMaterial->textures.begin(), baseMaterial->textures.end(),\n                    [](const TextureBinding& binding) { return binding.role == TextureRole::Environment; }))\n            {\n                enchanted_glow_detail::cancelReservations(world, {}, materials, frames);\n                return { EnchantedGlowPublishStatus::ExistingEnvironmentBinding, {} };\n            }\n""",
    """            const bool hasExistingEnvironment = std::any_of(baseMaterial->textures.begin(), baseMaterial->textures.end(),\n                [](const TextureBinding& binding) { return binding.role == TextureRole::Environment; });\n            if (hasExistingEnvironment && !replaceExistingEnvironment)\n            {\n                enchanted_glow_detail::cancelReservations(world, {}, materials, frames);\n                return { EnchantedGlowPublishStatus::ExistingEnvironmentBinding, {} };\n            }\n""",
)
replace_once(
    "components/nifrender/enchantedglow.hpp",
    """            MaterialRecord record = *baseMaterial;\n            record.revision = InitialResourceRevision;\n""",
    """            MaterialRecord record = *baseMaterial;\n            if (replaceExistingEnvironment)\n            {\n                record.textures.erase(std::remove_if(record.textures.begin(), record.textures.end(),\n                                          [](const TextureBinding& binding) {\n                                              return binding.role == TextureRole::Environment;\n                                          }),\n                    record.textures.end());\n            }\n            record.revision = InitialResourceRevision;\n""",
)

# Realize legacy no-lighting effect materials explicitly instead of treating
# them as an unresolved runtime-context effect. The eighth material vec4 already
# had an unused component, so this does not expand the descriptor ABI.
replace_once(
    "components/render/backend/vsg/legacymaterialshader.hpp",
    """        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior.\n        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };\n""",
    """        // x = MaterialFogMode, y = fog depth, z = additive-fog behavior,\n        // w = legacy unlit/no-lighting material.\n        vsg::vec4 effects{ 0.0f, 0.0f, 0.0f, 0.0f };\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """    vec3 color = vec3(0.0);\n    const float intensityMinimum = 0.001;\n\n    vec4 lightNums = lightData.values[0];\n""",
    """    vec3 color = vec3(0.0);\n    const float intensityMinimum = 0.001;\n    const bool materialUnlit = material.effects.w > 0.5;\n\n    vec4 lightNums = materialUnlit ? vec4(0.0) : lightData.values[0];\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """    int openmwPointLightCount = min(int(openmwLocalLights.header.x), openmwLocalLights.values.length() / 5);\n""",
    """    int openmwPointLightCount = materialUnlit\n        ? 0\n        : min(int(openmwLocalLights.header.x), openmwLocalLights.values.length() / 5);\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """    outColor.rgb = color * ambientOcclusion + surfaceColor.rgb * effectiveEmission.rgb * emissiveMultiplier;\n#ifdef VSG_EMISSIVE_MAP\n""",
    """    if (materialUnlit)\n    {\n        vec3 unlitColor = vertexColorMode == 1\n            ? effectiveEmission.rgb * emissiveMultiplier\n            : effectiveDiffuse.rgb;\n        outColor.rgb = surfaceColor.rgb * unlitColor;\n    }\n    else\n        outColor.rgb = color * ambientOcclusion + surfaceColor.rgb * effectiveEmission.rgb * emissiveMultiplier;\n#ifdef VSG_EMISSIVE_MAP\n""",
)
replace_once(
    "components/render/backend/vsg/legacymaterialshader.cpp",
    """        uniform.effects = vsg::vec4(static_cast<float>(source.fog.mode), source.fog.depth,\n            additiveFog ? 1.0f : 0.0f, 0.0f);\n""",
    """        uniform.effects = vsg::vec4(static_cast<float>(source.fog.mode), source.fog.depth,\n            additiveFog ? 1.0f : 0.0f, source.unlit ? 1.0f : 0.0f);\n""",
)
replace_once(
    "components/render/backend/vsg/staticassetrealizer.cpp",
    """            if (material->unlit)\n            {\n                ++result.stats.runtimeContextEffects;\n                result.diagnostics.emplace_back(\n                    \"Legacy unlit material requires a dedicated compatibility shader variant\");\n            }\n""",
    "",
)

# Snapshot live magic-bolt evaluated roots and their authoritative legacy light
# color. Lifetime remains owned by ProjectileManager; ref_ptr only keeps the
# source subtree valid through the same-frame neutral capture.
replace_once(
    "apps/openmw/mwworld/projectilemanager.hpp",
    "#include <osg/Vec3f>\n",
    "#include <osg/Vec3f>\n#include <osg/Vec4f>\n",
)
replace_once(
    "apps/openmw/mwworld/projectilemanager.hpp",
    """    struct V4ProjectileFrameSnapshot\n    {\n        std::vector<V4PhysicalProjectileSnapshot> physicalProjectiles;\n        std::size_t liveMagicBoltCount = 0;\n    };\n""",
    """    struct V4MagicBoltSnapshot\n    {\n        int runtimeId = 0;\n        osg::ref_ptr<osg::PositionAttitudeTransform> effectRoot;\n        osg::Vec4f lightDiffuse;\n    };\n\n    struct V4ProjectileFrameSnapshot\n    {\n        std::vector<V4PhysicalProjectileSnapshot> physicalProjectiles;\n        std::vector<V4MagicBoltSnapshot> magicBolts;\n    };\n""",
)
replace_once(
    "apps/openmw/mwworld/projectilemanager.cpp",
    """        for (const MagicBoltState& bolt : mMagicBolts)\n        {\n            if (!bolt.mToDelete)\n                ++result.liveMagicBoltCount;\n        }\n""",
    """        result.magicBolts.reserve(mMagicBolts.size());\n        for (const MagicBoltState& bolt : mMagicBolts)\n        {\n            if (bolt.mToDelete || !bolt.mNode)\n                continue;\n            result.magicBolts.push_back(V4MagicBoltSnapshot{\n                .runtimeId = bolt.mProjectileId,\n                .effectRoot = bolt.mNode,\n                .lightDiffuse = getMagicBoltLightDiffuseColor(bolt.mEffects),\n            });\n        }\n""",
)

# Guard the stage against accidental partial application.
checks = {
    "apps/openmw/mwrender/v4effectcapture.hpp": ["new osg::StateSet", "getUseShaders()", "SORT_FRONT_TO_BACK"],
    "apps/openmw/mwrender/animation.hpp": ["hasV4UpdateVfxAttachments", "getV4GlowColor"],
    "components/nifrender/enchantedglow.hpp": ["replaceExistingEnvironment", "override-r"],
    "components/render/backend/vsg/legacymaterialshader.cpp": ["materialUnlit", "source.unlit ? 1.0f : 0.0f"],
    "apps/openmw/mwworld/projectilemanager.hpp": ["V4MagicBoltSnapshot", "magicBolts"],
    "apps/openmw/mwworld/projectilemanager.cpp": ["V4MagicBoltSnapshot", "getMagicBoltLightDiffuseColor"],
}
for path, needles in checks.items():
    text = Path(path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing staged CP4F foundation token {needle!r}")

if "Legacy unlit material requires a dedicated compatibility shader variant" in Path(
    "components/render/backend/vsg/staticassetrealizer.cpp"
).read_text(encoding="utf-8"):
    raise RuntimeError("legacy unlit effect path remains fail-closed after shader support")
if "liveMagicBoltCount" in Path("apps/openmw/mwworld/projectilemanager.hpp").read_text(encoding="utf-8"):
    raise RuntimeError("magic bolt snapshot still uses count-only sentinel")

print("CP4F evaluated-effect producer foundations applied")
