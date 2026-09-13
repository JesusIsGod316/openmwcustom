#!/usr/bin/env python3
from pathlib import Path
import re


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

# The Vulkan frame boundary must snapshot the evaluated graph using active-child
# traversal, otherwise inactive osg::Switch branches and visibility animation
# would be resurrected. The result is renderer-neutral ImmediateEffectDraw data;
# OSG is an evaluator/source here, never a Vulkan presentation backend.
require(bridge, "if (!ptr.getClass().isActor())", "non-actor frame capture")
require(bridge, "needsEvaluatedCapture = ptr.getClass().useAnim()", "useAnim evaluated capture")
require(bridge, 'v4_effect_detail::CaptureVisitor visitor("animated-object:" + *identity, true, mVfs)',
        "evaluated object neutral capture")
require(bridge, "visitor.setTraversalMode(osg::NodeVisitor::TRAVERSE_ACTIVE_CHILDREN)",
        "animated switch/visibility preservation")
require(bridge, "source.immediateEffectDraws.push_back(std::move(draw))", "neutral frame publication")
require(bridge, "InstanceSemanticFlag::OrdinaryWorld", "animated world-object semantic classification")
require(bridge, "InstanceSemanticFlag::ShadowCaster", "animated world-object shadow eligibility")
require(bridge, "InstanceSemanticFlag::ReflectionEligible", "animated world-object reflection eligibility")
require(bridge, "InstanceSemanticFlag::RefractionEligible", "animated world-object refraction eligibility")
require(effects, "captureV4WholeEffectSubtree", "evaluated OSG-to-neutral compatibility seam")
require(effects, "evaluated effect requires soft-particle opaque-depth sampling", "soft-particle fail-closed guard")
require(effects, "evaluated effect requires the post-process distortion target", "distortion fail-closed guard")
require(effects, "evaluated effect requires authored polygon-offset realization", "polygon-offset fail-closed guard")

# Headless Vulkan still executes the OSG update traversal. That keeps OpenMW's
# controller graph authoritative and current before V4 snapshots evaluated nodes.
require(engine, "mViewer->updateTraversal();", "authoritative animation update traversal")
require(engine, "presentVulkanFrame(frametime, false);", "Vulkan gameplay presentation")

print("V4 evaluated non-actor/mod animation source contract: PASS")
