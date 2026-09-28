#!/usr/bin/env python3
"""Clockwork P1 post-processing boundary/source contract.

The Vulkan OMWFX backend must consume only frozen native FX records. OSG is
allowed in components/fx where OpenMW parses/imports effects, but must not leak
into components/render/backend/vsg/omwfx.cpp.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def require(text: str, needle: str, why: str) -> None:
    if needle not in text:
        raise SystemExit(f"missing {why}: {needle}")

native = (ROOT / "components/fx/nativeplan.hpp").read_text(encoding="utf-8")
backend = (ROOT / "components/render/backend/vsg/omwfx.cpp").read_text(encoding="utf-8")
camera = (ROOT / "components/fx/stateupdater.hpp").read_text(encoding="utf-8")

for forbidden in ("osg::", "#include <osg/", '#include "osg/'):
    if forbidden in backend:
        raise SystemExit(f"native OMWFX backend regained OSG dependency: {forbidden}")

for needle, why in [
    ("struct NativeTexturePayload", "owned native texture pixels"),
    ("struct NativeTexture", "neutral imported texture"),
    ("struct NativeRenderTarget", "neutral render target"),
    ("enum class NativeBlendFactor", "neutral blend factors"),
    ("enum class NativeBlendOperation", "neutral blend equations"),
    ("makeNativeTexture", "OSG-to-native texture freeze"),
    ("makeNativeRenderTarget", "OSG-to-native target freeze"),
]:
    require(native, needle, why)

for needle, why in [
    ("ImageInfo texture(const Fx::NativeTexture& texture)", "native texture consumption"),
    ("VkBlendFactor blendFactor(Fx::NativeBlendFactor value)", "Vulkan blend factor translation"),
    ("VkBlendOp blendOperation(Fx::NativeBlendOperation value)", "Vulkan blend equation translation"),
    ("attachment.blendEnable = VK_TRUE", "authored blend enable"),
    ("&view.current.projection.matrix[0][0]", "backend-neutral camera state handoff"),
]:
    require(backend, needle, why)

require(camera, "const float* projection", "backend-neutral native camera overload")
print("Clockwork P1 native post-processing boundary PASS")
