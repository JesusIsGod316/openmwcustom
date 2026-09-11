#!/usr/bin/env python3
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]


def require(path: str, *needles: str) -> None:
    text = (ROOT / path).read_text(encoding="utf-8")
    for needle in needles:
        if needle not in text:
            raise SystemExit(f"CP4E contract missing {needle!r} in {path}")


require(
    "apps/openmw/mwrender/v4semanticsource.cpp",
    "result.waterEnabled = cell.hasWater();",
    "result.waterHeight = cell.hasWater() ? cell.getWaterHeight() : 0.0;",
    "result.underwater = underwater;",
)
require(
    "components/rendercore/frameproducer.hpp",
    "struct WaterViews",
    "ViewKind::Reflection",
    "ViewKind::Refraction",
    "struct AuxiliaryView",
)
require(
    "components/render/backend/vsg/vsgruntimehost.cpp",
    "createOffscreenRenderTarget",
    "waterViewsCompatible",
    "ReflectionTraversalMask",
    "RefractionTraversalMask",
    "mWaterSurface.update",
)
require(
    "components/render/backend/vsg/runtime-sources.cmake",
    "offscreenrendertarget.cpp",
    "watersurface.cpp",
)
require(
    "apps/openmw/mwrender/v4runtimeoptions.cpp",
    "Settings::water().mRttSize",
    "Settings::water().mReflectionDetail",
    "Settings::water().mRefraction",
)

runtime = (ROOT / "components/render/backend/vsg/vsgruntimehost.cpp").read_text(encoding="utf-8")
if "water requires the CP4E environment compatibility facet" in runtime:
    raise SystemExit("CP4E retained the old unconditional water rejection")
if "vsgopenmw" in runtime.lower():
    raise SystemExit("CP4E runtime unexpectedly depends on donor ownership")

print("V4 CP4E water/environment source contract: PASS")
