#!/usr/bin/env python3
# V4 CP1B: preserve the V3.25/OpenGL input contract across SDL3's float/bool API changes.
#
# SDL3 changed mouse-wheel and relative-mouse coordinates to floating point and
# SDL_GetGamepadButton() to bool.  OpenMW's existing Lua wheel contract and
# relative-mouse getters are integer-facing.  Keep those public/gameplay
# semantics intact by projecting SDL3 values explicitly at the boundary rather
# than widening OpenMW interfaces during the parity checkpoint.

from __future__ import annotations

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    path = ROOT / rel
    old = path.read_text(encoding="utf-8")
    if old != text:
        path.write_text(text, encoding="utf-8")


def replace_once_or_already(rel: str, old: str, new: str) -> None:
    text = read(rel)
    count = text.count(old)
    if count == 1:
        write(rel, text.replace(old, new, 1))
        return
    if count == 0 and new in text:
        return
    raise RuntimeError(f"{rel}: expected one legacy pattern or already-migrated form; found {count}: {old!r}")


def port_mouse_manager() -> None:
    rel = "apps/openmw/mwinput/mousemanager.cpp"

    # SDL2 exposed integer wheel deltas and OpenMW's Lua InputEvent::WheelChange
    # is intentionally int/int. SDL3 exposes float wheel deltas. Preserve the
    # established Lua-facing contract explicitly instead of changing script API
    # semantics during CP1B.
    replace_once_or_already(
        rel,
        "MWBase::LuaManager::InputEvent::WheelChange{ arg.x, arg.y }",
        "MWBase::LuaManager::InputEvent::WheelChange{ static_cast<int>(arg.x), static_cast<int>(arg.y) }",
    )

    # SDL3 SDL_GetRelativeMouseState() writes float coordinates. Existing
    # OpenMW consumers/getters are integer-facing, so capture in SDL3-native
    # temporaries and explicitly quantize at the same boundary.
    replace_once_or_already(
        rel,
        "        SDL_GetRelativeMouseState(&mMouseMoveX, &mMouseMoveY);\n",
        "        float relativeMouseX = 0.f;\n"
        "        float relativeMouseY = 0.f;\n"
        "        SDL_GetRelativeMouseState(&relativeMouseX, &relativeMouseY);\n"
        "        mMouseMoveX = static_cast<int>(relativeMouseX);\n"
        "        mMouseMoveY = static_cast<int>(relativeMouseY);\n",
    )


def port_gamepad_button_bool() -> None:
    rel = "apps/openmw/mwinput/controllermanager.cpp"
    replace_once_or_already(
        rel,
        "            return SDL_GetGamepadButton(cntrl, button) > 0;",
        "            return SDL_GetGamepadButton(cntrl, button);",
    )


def clean_input_wrapper_types() -> None:
    rel = "components/sdlutil/sdlinputwrapper.cpp"
    replace_once_or_already(
        rel,
        "        Uint32 flags = SDL_GetWindowFlags(mSDLWindow);",
        "        const SDL_WindowFlags flags = SDL_GetWindowFlags(mSDLWindow);",
    )


def verify_contract() -> None:
    forbidden = {
        "apps/openmw/mwinput/mousemanager.cpp": (
            "WheelChange{ arg.x, arg.y }",
            "SDL_GetRelativeMouseState(&mMouseMoveX, &mMouseMoveY)",
        ),
        "apps/openmw/mwinput/controllermanager.cpp": (
            "SDL_GetGamepadButton(cntrl, button) > 0",
        ),
        "components/sdlutil/sdlinputwrapper.cpp": (
            "Uint32 flags = SDL_GetWindowFlags(mSDLWindow)",
        ),
    }

    offenders: list[str] = []
    for rel, tokens in forbidden.items():
        text = read(rel)
        for token in tokens:
            if token in text:
                offenders.append(f"{rel}: unresolved SDL3 value-semantic boundary: {token}")

    # Fail closed on direct SDL3 relative-mouse writes into integral variables
    # anywhere in the game/app source. This catches recurrence without banning
    # valid float-pointer uses.
    relative_state_re = re.compile(
        r"SDL_GetRelativeMouseState\s*\(\s*&m[A-Za-z0-9_]+\s*,\s*&m[A-Za-z0-9_]+\s*\)"
    )
    for base in (ROOT / "apps", ROOT / "components", ROOT / "extern" / "oics"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".cpp", ".hpp", ".h", ".c"}:
                continue
            text = path.read_text(encoding="utf-8")
            if relative_state_re.search(text):
                offenders.append(
                    f"{path.relative_to(ROOT)}: direct member-pointer SDL_GetRelativeMouseState call requires type audit"
                )

    if offenders:
        raise RuntimeError("CP1B SDL3 mouse/input value-semantics audit failed:\n" + "\n".join(offenders))


def main() -> None:
    port_mouse_manager()
    port_gamepad_button_bool()
    clean_input_wrapper_types()
    verify_contract()
    print("CP1B SDL3 mouse/input value-semantic port complete; parity contract audit clean.")


if __name__ == "__main__":
    main()
