#!/usr/bin/env python3
"""VulkanMW Phase 3C native animation runtime-substitution contract."""

from __future__ import annotations

import pathlib
import re
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


def main() -> int:
    require(PROGRAM_HPP, (
        "defaultTranslation",
        "defaultRotation",
        "defaultScale",
        "TransformTrackSample sample(float time) const noexcept",
    ))
    require(PROGRAM_CPP, (
        "interpolator->mDefaultValue",
        "target.defaultTranslation",
        "target.defaultRotation",
        "target.defaultScale",
    ))
    require(RECORDS, (
        "glm::mat4 sourceParentPath",
        "glm::mat4 sourceLocal",
    ))
    require(TRANSLATOR, (
        "const glm::mat4 sourceLocal = mResult.model.nodes[modelNode].localTransform;",
        "bone.sourceParentPath = sourceParentPath;",
        "bone.sourceLocal = sourceLocal;",
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
        "controller->isInterpolating()",
        "rotate->isEnabled()",
        "state.layers[blendMask]",
    ))
    if "mMatrixInSkeletonSpace" in state_source[state_source.find("captureV4NativeAnimationState"):
                                                state_source.find("prepareV4PersistentObject")]:
        fail("Animation state snapshot must not copy evaluated skeleton matrices")

    runtime = require(RUNTIME_CPP, (
        "NifKeyframeClipCompiler",
        "controller.timing.map(layer.time)",
        "controller.track.sample(keyTime)",
        "bone.sourceParentPath * animatedSourceLocal",
        "result.sampledTracks == 0",
    ))
    for forbidden in ("updateBoneMatrices", "mMatrixInSkeletonSpace", "SceneUtil::Bone"):
        if forbidden in runtime:
            fail(f"native animation runtime regained evaluated OSG pose dependency: {forbidden}")

    bridge = require(BRIDGE_CPP, (
        "mNativeAnimation->captureSkeletonPose",
        "nativePose.applied()",
        "native_animation_fallback",
        "native_animation_runtime",
        "legacy_actor_poses",
    ))
    native_call = bridge.find("mNativeAnimation->captureSkeletonPose")
    legacy_update = bridge.find("evaluated->updateBoneMatrices", native_call)
    if native_call < 0 or legacy_update < 0 or native_call > legacy_update:
        fail("native pose substitution must precede the compatibility skeleton traversal")

    require(BRIDGE_HPP, ("std::unique_ptr<V4NativeAnimationRuntime> mNativeAnimation;",))
    require(ENGINE_SOURCES, ("apps/openmw/mwrender/vulkanmw/nativeanimationruntime.cpp",))

    print("VulkanMW Phase 3C native animation runtime substitution contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
