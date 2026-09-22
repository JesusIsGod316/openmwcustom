#!/usr/bin/env python3
from pathlib import Path


def source(path: str) -> str:
    value = Path(path).read_text(encoding="utf-8")
    if not value:
        raise SystemExit(f"empty source file: {path}")
    return value


def require(text: str, token: str, label: str) -> None:
    if token not in text:
        raise SystemExit(f"{label}: missing required source contract: {token}")


def forbid(text: str, token: str, label: str) -> None:
    if token in text:
        raise SystemExit(f"{label}: forbidden source contract returned: {token}")


lifecycle = source("apps/openmw/mwrender/v4scenerenderlifecycle.cpp")
bridge = source("apps/openmw/mwrender/v4enginerenderbridge.cpp")
effects = source("apps/openmw/mwrender/v4effectcapture.hpp")
effect_frame = source("components/rendercore/effectframe.hpp")
objects = source("apps/openmw/mwrender/objects.cpp")
animation = source("apps/openmw/mwrender/animation.cpp")
engine = source("apps/openmw/engine.cpp")

# Mod and base-game non-actor animation continue to use OpenMW's established
# animation-source discovery/evaluation path. V4 must copy evaluated state rather
# than rejecting external KF sources or pretending they are immutable statics.
require(objects, "new ObjectAnimation(ptr, animationMesh, mResourceSystem, animated, allowLight)",
        "authoritative ObjectAnimation construction")
require(animation, "if (animated)\n                addAnimSource(model, model);", "external animation source loading")
require(animation, "if (Settings::game().mUseAdditionalAnimSources)\n            loadAdditionalAnimations(kfname, baseModel);",
        "mod additional-animation discovery")
forbid(lifecycle, "external animation source before model-animation compatibility is available",
       "obsolete external-animation rejection")
require(lifecycle, "const bool animatedClass = ptr.getClass().useAnim();", "animated class routing")
require(lifecycle, "retirePersistentObject();", "frozen static retirement")
require(lifecycle, "requiresModelPlayback(*modelRecord)", "embedded controller routing")

# Non-useAnim objects still need one canonical model inspection to detect embedded
# controllers, but that decision is model-content stable for a world epoch. Do not
# redo NIF translation/model lookup for every animated-frame snapshot.
require(bridge, "mEvaluatedObjectPlaybackEpoch != worldEpoch", "playback cache epoch invalidation")
require(bridge, "mEvaluatedObjectPlayback.clear();", "playback cache reset")
require(bridge, "mEvaluatedObjectPlayback.find(modelPath.value())", "playback cache lookup")
require(bridge, "mEvaluatedObjectPlayback.emplace(std::string(modelPath.value()), needsEvaluatedCapture)",
        "playback cache population")

# The Vulkan frame boundary snapshots the evaluated graph using active-child
# traversal, otherwise inactive osg::Switch branches and visibility animation
# would be resurrected. Attached UpdateVfx subtrees are deliberately excluded
# from the ordinary-world pass and captured separately so their Effect semantics
# are not overwritten with shadow-casting world-object flags.
require(bridge, "class AnimatedObjectCaptureVisitor final", "filtered evaluated object capture")
require(bridge, "osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)", "animated switch/visibility preservation")
require(bridge, "nestedEffectRoot", "attached-effect exclusion")
require(bridge, "v4_effect_detail::isEffectRoot(node)", "UpdateVfx root classification")
require(bridge, 'AnimatedObjectCaptureVisitor objectVisitor("animated-object:" + *identity, mVfs, &mTextureIdentities)',
        "evaluated object neutral capture")
require(bridge, 'v4_effect_detail::CaptureVisitor effectVisitor("animated-object-effect:" + *identity, false, mVfs, &mTextureIdentities)',
        "separate attached-effect capture")
require(bridge, "effectVisitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN)",
        "attached-effect active-child traversal")
require(bridge, "draw.semanticFlags = worldObjectFlags", "world semantics limited to object draw pass")
require(bridge, "for (RenderCore::ImmediateEffectDraw& draw : capturedEffects->draws)",
        "attached effects published without world semantic overwrite")
require(bridge, "source.immediateEffectDraws.push_back(std::move(draw))", "neutral frame publication")
require(bridge, "InstanceSemanticFlag::OrdinaryWorld", "animated world-object semantic classification")
require(bridge, "InstanceSemanticFlag::ShadowCaster", "animated world-object shadow eligibility")
require(bridge, "InstanceSemanticFlag::ReflectionEligible", "animated world-object reflection eligibility")
require(bridge, "InstanceSemanticFlag::RefractionEligible", "animated world-object refraction eligibility")
forbid(bridge, 'v4_effect_detail::CaptureVisitor visitor("animated-object:" + *identity, true, mVfs)',
       "unfiltered whole-object/effect semantic mixing")
require(effect_frame, "semanticFlag(InstanceSemanticFlag::Effect)", "default attached-effect semantic classification")
require(effects, "captureV4WholeEffectSubtree", "evaluated OSG-to-neutral compatibility seam")
require(effects, "softDepthFallback", "soft-particle compatibility fallback")
require(effects, "distortionFallback", "distortion compatibility fallback")
forbid(effects, "evaluated effect requires soft-particle opaque-depth sampling", "soft-particle world-load fatal")
forbid(effects, "evaluated effect requires the post-process distortion target", "distortion world-load fatal")
require(effects, "evaluated effect requires authored polygon-offset realization", "polygon-offset fail-closed guard")

# Headless Vulkan still executes the OSG update traversal. That keeps OpenMW's
# controller graph authoritative and current before V4 snapshots evaluated nodes.
require(engine, "mViewer->updateTraversal();", "authoritative animation update traversal")
require(engine, "prepareVulkanFrame(frametime, false);", "Vulkan gameplay frame capture")
require(engine, "presentPreparedVulkanFrame();", "Vulkan immutable gameplay presentation")
if not (engine.index("mViewer->updateTraversal();")
        < engine.index("prepareVulkanFrame(frametime, false);")
        < engine.index("mLuaWorker->allowUpdate(frameStart, frameNumber, *stats);")
        < engine.index("presentPreparedVulkanFrame();")):
    raise SystemExit("Vulkan frame ownership: expected update -> capture -> Lua release -> presentation ordering")

require(effects, "rig->evaluateGeometry(visitor.getTraversalNumber(), visitor.getNodePath())",
        "canonical rig evaluation instead of rest-pose capture")
require(effects, "morph->evaluateGeometry(visitor.getTraversalNumber())", "canonical morph evaluation")
require(bridge, "objectVisitor.setTraversalNumber(mPoseTraversal)", "evaluated geometry frame identity")
print("V4 evaluated non-actor/mod animation source contract: PASS")
