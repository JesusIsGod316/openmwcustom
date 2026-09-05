#!/usr/bin/env python3
# CP1B SDL3 migration follow-up: preserve the specialized OpenGL extension
# header semantics that the generic bare-SDL include normalizer must not erase.

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# These are the complete CP1A source sites that intentionally included
# SDL_opengl_glext.h for OpenGL constants not supplied by the Windows 1.1 GL
# header / OSG transitive includes. The broad SDL include migration had
# collapsed them to SDL3/SDL.h, which does not supply those extension tokens.
TARGETS = (
    "apps/openmw/mwrender/postprocessor.cpp",
    "components/debug/gldebug.cpp",
    "components/fx/technique.cpp",
)

OLD_HEADERS = (
    "#include <SDL_opengl_glext.h>",
    "#include <SDL3/SDL.h>",
)
NEW_HEADER = "#include <SDL3/SDL_opengl_glext.h>"


def restore(rel: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")

    if NEW_HEADER in text:
        return

    matches = [header for header in OLD_HEADERS if header in text]
    if len(matches) != 1:
        raise RuntimeError(
            f"{rel}: expected exactly one migratable OpenGL extension include, found {matches!r}"
        )

    path.write_text(text.replace(matches[0], NEW_HEADER, 1), encoding="utf-8")


def verify() -> None:
    failures: list[str] = []
    for rel in TARGETS:
        text = (ROOT / rel).read_text(encoding="utf-8")
        if text.count(NEW_HEADER) != 1:
            failures.append(f"{rel}: SDL3 OpenGL extension header missing or duplicated")
        if "#include <SDL_opengl_glext.h>" in text:
            failures.append(f"{rel}: legacy bare SDL OpenGL extension include remains")

    if failures:
        raise RuntimeError("CP1B OpenGL extension-header parity failed:\n" + "\n".join(failures))


for target in TARGETS:
    restore(target)
verify()
print("CP1B SDL3 OpenGL extension-header parity restored for all CP1A sites.")
