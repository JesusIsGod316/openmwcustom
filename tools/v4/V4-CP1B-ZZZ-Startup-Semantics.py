#!/usr/bin/env python3
"""Finish CP1B SDL3 application-startup semantics and fail closed on removed SDL2 startup tokens.

This pass intentionally runs last in the CP1B materializer stack. It fixes the
SDL3 header-ownership change for SDL_SetMainReady(), removes the SDL2
accelerometer-as-joystick hint (SDL3 no longer exposes accelerometers as
joysticks), and audits source for the SDL2 hints/init flags that SDL 3.4.10
explicitly removed.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
ENGINE = ROOT / "apps/openmw/engine.cpp"

SOURCE_ROOTS = (
    ROOT / "apps",
    ROOT / "components",
    ROOT / "extern/oics",
    ROOT / "extern/osg-ffmpeg-videoplayer",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".m", ".mm"}

# SDL 3.4.10 docs/README-migration.md, SDL_hints.h: symbols renamed from SDL2.
RENAMED_HINTS = {
    "SDL_HINT_ALLOW_TOPMOST": "SDL_HINT_WINDOW_ALLOW_TOPMOST",
    "SDL_HINT_AUDIODRIVER": "SDL_HINT_AUDIO_DRIVER",
    "SDL_HINT_DIRECTINPUT_ENABLED": "SDL_HINT_JOYSTICK_DIRECTINPUT",
    "SDL_HINT_GDK_TEXTINPUT_DEFAULT": "SDL_HINT_GDK_TEXTINPUT_DEFAULT_TEXT",
    "SDL_HINT_JOYSTICK_GAMECUBE_RUMBLE_BRAKE": "SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE_RUMBLE_BRAKE",
    "SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE": "SDL_HINT_JOYSTICK_ENHANCED_REPORTS",
    "SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE": "SDL_HINT_JOYSTICK_ENHANCED_REPORTS",
    "SDL_HINT_LINUX_DIGITAL_HATS": "SDL_HINT_JOYSTICK_LINUX_DIGITAL_HATS",
    "SDL_HINT_LINUX_HAT_DEADZONES": "SDL_HINT_JOYSTICK_LINUX_HAT_DEADZONES",
    "SDL_HINT_LINUX_JOYSTICK_CLASSIC": "SDL_HINT_JOYSTICK_LINUX_CLASSIC",
    "SDL_HINT_LINUX_JOYSTICK_DEADZONES": "SDL_HINT_JOYSTICK_LINUX_DEADZONES",
    "SDL_HINT_VIDEODRIVER": "SDL_HINT_VIDEO_DRIVER",
    "SDL_HINT_VIDEO_WAYLAND_EMULATE_MOUSE_WARP": "SDL_HINT_MOUSE_EMULATE_WARP_WITH_RELATIVE",
}

# SDL 3.4.10 docs/README-migration.md, SDL_hints.h: symbols removed from SDL3.
REMOVED_HINTS = {
    "SDL_HINT_ACCELEROMETER_AS_JOYSTICK",
    "SDL_HINT_ANDROID_BLOCK_ON_PAUSE_PAUSEAUDIO",
    "SDL_HINT_AUDIO_DEVICE_APP_NAME",
    "SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS",
    "SDL_HINT_GRAB_KEYBOARD",
    "SDL_HINT_IDLE_TIMER_DISABLED",
    "SDL_HINT_IME_INTERNAL_EDITING",
    "SDL_HINT_IME_SHOW_UI",
    "SDL_HINT_IME_SUPPORT_EXTENDED_TEXT",
    "SDL_HINT_MOUSE_RELATIVE_MODE_WARP",
    "SDL_HINT_MOUSE_RELATIVE_SCALING",
    "SDL_HINT_PS2_DYNAMIC_VSYNC",
    "SDL_HINT_RENDER_BATCHING",
    "SDL_HINT_RENDER_LOGICAL_SIZE_MODE",
    "SDL_HINT_RENDER_OPENGL_SHADERS",
    "SDL_HINT_RENDER_SCALE_QUALITY",
    "SDL_HINT_THREAD_STACK_SIZE",
    "SDL_HINT_VIDEO_EXTERNAL_CONTEXT",
    "SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL",
    "SDL_HINT_VIDEO_FOREIGN_WINDOW_VULKAN",
    "SDL_HINT_VIDEO_HIGHDPI_DISABLED",
    "SDL_HINT_VIDEO_WINDOW_SHARE_PIXEL_FORMAT",
    "SDL_HINT_VIDEO_X11_FORCE_EGL",
    "SDL_HINT_VIDEO_X11_XINERAMA",
    "SDL_HINT_VIDEO_X11_XVIDMODE",
    "SDL_HINT_WINDOWS_DISABLE_THREAD_NAMING",
    "SDL_HINT_WINDOWS_FORCE_MUTEX_CRITICAL_SECTIONS",
    "SDL_HINT_WINDOWS_NO_CLOSE_ON_ALT_F4",
    "SDL_HINT_WINRT_HANDLE_BACK_BUTTON",
    "SDL_HINT_WINRT_PRIVACY_POLICY_LABEL",
    "SDL_HINT_WINRT_PRIVACY_POLICY_URL",
    "SDL_HINT_XINPUT_USE_OLD_JOYSTICK_MAPPING",
}

# SDL 3.4.10 docs/README-migration.md, SDL_init.h.
REMOVED_INIT_SYMBOLS = {
    "SDL_INIT_NOPARACHUTE",
    "SDL_INIT_EVERYTHING",
    "SDL_INIT_TIMER",
}

IDENT = re.compile(r"\bSDL_[A-Za-z0-9_]+\b")
SDL3_INCLUDE = re.compile(r"^#include\s+[<\"]SDL3/SDL[^>\"]*[>\"]\s*$", re.MULTILINE)


def replace_identifier(text: str, old: str, new: str) -> str:
    return re.sub(rf"(?<![A-Za-z0-9_]){re.escape(old)}(?![A-Za-z0-9_])", new, text)


def iter_sources():
    for base in SOURCE_ROOTS:
        if not base.exists():
            continue
        for path in base.rglob("*"):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def ensure_mainready_header(path: pathlib.Path) -> None:
    text = path.read_text(encoding="utf-8", errors="replace")
    if "SDL_SetMainReady" not in text or "#include <SDL3/SDL_main.h>" in text:
        return

    match = SDL3_INCLUDE.search(text)
    if not match:
        raise RuntimeError(
            f"{path.relative_to(ROOT)} uses SDL_SetMainReady but has no SDL3 include anchor"
        )

    insert_at = match.end()
    text = text[:insert_at] + "\n#include <SDL3/SDL_main.h>" + text[insert_at:]
    path.write_text(text, encoding="utf-8", newline="\n")


def patch_engine() -> None:
    text = ENGINE.read_text(encoding="utf-8")
    original = text

    # This SDL2 hint was removed in SDL3. Accelerometer sensor access is now a
    # gamepad-sensor API and is not exposed as a synthetic joystick, matching
    # the intent of the old OpenMW setting ("We use only gamepads").
    text = re.sub(
        r"^[ \t]*SDL_SetHint\(SDL_HINT_ACCELEROMETER_AS_JOYSTICK,[^\n]*\);[^\n]*\n",
        "",
        text,
        flags=re.MULTILINE,
    )

    # CP1B pins SDL 3.4.10, where this macOS OpenGL hint is native. The inherited
    # SDL2 version guard is dead compatibility syntax and can hide header/API
    # ownership mistakes.
    old_mac_block = (
        "#if SDL_VERSION_ATLEAST(2, 24, 0)\n"
        "    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, \"1\");\n"
        "#endif\n"
    )
    if old_mac_block in text:
        text = text.replace(
            old_mac_block,
            "    SDL_SetHint(SDL_HINT_MAC_OPENGL_ASYNC_DISPATCH, \"1\");\n",
            1,
        )

    # Apply only SDL's documented one-to-one hint renames. Removed hints are
    # never auto-translated because their replacements can require new behavior.
    for old, new in RENAMED_HINTS.items():
        text = replace_identifier(text, old, new)

    if text != original:
        ENGINE.write_text(text, encoding="utf-8", newline="\n")


def patch_mainready_headers() -> None:
    # SDL3 stopped including SDL_main.h from SDL.h. Apply the ownership fix to
    # every current callsite, not only the first compiler failure.
    for path in iter_sources():
        ensure_mainready_header(path)


def audit() -> None:
    failures: list[str] = []

    for path in iter_sources():
        text = path.read_text(encoding="utf-8", errors="replace")
        tokens = set(IDENT.findall(text))
        rel = path.relative_to(ROOT)

        for token in sorted(tokens & REMOVED_HINTS):
            failures.append(f"{rel}: removed SDL3 hint remains: {token}")
        for token in sorted(tokens & set(RENAMED_HINTS)):
            failures.append(
                f"{rel}: SDL2 hint spelling remains: {token} -> {RENAMED_HINTS[token]}"
            )
        for token in sorted(tokens & REMOVED_INIT_SYMBOLS):
            failures.append(f"{rel}: removed SDL3 init symbol remains: {token}")

        if "SDL_SetMainReady" in text and "#include <SDL3/SDL_main.h>" not in text:
            failures.append(f"{rel}: SDL_SetMainReady requires explicit <SDL3/SDL_main.h>")

    engine_text = ENGINE.read_text(encoding="utf-8")
    if "SDL_VERSION_ATLEAST(2," in engine_text:
        failures.append("apps/openmw/engine.cpp: inherited SDL2 version gate remains in SDL3 startup path")
    if engine_text.count("#include <SDL3/SDL_main.h>") != 1:
        failures.append("apps/openmw/engine.cpp: expected exactly one <SDL3/SDL_main.h> include")
    if "SDL_SetMainReady();" not in engine_text:
        failures.append("apps/openmw/engine.cpp: SDL_MAIN_HANDLED startup lost SDL_SetMainReady()")

    if failures:
        print("CP1B SDL3 startup semantic audit FAILED:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        raise SystemExit(1)

    print("CP1B SDL3 startup semantic audit PASS")


def main() -> None:
    patch_engine()
    patch_mainready_headers()
    audit()


if __name__ == "__main__":
    main()
