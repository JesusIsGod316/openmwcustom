#!/usr/bin/env python3
"""VulkanMW Phase 3C native animation runtime-substitution contract."""

from __future__ import annotations

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]

ANIMATION_HPP = ROOT / "apps/openmw/mwrender/animation.hpp"
ANIMATION_CPP = ROOT / "apps/openmw/mwrender/animation.cpp"
BLEND_HPP = ROOT / "apps/openmw/mwrender/animblendcontroller.hpp"
ROTATE_HPP = ROOT / "apps/openmw/mwrender/rotatecontroller.hpp"
RUNTIME_HPP = ROOT / "apps/openmw/mwrender/vulkanmw/nativeanimationruntime.hpp"
RUNTIME_CPP = ROOT / "apps/openmw/mwrender/vulkanmw/nativeanimationruntime.cpp"
BRIDGE_HPP = ROOT / "apps/openmw/mwrender/v4enginerenderbridge.hpp"
BRIDGE_CPP = ROOT / "apps/openmw/mwrender/v4enginerenderbridge.cpp"
ENGINE_SOURCES = ROOT / "apps/openmw/mwrender/v4engine-sources.cmake"
RECORDS = ROOT / "components/rendercore/records.hpp"
TRANSLATOR = ROOT / "components/nifrender/niftranslator.cpp"
PROGRAM_HPP = ROOT / "components/render/native/nifcontrollerprogram.hpp"
PROGRAM_CPP = ROOT / "components/render/native/nifcontrollerprogram.cpp"


def fail(message: str) -> None:
    print(f"VulkanMW Phase 3C runtime substitution FAILED: {message}", file=sys.stderr)
    raise SystemExit(1)


def require(path: pathlib.Path, needles: tuple[str, ...]) -> str:
    if not path.is_file():
        fail(f"missing {path.relative_to(ROOT)}")
    text = path.read_text(encoding="utf-8", errors="replace")
    for needle in needles:
        if needle not in text:
            fail(f"{path.relative_to(ROOT)} lost required contract: {needle}")
    return text


def forbid(path: pathlib.Path, needles: tuple[str, ...]) -> None:
    text = require(path, ())
    for needle in needles:
        if needle in text:
            fail(f"{path.relative_to(ROOT)} regained forbidden Phase 3C behavior: {needle}")


def main() -> int:
    program_hpp = require(PROGRAM_HPP, (
        "struct TransformTrackSample",
        "TransformTrackSample sample(float time) const noexcept",
        "result.translation = translations.sample(time);",
        "result.scale = scales.sample(time);",
    ))
    # Missing KF channels have compatibility-specific behavior. Do not silently
    # synthesize NiTransformInterpolator defaults into the per-frame sample.
    for removed in ("defaultTranslation", "defaultRotation", "defaultScale"):
        if removed in program_hpp:
            fail(f"native transform track incorrectly synthesizes removed default channel: {removed}")

    forbid(PROGRAM_CPP, (
        "target.defaultTranslation",
        "target.defaultRotation",
        "target.defaultScale",
    ))

    require(RECORDS, (
        "glm::mat4 sourceParentPath",
        "glm::mat4 sourceLocal",
        "bool sourceAnimationBoundary",
        "std::uint32_t sourceControllerFlags",
        "std::uint32_t sourceParentControllerFlags",
        "std::vector<std::string> sourceParentPathNodes",
    ))
    require(TRANSLATOR, (
        "const glm::mat4 sourceLocal = mResult.model.nodes[modelNode].localTransform;",
        "bone.sourceParentPath = sourceParentPath;",
        "bone.sourceLocal = sourceLocal;",
        "bone.sourceAnimationBoundary = true;",
        "bone.sourceControllerFlags = mResult.model.nodes[modelNode].controllerFlags;",
        "bone.sourceParentControllerFlags = sourceParentControllerFlags;",
        "bone.sourceParentPathNodes = std::move(sourceParentPathNodes);",
    ))

    require(BLEND_HPP, ("bool isInterpolating() const noexcept",))
    require(ROTATE_HPP, ("bool isEnabled() const noexcept",))
    require(ANIMATION_HPP, (
        "struct V4NativeAnimationLayer",
        "struct V4NativeAnimationState",
        "captureV4NativeAnimationState",
    ))

    state_source = require(ANIMATION_CPP, (
        "mV4NativeBoneNames",
        "mV4NativeKf",
        "OPENMW_V4_LEGACY_ANIMATION_CAPTURE_CONTROL",
        "dynamic_cast<const NifAnimBlendController*>(callback.get())",
        "dynamic_cast<const BoneAnimBlendController*>(callback.get())",
        "dynamic_cast<const HybridNifAnimController*>(callback.get())",
        "rotate->isEnabled()",
        "state.layers[blendMask]",
        "mAccumRoot && mAccumCtrl",
    ))
    state_begin = state_source.find("bool Animation::captureV4NativeAnimationState")
    state_end = state_source.find("V4PersistentObject* Animation::prepareV4PersistentObject", state_begin)
    if state_begin < 0 or state_end < 0:
        fail("could not isolate Animation native-state snapshot implementation")
    state_body = state_source[state_begin:state_end]
    for forbidden in ("mMatrixInSkeletonSpace", "updateBoneMatrices"):
        if forbidden in state_body:
            fail(f"Animation native-state snapshot reads evaluated scenegraph pose: {forbidden}")

    require(RUNTIME_HPP, (
        "void beginFrame();",
        "captureSkeletonPose(",
        "std::string_view actorIdentity",
        "seedSkeletonPose(",
        "void endFrame();",
        "void clear();",
    ))
    runtime = require(RUNTIME_CPP, (
        "NifKeyframeClipCompiler",
        "native animation pose requires one compatibility seed frame",
        "controller.timing.map(layer.time)",
        "controller.track.sample(keyTime)",
        "seedSkeletonPose",
        "bone.sourceParentPath * composeNifLocal(actorPose.sourceLocal[i])",
        "binding.collapsedParentNodes",
        "sourceControllerFlags",
        "sourceParentControllerFlags",
        "mSeenActors",
    ))
    for forbidden in ("updateBoneMatrices", "mMatrixInSkeletonSpace", "SceneUtil::Bone"):
        if forbidden in runtime:
            fail(f"native animation runtime regained evaluated OSG pose dependency: {forbidden}")

    bridge = require(BRIDGE_CPP, (
        "mNativeAnimation->beginFrame()",
        "mNativeAnimation->captureSkeletonPose(",
        "mNativeAnimation->seedSkeletonPose(",
        "mNativeAnimation->endFrame()",
        "nativePose.applied()",
        "native_animation_fallback",
        "native_animation_seed_fallback",
        "native_animation_runtime",
        "native_actor_poses",
        "legacy_actor_poses",
        "sampled_tracks",
    ))
    native_call = bridge.find("mNativeAnimation->captureSkeletonPose")
    legacy_update = bridge.find("evaluated->updateBoneMatrices", native_call)
    if native_call < 0 or legacy_update < 0 or native_call > legacy_update:
        fail("native pose substitution must precede compatibility skeleton traversal")

    seed_call = bridge.find("mNativeAnimation->seedSkeletonPose", native_call)
    if seed_call < legacy_update:
        fail("compatibility seed must be captured after the exact legacy pose has been reconstructed")

    require(BRIDGE_HPP, ("std::unique_ptr<V4NativeAnimationRuntime> mNativeAnimation;",))
    require(ENGINE_SOURCES, ("apps/openmw/mwrender/vulkanmw/nativeanimationruntime.cpp",))

    print("VulkanMW Phase 3C native animation runtime substitution contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
