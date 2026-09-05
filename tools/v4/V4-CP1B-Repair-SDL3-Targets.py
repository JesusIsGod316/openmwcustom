#!/usr/bin/env python3
"""Replace remaining application-level SDL2 link targets for CP1B."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = (
    ROOT / "apps/openmw/CMakeLists.txt",
    ROOT / "apps/launcher/CMakeLists.txt",
)

changed = 0
for path in TARGETS:
    text = path.read_text(encoding="utf-8")
    if "SDL2::SDL2" not in text:
        if "SDL3::SDL3" not in text:
            raise RuntimeError(f"expected SDL link target missing in {path}")
        continue
    updated = text.replace("SDL2::SDL2", "SDL3::SDL3")
    path.write_text(updated, encoding="utf-8")
    changed += 1

print(f"CP1B SDL3 application target repair complete: changed={changed}")
