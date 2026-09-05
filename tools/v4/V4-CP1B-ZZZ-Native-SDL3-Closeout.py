#!/usr/bin/env python3
# V4 CP1B final native SDL3 closeout pass.
#
# This runs after V4-CP1B-Migrate-SDL3.py. It fixes fractional-DPI math,
# materializes the remaining one-to-one SDL3 old-name aliases against the
# exact pinned SDL 3.4.10 oldnames table, and then disables the alias bridge.

from __future__ import annotations

import hashlib
import re
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (ROOT / "components", ROOT / "apps", ROOT / "extern" / "oics")
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".c"}

SDL_OLDNAMES_URL = (
    "https://raw.githubusercontent.com/libsdl-org/SDL/"
    "release-3.4.10/include/SDL3/SDL_oldnames.h"
)
SDL_OLDNAMES_GIT_BLOB_SHA1 = "cbf045330769be544d1dd9cc88e252689b22f3fe"


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    path = ROOT / rel
    old = path.read_text(encoding="utf-8")
    if old != text:
        path.write_text(text, encoding="utf-8")


def iter_sources():
    for base in SOURCE_ROOTS:
        for path in sorted(base.rglob("*")):
            if path.suffix in SOURCE_SUFFIXES:
                yield path


def fix_fractional_dpi() -> None:
    rel = "components/sdlutil/sdlinputwrapper.hpp"
    text = read(rel)
    old = """        Uint16 mScaleX;\n        Uint16 mScaleY;\n"""
    new = """        float mScaleX;\n        float mScaleY;\n"""
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: DPI scale member block not found")
    write(rel, text)

    rel = "components/sdlutil/sdlinputwrapper.cpp"
    text = read(rel)
    old = """    void InputWrapper::_setWindowScale()\n    {\n        int w, h;\n        SDL_GetWindowSize(mSDLWindow, &w, &h);\n        int dw, dh;\n        SDL_GetWindowSizeInPixels(mSDLWindow, &dw, &dh);\n        mScaleX = static_cast<Uint16>(dw / w);\n        mScaleY = static_cast<Uint16>(dh / h);\n    }\n"""
    new = """    void InputWrapper::_setWindowScale()\n    {\n        const float density = SDL_GetWindowPixelDensity(mSDLWindow);\n        const float scale = density > 0.f ? density : 1.f;\n        mScaleX = scale;\n        mScaleY = scale;\n    }\n"""
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: inherited integer DPI scale block not found")
    write(rel, text)

    rel = "components/sdlutil/sdlgraphicswindow.cpp"
    text = read(rel)
    old = """        int w, h;\n        SDL_GetWindowSize(mWindow, &w, &h);\n        int dw, dh;\n        SDL_GetWindowSizeInPixels(mWindow, &dw, &dh);\n\n        SDL_SetWindowPosition(mWindow, x, y);\n        SDL_SetWindowSize(mWindow, width / (dw / w), height / (dh / h));\n        return true;\n"""
    new = """        const float density = SDL_GetWindowPixelDensity(mWindow);\n        const float scale = density > 0.f ? density : 1.f;\n\n        SDL_SetWindowPosition(mWindow, x, y);\n        SDL_SetWindowSize(mWindow, static_cast<int>(static_cast<float>(width) / scale + 0.5f),\n            static_cast<int>(static_cast<float>(height) / scale + 0.5f));\n        return true;\n"""
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: inherited integer window-DPI conversion block not found")
    write(rel, text)


def git_blob_sha1(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def fetch_pinned_oldname_map() -> dict[str, str]:
    request = urllib.request.Request(
        SDL_OLDNAMES_URL,
        headers={"User-Agent": "OpenMW-Custom-CP1B-SDL3-Materializer"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        data = response.read()

    actual = git_blob_sha1(data)
    if actual != SDL_OLDNAMES_GIT_BLOB_SHA1:
        raise RuntimeError(
            "Pinned SDL 3.4.10 SDL_oldnames.h identity mismatch: "
            f"expected {SDL_OLDNAMES_GIT_BLOB_SHA1}, got {actual}"
        )

    text = data.decode("utf-8")
    start_marker = "#ifdef SDL_ENABLE_OLD_NAMES"
    end_marker = "#elif !defined(SDL_DISABLE_OLD_NAMES)"
    start = text.find(start_marker)
    end = text.find(end_marker, start + len(start_marker))
    if start < 0 or end < 0:
        raise RuntimeError("Could not isolate SDL_ENABLE_OLD_NAMES mapping block")

    mapping: dict[str, str] = {}
    define_re = re.compile(r"^#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
    for line in text[start:end].splitlines():
        match = define_re.match(line)
        if match:
            old, new = match.groups()
            if old != new:
                mapping[old] = new

    if len(mapping) < 250:
        raise RuntimeError(f"Unexpectedly small SDL old-name mapping: {len(mapping)} entries")
    return mapping


def replace_identifier(text: str, old: str, new: str) -> str:
    return re.sub(rf"(?<![A-Za-z0-9_]){re.escape(old)}(?![A-Za-z0-9_])", new, text)


def materialize_remaining_old_names() -> None:
    cmake = read("CMakeLists.txt")
    enabled = "add_compile_definitions(SDL_ENABLE_OLD_NAMES)" in cmake
    disabled = "add_compile_definitions(SDL_DISABLE_OLD_NAMES)" in cmake

    # First materialization pass: convert every alias that SDL itself exposes
    # for 3.4.10. Sorting longest-first prevents shorter type names from
    # corrupting longer function identifiers.
    if enabled:
        mapping = fetch_pinned_oldname_map()
        ordered = sorted(mapping.items(), key=lambda item: len(item[0]), reverse=True)
        for path in iter_sources():
            text = path.read_text(encoding="utf-8")
            updated = text
            for old, new in ordered:
                updated = replace_identifier(updated, old, new)
            if updated != text:
                path.write_text(updated, encoding="utf-8")

        cmake = cmake.replace(
            """# CP1B links the native SDL3 runtime. SDL3's old-name aliases remain enabled\n# only as a compile-time bridge for harmless one-to-one renames; semantic\n# changes are ported explicitly and enforced by the CP1B source audit.\nadd_compile_definitions(SDL_ENABLE_OLD_NAMES)\n""",
            """# CP1B is now source-native SDL3. Disable SDL's old-name compatibility\n# aliases so any future SDL2 spelling fails at compile time instead of silently\n# passing through a transition macro. Semantic API changes are ported explicitly.\nadd_compile_definitions(SDL_DISABLE_OLD_NAMES)\n""",
            1,
        )
        if "SDL_ENABLE_OLD_NAMES" in cmake:
            raise RuntimeError("CMakeLists.txt: SDL_ENABLE_OLD_NAMES remains after native closeout")
        write("CMakeLists.txt", cmake)
    elif not disabled:
        raise RuntimeError("CMakeLists.txt: neither SDL_ENABLE_OLD_NAMES nor SDL_DISABLE_OLD_NAMES is set")


def verify_native_closeout() -> None:
    cmake = read("CMakeLists.txt")
    if "SDL_ENABLE_OLD_NAMES" in cmake:
        raise RuntimeError("SDL_ENABLE_OLD_NAMES remains enabled")
    if "add_compile_definitions(SDL_DISABLE_OLD_NAMES)" not in cmake:
        raise RuntimeError("SDL_DISABLE_OLD_NAMES compile guard is missing")

    header = read("components/sdlutil/sdlinputwrapper.hpp")
    if "float mScaleX;" not in header or "float mScaleY;" not in header:
        raise RuntimeError("InputWrapper DPI scale is not floating point")
    if "Uint16 mScaleX;" in header or "Uint16 mScaleY;" in header:
        raise RuntimeError("InputWrapper still truncates DPI scale to Uint16")

    wrapper = read("components/sdlutil/sdlinputwrapper.cpp")
    if "SDL_GetWindowPixelDensity(mSDLWindow)" not in wrapper:
        raise RuntimeError("InputWrapper does not use SDL3 window pixel density")
    if "static_cast<Uint16>(dw / w)" in wrapper or "static_cast<Uint16>(dh / h)" in wrapper:
        raise RuntimeError("InputWrapper integer DPI division remains")

    graphics = read("components/sdlutil/sdlgraphicswindow.cpp")
    if "SDL_GetWindowPixelDensity(mWindow)" not in graphics:
        raise RuntimeError("GraphicsWindow does not use SDL3 window pixel density")
    if "width / (dw / w)" in graphics or "height / (dh / h)" in graphics:
        raise RuntimeError("GraphicsWindow integer DPI division remains")

    # These patterns are the highest-risk alias families in the current OpenMW
    # SDL surface. With SDL_DISABLE_OLD_NAMES, any missed obscure alias will also
    # fail the compiler, but common regressions should fail the cheap preflight.
    forbidden_patterns = {
        r"\bSDL_(?:APP|AUDIODEVICE|CLIPBOARDUPDATE|CONTROLLER|DISPLAYEVENT|DROP|FINGER|JOY|KEYDOWN|KEYUP|KEYMAPCHANGED|MOUSEBUTTON|MOUSEMOTION|MOUSEWHEEL|QUIT|SENSORUPDATE|TEXTEDITING|TEXTINPUT|USEREVENT)\b":
            "SDL2-era event token remains",
        r"(?<!SDL_)\bKMOD_[A-Z0-9_]+\b": "SDL2 modifier token remains",
        r"\bSDLK_[a-z]\b": "SDL2 lowercase keycode token remains",
        r"\bSDL_GameController[A-Za-z0-9_]*\b": "SDL2 GameController token remains",
        r"\bSDL_Joystick(?:Close|FromInstanceID|FromPlayerIndex|Get|InstanceID|Name|Num|Open|Path|Rumble|Send|Set|Update)\b":
            "SDL2 joystick alias remains",
        r"\bSDL_Sensor(?:Close|FromInstanceID|Get|Open|Update)\b": "SDL2 sensor alias remains",
        r"\bSDL_GL_DeleteContext\b": "SDL2 GL context destructor alias remains",
        r"\bSDL_GetWindowDisplayMode\b|\bSDL_SetWindowDisplayMode\b|\bSDL_GetDisplayOrientation\b":
            "SDL2 video alias remains",
        r"\bSDL_FreeCursor\b|\bSDL_FreeSurface\b": "SDL2 destructor alias remains",
        r"\bSDL_RenderCopy(?:Ex|F|ExF)?\b": "SDL2 render-copy alias remains",
    }
    offenders: list[str] = []
    for path in iter_sources():
        text = path.read_text(encoding="utf-8")
        for pattern, reason in forbidden_patterns.items():
            if re.search(pattern, text):
                offenders.append(f"{path.relative_to(ROOT)}: {reason}: {pattern}")
    if offenders:
        raise RuntimeError("CP1B native SDL3 closeout incomplete:\n" + "\n".join(offenders))


def main() -> None:
    fix_fractional_dpi()
    materialize_remaining_old_names()
    verify_native_closeout()
    print("CP1B native SDL3 old-name and fractional-DPI closeout verified.")


if __name__ == "__main__":
    main()
