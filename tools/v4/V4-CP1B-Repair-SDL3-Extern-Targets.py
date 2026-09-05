#!/usr/bin/env python3
"""Migrate remaining bundled SDL consumers to the native SDL3 CMake target."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = (
    ROOT / "extern/osg-ffmpeg-videoplayer/CMakeLists.txt",
    ROOT / "extern/oics/CMakeLists.txt",
)

changed = 0
for path in TARGETS:
    text = path.read_text(encoding="utf-8")
    count = text.count("SDL2::SDL2")
    if count == 0:
        if "SDL3::SDL3" not in text:
            raise RuntimeError(f"expected SDL target missing in {path}")
        continue
    if count != 1:
        raise RuntimeError(f"expected one SDL2 target in {path}, found {count}")
    path.write_text(text.replace("SDL2::SDL2", "SDL3::SDL3", 1), encoding="utf-8")
    changed += 1

print(f"CP1B SDL3 extern target repair complete: changed={changed}")
