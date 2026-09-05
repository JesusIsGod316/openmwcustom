#!/usr/bin/env python3
"""Materialize the V4 CP1B SDL3/OpenGL parity migration.

This is deliberately narrow: dependency resolution, SDL link target, and the
platform/input SDL include surface. OSG/OpenGL render semantics are not changed.
The script is fail-closed on the CP1A source patterns it replaces and is
idempotent after materialization.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

SDL3_VERSION = "3.4.10"
SDL3_VC_SHA256 = "e2b336b10b037934af98308027410732ef7b22f2c6697d58092aa1c209fae7d7"


def replace_once(path: Path, old: str, new: str) -> bool:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count == 0:
        if new in text:
            return False
        raise RuntimeError(f"expected source pattern missing in {path}: {old!r}")
    if count != 1:
        raise RuntimeError(f"expected exactly one source pattern in {path}, found {count}: {old!r}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8")
    return True


def migrate_root_cmake() -> bool:
    path = ROOT / "CMakeLists.txt"
    old = "find_package(SDL2 2.0.20 REQUIRED)"
    marker = "# V4 CP1B: pinned native SDL3 dependency"
    text = path.read_text(encoding="utf-8")
    if marker in text:
        return False
    if text.count(old) != 1:
        raise RuntimeError(f"expected one CP1A SDL2 dependency declaration, found {text.count(old)}")

    block = f'''{marker}\nset(OPENMW_SDL3_VERSION "{SDL3_VERSION}")\nset(OPENMW_SDL3_VC_SHA256 "{SDL3_VC_SHA256}")\n\n# CP1B's Windows CI bundle is still the frozen V3.25 dependency set, so SDL3\n# is pinned separately here. This keeps the migration reproducible without\n# changing the legacy OSG/OpenGL dependency bundle used by the control path.\nif(WIN32)\n    set(_openmw_sdl3_deps_dir "${{CMAKE_BINARY_DIR}}/_deps")\n    set(_openmw_sdl3_archive "${{_openmw_sdl3_deps_dir}}/SDL3-devel-${{OPENMW_SDL3_VERSION}}-VC.zip")\n    set(_openmw_sdl3_root "${{_openmw_sdl3_deps_dir}}/SDL3-${{OPENMW_SDL3_VERSION}}")\n    set(_openmw_sdl3_config "${{_openmw_sdl3_root}}/cmake/SDL3Config.cmake")\n    if(NOT EXISTS "${{_openmw_sdl3_config}}")\n        file(MAKE_DIRECTORY "${{_openmw_sdl3_deps_dir}}")\n        file(DOWNLOAD\n            "https://github.com/libsdl-org/SDL/releases/download/release-${{OPENMW_SDL3_VERSION}}/SDL3-devel-${{OPENMW_SDL3_VERSION}}-VC.zip"\n            "${{_openmw_sdl3_archive}}"\n            EXPECTED_HASH "SHA256=${{OPENMW_SDL3_VC_SHA256}}"\n            SHOW_PROGRESS\n            STATUS _openmw_sdl3_download_status)\n        list(GET _openmw_sdl3_download_status 0 _openmw_sdl3_download_code)\n        list(GET _openmw_sdl3_download_status 1 _openmw_sdl3_download_message)\n        if(NOT _openmw_sdl3_download_code EQUAL 0)\n            message(FATAL_ERROR "SDL3 download failed: ${{_openmw_sdl3_download_message}}")\n        endif()\n        execute_process(\n            COMMAND "${{CMAKE_COMMAND}}" -E tar xvf "${{_openmw_sdl3_archive}}"\n            WORKING_DIRECTORY "${{_openmw_sdl3_deps_dir}}"\n            RESULT_VARIABLE _openmw_sdl3_extract_result)\n        if(NOT _openmw_sdl3_extract_result EQUAL 0 OR NOT EXISTS "${{_openmw_sdl3_config}}")\n            message(FATAL_ERROR "Failed to extract pinned SDL3 ${{OPENMW_SDL3_VERSION}} development package")\n        endif()\n    endif()\n    list(PREPEND CMAKE_PREFIX_PATH "${{_openmw_sdl3_root}}")\nendif()\n\nfind_package(SDL3 {SDL3_VERSION} CONFIG REQUIRED)\n# SDL3 provides a native transition alias set while the remaining CP1B call\n# sites are converted to SDL3 names and semantics. No SDL2 runtime is linked.\nadd_compile_definitions(SDL_ENABLE_OLD_NAMES)'''

    path.write_text(text.replace(old, block, 1), encoding="utf-8")
    return True


def migrate_component_link() -> bool:
    return replace_once(ROOT / "components/CMakeLists.txt", "    SDL2::SDL2\n", "    SDL3::SDL3\n")


def migrate_sdl_includes() -> int:
    include_map = {
        "#include <SDL.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_events.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_gamecontroller.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_joystick.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_keyboard.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_keycode.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_mouse.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_scancode.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_sensor.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_video.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_clipboard.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_timer.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_stdinc.h>": "#include <SDL3/SDL.h>",
        "#include <SDL_version.h>": "#include <SDL3/SDL.h>",
    }

    roots = [ROOT / "components/sdlutil", ROOT / "apps/openmw"]
    changed = 0
    for base in roots:
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".cpp", ".hpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8")
            updated = text
            for old, new in include_map.items():
                updated = updated.replace(old, new)
            if updated != text:
                path.write_text(updated, encoding="utf-8")
                changed += 1
    return changed


def verify_no_legacy_headers() -> None:
    forbidden = tuple(
        name
        for name in (
            "<SDL.h>",
            "<SDL_events.h>",
            "<SDL_gamecontroller.h>",
            "<SDL_joystick.h>",
            "<SDL_keyboard.h>",
            "<SDL_keycode.h>",
            "<SDL_mouse.h>",
            "<SDL_scancode.h>",
            "<SDL_sensor.h>",
            "<SDL_video.h>",
            "<SDL_clipboard.h>",
            "<SDL_timer.h>",
            "<SDL_stdinc.h>",
            "<SDL_version.h>",
        )
    )
    offenders: list[str] = []
    for base in (ROOT / "components/sdlutil", ROOT / "apps/openmw"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".cpp", ".hpp", ".h"}:
                continue
            text = path.read_text(encoding="utf-8")
            if any(token in text for token in forbidden):
                offenders.append(str(path.relative_to(ROOT)))
    if offenders:
        raise RuntimeError("legacy SDL2-style includes remain: " + ", ".join(offenders))


def main() -> None:
    root_changed = migrate_root_cmake()
    link_changed = migrate_component_link()
    include_count = migrate_sdl_includes()
    verify_no_legacy_headers()
    print(
        "CP1B SDL3 materialization complete: "
        f"root_cmake_changed={int(root_changed)} "
        f"component_link_changed={int(link_changed)} "
        f"source_files_with_include_updates={include_count}"
    )


if __name__ == "__main__":
    main()
