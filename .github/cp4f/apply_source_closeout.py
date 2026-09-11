from pathlib import Path


def replace_once(path: str, old: str, new: str) -> None:
    p = Path(path)
    text = p.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected exactly one guarded match, found {count}")
    p.write_text(text.replace(old, new, 1), encoding="utf-8")


# Particle current color is part of the evaluated drawable state, regardless of
# whether the source material itself requested authored mesh vertex colors.
replace_once(
    "apps/openmw/mwrender/v4effectcapture.hpp",
    """            if (particles.getSortMode() == osgParticle::ParticleSystem::NO_SORT)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;\n            else if (captured.material.alphaBlendEnabled)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Sorted;\n""",
    """            captured.material.vertexColorMode = RenderCore::VertexColorMode::AmbientDiffuse;\n            if (particles.getSortMode() == osgParticle::ParticleSystem::NO_SORT)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;\n            else if (captured.material.alphaBlendEnabled)\n                captured.material.transparentSort = RenderCore::TransparentSortPolicy::Sorted;\n""",
)

# Effects without a bone name are attached to Animation::mInsert, not
# necessarily mObjectRoot. Expose only that source-side root; the V4 capture
# helper still exports neutral evaluated draw values rather than OSG objects.
replace_once(
    "apps/openmw/mwrender/animation.hpp",
    """        osg::Group* getObjectRoot();\n\n        // Transitional V4 producer access. The evaluated OSG skeleton remains\n""",
    """        osg::Group* getObjectRoot();\n        osg::Group* getV4EffectRoot() const noexcept { return mInsert.get(); }\n\n        // Transitional V4 producer access. The evaluated OSG skeleton remains\n""",
)

# Wire the authoritative projectile snapshot into the frame-local evaluated
# effect stream and the existing neutral local-light publication path.
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.hpp",
    """        [[nodiscard]] bool synchronizeProjectiles();\n""",
    """        [[nodiscard]] bool synchronizeProjectiles(V4MainFrameSource& source);\n""",
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.hpp",
    """        std::set<std::string, std::less<>> mProjectileInstances;\n        RenderCore::WorldEpoch mProjectileEpoch;\n""",
    """        std::set<std::string, std::less<>> mProjectileInstances;\n        std::set<std::string, std::less<>> mProjectileLights;\n        RenderCore::WorldEpoch mProjectileEpoch;\n""",
)

replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    '#include "v4enginerenderbridge.hpp"\n',
    '#include "v4enginerenderbridge.hpp"\n#include "v4effectcapture.hpp"\n',
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """    bool V4EngineRenderBridge::synchronizeProjectiles()\n""",
    """    bool V4EngineRenderBridge::synchronizeProjectiles(V4MainFrameSource& source)\n""",
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """        {\n            mProjectileInstances.clear();\n            mProjectileEpoch = mSession->world().epoch();\n        }\n""",
    """        {\n            mProjectileInstances.clear();\n            mProjectileLights.clear();\n            mProjectileEpoch = mSession->world().epoch();\n        }\n""",
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """        std::set<std::string, std::less<>> currentProjectiles;\n\n        std::optional<RenderCore::ActiveCellSource> activeCell;\n        if (!frame.physicalProjectiles.empty())\n""",
    """        std::set<std::string, std::less<>> currentProjectiles;\n        std::set<std::string, std::less<>> currentProjectileLights;\n\n        std::optional<RenderCore::ActiveCellSource> activeCell;\n        if (!frame.physicalProjectiles.empty() || !frame.magicBolts.empty())\n""",
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """                mLastDiagnostic = \"live physical projectiles have no authoritative active cell identity\";\n""",
    """                mLastDiagnostic = \"live projectiles have no authoritative active cell identity\";\n""",
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """        mProjectileInstances = std::move(currentProjectiles);\n\n        // Magic bolts use rotating NIF controllers, particle texture overrides,\n        // dynamic lights, and sometimes multi-effect Dummy attachments. Until\n        // that evaluated payload has a neutral V4 representation, failing here\n        // is required; treating it as a static mesh would be silent visual loss.\n        if (frame.liveMagicBoltCount != 0)\n        {\n            mLastDiagnostic = \"live magic projectile requires particle/controller realization\";\n            return false;\n        }\n\n        return true;\n""",
    """        mProjectileInstances = std::move(currentProjectiles);\n\n        for (const MWWorld::V4MagicBoltSnapshot& bolt : frame.magicBolts)\n        {\n            if (!bolt.effectRoot || !activeCell)\n            {\n                mLastDiagnostic = \"live magic projectile has no evaluated source root or active cell\";\n                return false;\n            }\n            V4EffectCaptureResult captured = captureV4WholeEffectSubtree(\n                *bolt.effectRoot, \"magic-projectile:\" + std::to_string(bolt.runtimeId));\n            if (!captured.valid())\n            {\n                mLastDiagnostic = captured.diagnostic.empty()\n                    ? \"live magic projectile could not produce evaluated V4 effect draws\"\n                    : captured.diagnostic;\n                return false;\n            }\n            for (RenderCore::ImmediateEffectDraw& draw : captured.draws)\n                source.immediateEffectDraws.push_back(std::move(draw));\n\n            RenderCore::CellLightSource light;\n            light.identity = \"magic-projectile-light:\" + std::to_string(bolt.runtimeId);\n            light.cellIdentity = activeCell->identity;\n            const osg::Vec3f position = bolt.effectRoot->getPosition();\n            light.light.position = { position.x(), position.y(), position.z() };\n            light.light.diffuse = { bolt.lightDiffuse.r(), bolt.lightDiffuse.g(), bolt.lightDiffuse.b(),\n                bolt.lightDiffuse.a() };\n            light.light.specular = { 0.0f, 0.0f, 0.0f, 0.0f };\n            light.light.ambient = { 1.0f, 1.0f, 1.0f, 1.0f };\n            light.light.constantAttenuation = 0.0f;\n            light.light.linearAttenuation = 0.1f;\n            light.light.quadraticAttenuation = 0.0f;\n            light.light.effectiveRadius = 66.0f;\n            light.light.actorFade = 1.0f;\n            light.light.semanticFlags = RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Dynamic);\n            const RenderCore::ActiveCellPublishResult published = mSession->cells().upsertLight(light);\n            if (published.status != RenderCore::ActiveCellPublishStatus::Applied\n                && published.status != RenderCore::ActiveCellPublishStatus::AlreadyPresent)\n            {\n                mLastDiagnostic = \"magic projectile light publication failed\";\n                return false;\n            }\n            currentProjectileLights.insert(light.identity);\n        }\n\n        for (const std::string& identity : mProjectileLights)\n        {\n            if (currentProjectileLights.contains(identity))\n                continue;\n            const RenderCore::ActiveCellPublishResult removed = mSession->cells().removeLight(identity);\n            if (removed.status != RenderCore::ActiveCellPublishStatus::Applied\n                && removed.status != RenderCore::ActiveCellPublishStatus::NotFound)\n            {\n                mLastDiagnostic = \"stale magic projectile light retirement failed\";\n                return false;\n            }\n        }\n        mProjectileLights = std::move(currentProjectileLights);\n        return true;\n""",
)
replace_once(
    "apps/openmw/mwrender/v4engineframecoordinator.cpp",
    """        if (!mBridge.synchronizeProjectiles())\n            return fail(mBridge.lastDiagnostic().empty()\n                    ? \"authoritative projectile state could not produce a compatible V4 frame\"\n                    : mBridge.lastDiagnostic());\n\n        std::optional<V4MainFrameSource> source = makeV4MainFrameSource(\n            rendering, cell, rendering.isUnderwater(), *extent, simulationTime, frameDelta, invalidateHistory);\n        if (!source)\n            return fail(\"authoritative gameplay state could not produce a compatible V4 main frame\");\n""",
    """        std::optional<V4MainFrameSource> source = makeV4MainFrameSource(\n            rendering, cell, rendering.isUnderwater(), *extent, simulationTime, frameDelta, invalidateHistory);\n        if (!source)\n            return fail(\"authoritative gameplay state could not produce a compatible V4 main frame\");\n\n        if (!mBridge.synchronizeProjectiles(*source))\n            return fail(mBridge.lastDiagnostic().empty()\n                    ? \"authoritative projectile state could not produce a compatible V4 frame\"\n                    : mBridge.lastDiagnostic());\n""",
)

# Actor-attached UpdateVfx subtrees become immediate neutral draws. Temporary
# actor-root caustic glow uses the existing exact 32-frame material path, with
# explicit environment replacement semantics from the foundation stage.
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    '#include "v4runtimeoptions.hpp"\n',
    '#include "v4runtimeoptions.hpp"\n#include "v4effectcapture.hpp"\n',
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    """            if (animation.hasV4EffectAttachments())\n            {\n                compatible = false;\n                mLastDiagnostic = \"active actor has a live magic/effect attachment\";\n                return;\n            }\n""",
    """            if (animation.hasV4UpdateVfxAttachments())\n            {\n                osg::Group* const effectRoot = animation.getV4EffectRoot();\n                if (!effectRoot)\n                {\n                    compatible = false;\n                    mLastDiagnostic = \"active actor effect attachment has no evaluated source root\";\n                    return;\n                }\n                V4EffectCaptureResult captured\n                    = captureV4AttachedEffects(*effectRoot, \"actor-effect:\" + *identity);\n                if (!captured.valid())\n                {\n                    compatible = false;\n                    mLastDiagnostic = captured.diagnostic.empty()\n                        ? \"active actor effect attachment could not produce evaluated V4 draws\"\n                        : captured.diagnostic;\n                    return;\n                }\n                for (RenderCore::ImmediateEffectDraw& draw : captured.draws)\n                    source.immediateEffectDraws.push_back(std::move(draw));\n            }\n""",
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    """                else\n                    actorModel = entry->second.model;\n            }\n\n            std::optional<RenderCore::InstanceHandle> handle = mSession->cells().findInstance(*identity);\n""",
    """                else\n                    actorModel = entry->second.model;\n            }\n\n            if (const std::optional<osg::Vec4f> sourceColor = animation.getV4GlowColor())\n            {\n                const RenderCore::Color color{\n                    sourceColor->r(), sourceColor->g(), sourceColor->b(), sourceColor->a() };\n                const NifRender::EnchantedGlowPublishResult glow = NifRender::publishEnchantedGlowVariant(\n                    mSession->world(), mSession->publisher(), mVfs, actorModel, color,\n                    Settings::shaders().mApplyLightingToEnvironmentMaps, true);\n                if (!glow.available())\n                {\n                    compatible = false;\n                    mLastDiagnostic = \"actor spell-cast glow publication failed: \" + enchantedGlowDiagnostic(glow.status);\n                    return;\n                }\n                actorModel = glow.model;\n                if (!mSession->world().get(actorModel))\n                {\n                    compatible = false;\n                    mLastDiagnostic = \"actor spell-cast glow variant returned a stale model handle\";\n                    return;\n                }\n            }\n\n            std::optional<RenderCore::InstanceHandle> handle = mSession->cells().findInstance(*identity);\n""",
)
replace_once(
    "apps/openmw/mwrender/v4enginerenderbridge.cpp",
    """            source.dynamicTransforms.clear();\n            source.skeletonPoses.clear();\n            source.morphWeights.clear();\n""",
    """            source.dynamicTransforms.clear();\n            source.skeletonPoses.clear();\n            source.morphWeights.clear();\n            source.immediateEffectDraws.clear();\n""",
)

# Verify the runtime blockers are truly wired rather than merely silenced.
checks = {
    "apps/openmw/mwrender/animation.hpp": ["getV4EffectRoot", "getV4GlowColor"],
    "apps/openmw/mwrender/v4enginerenderbridge.hpp": ["synchronizeProjectiles(V4MainFrameSource& source)", "mProjectileLights"],
    "apps/openmw/mwrender/v4engineframecoordinator.cpp": ["captureV4WholeEffectSubtree", "magic-projectile-light:", "synchronizeProjectiles(*source)"],
    "apps/openmw/mwrender/v4enginerenderbridge.cpp": ["captureV4AttachedEffects", "actor spell-cast glow", "replaceExistingEnvironment" if False else "mApplyLightingToEnvironmentMaps, true"],
}
for path, needles in checks.items():
    text = Path(path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise RuntimeError(f"{path}: missing CP4F live-effect wiring token {needle!r}")

coordinator = Path("apps/openmw/mwrender/v4engineframecoordinator.cpp").read_text(encoding="utf-8")
bridge = Path("apps/openmw/mwrender/v4enginerenderbridge.cpp").read_text(encoding="utf-8")
for stale in ["liveMagicBoltCount", "live magic projectile requires particle/controller realization"]:
    if stale in coordinator:
        raise RuntimeError(f"projectile fail-closed sentinel remains after live wiring: {stale}")
if "active actor has a live magic/effect attachment" in bridge:
    raise RuntimeError("actor live-effect fail-closed sentinel remains after evaluated capture wiring")

print("CP4F live actor/projectile effect producer wiring applied")
