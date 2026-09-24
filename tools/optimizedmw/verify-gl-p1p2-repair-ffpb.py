#!/usr/bin/env python3
"""Verify the combined OptimizedMW P1/P2 repair with latest green hybrid FFPB source."""

from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HYBRID = "54f3e43a95469c5978f761ed76083a3107360796"

HYBRID_FILES = [
    "apps/openmw/mwrender/animation.cpp",
    "apps/openmw/mwrender/animation.hpp",
    "apps/openmw/mwrender/animblendcontroller.cpp",
    "apps/openmw/mwrender/animblendcontroller.hpp",
    "apps/openmw/mwrender/hybridanimationtimemap.hpp",
    "apps/openmw/mwrender/npcanimation.cpp",
    "apps/openmw/mwrender/npcanimation.hpp",
    "apps/openmw_tests/CMakeLists.txt",
    "apps/openmw_tests/mwrender/testhybridanimationtimemap.cpp",
    "components/settings/categories/camera.hpp",
]

def git(*args: str) -> bytes:
    return subprocess.check_output(("git", *args), cwd=ROOT)

for path in HYBRID_FILES:
    expected = git("show", f"{HYBRID}:{path}")
    actual = git("show", f"HEAD:{path}")
    if actual != expected:
        raise SystemExit(f"hybrid source drift from {HYBRID}: {path}")

settings = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
for line in (
    "full body first person hybrid animations = false",
    "opimizedmw paging optimizer = false",
    "opimizedmw paging readiness split = false",
    "opimizedmw host pressure = false",
    "opimizedmw speculative budget = false",
):
    if settings.count(line) != 1:
        raise SystemExit(f"missing or duplicate combined setting: {line}")

budget = (ROOT / "components/resource/speculativebudget.hpp").read_text(encoding="utf-8")
cell = (ROOT / "apps/openmw/mwworld/cellpreloader.cpp").read_text(encoding="utf-8")
terrain = (ROOT / "components/terrain/quadtreeworld.cpp").read_text(encoding="utf-8")
engine = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")

checks = {
    "near-future priority type": "SpeculativePriority : unsigned char" in budget,
    "reserved near-future capacity": "reservedNearFutureJobs" in budget and "reservedNearFutureJobs" in engine,
    "caution admits near-future": "OpenGlPressure::Caution" in budget and "NearFuture" in budget,
    "critical weak fallback": "HostMemoryPressure::Critical" in cell,
    "direct strong upgrade": "preloadStrongUpgrade" in cell and "preloadStrongUpgrade" in terrain,
    "weak private owner release": "entry.mRenderingNode = nullptr" in terrain,
    "required wait does not join optional tail": "Do not join the optional strong-upgrade tail" in cell,
}
failed = [name for name, ok in checks.items() if not ok]
if failed:
    raise SystemExit("combined repair contract failed: " + ", ".join(failed))

print(f"Latest green hybrid FFPB ({HYBRID}) and combined P1/P2 repair contracts verified.")
