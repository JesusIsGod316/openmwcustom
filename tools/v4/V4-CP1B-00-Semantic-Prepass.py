#!/usr/bin/env python3
# CP1B pre-pass: repair migration-audit false positives and semantic stragglers
# that must be corrected before the main SDL3 materializer can validate the tree.

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIGRATION = ROOT / "tools/v4/V4-CP1B-Migrate-SDL3.py"
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".m", ".mm"}


def rewrite(path: Path, transforms: list[tuple[str, str]]) -> None:
    text = path.read_text(encoding="utf-8")
    updated = text
    for old, new in transforms:
        updated = updated.replace(old, new)
    if updated != text:
        path.write_text(updated, encoding="utf-8")


def rewrite_identifiers(path: Path, transforms: list[tuple[str, str]]) -> None:
    """Replace complete C/C++ identifiers only, never identifier prefixes."""
    text = path.read_text(encoding="utf-8")
    updated = text
    for old, new in transforms:
        updated = re.sub(
            rf"(?<![A-Za-z0-9_]){re.escape(old)}(?![A-Za-z0-9_])",
            new,
            updated,
        )
    if updated != text:
        path.write_text(updated, encoding="utf-8")


def iter_sources():
    for base in (ROOT / "apps", ROOT / "components", ROOT / "extern" / "oics"):
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


# The first broad audit deliberately failed closed, but two checks were too
# lexical: "nWindow" matched legitimate names such as mainWindow, and the
# removed global relative-mouse API was present only in a comment after the
# real call had already moved to SDL_SetWindowRelativeMouseMode(). Keep the
# audit strict, but make these checks invocation/identifier accurate.
rewrite(
    MIGRATION,
    [
        (
            '        "SDL_SetRelativeMouseMode": "SDL2 global relative-mouse API",\n',
            '        "SDL_SetRelativeMouseMode(": "SDL2 global relative-mouse API",\n',
        ),
        ('        "nWindow": "invalid CP1B window identifier",\n', ''),
    ],
)

# SDL3 renamed the game-controller API to gamepad, but the enum migration is
# not a mechanical prefix swap. Face buttons became positional names,
# stick/shoulder names gained separators, paddles were remapped by hand, and
# trigger axes changed word order. Keep every mapping token-exact: a previous
# substring replacement of SDL_GAMEPAD_BUTTON_B also matched the valid
# SDL_GAMEPAD_BUTTON_BACK token and produced SDL_GAMEPAD_BUTTON_EASTACK.
old_gamepad_constants = {
    "SDL_CONTROLLER_BUTTON_INVALID": "SDL_GAMEPAD_BUTTON_INVALID",
    "SDL_CONTROLLER_BUTTON_A": "SDL_GAMEPAD_BUTTON_SOUTH",
    "SDL_CONTROLLER_BUTTON_B": "SDL_GAMEPAD_BUTTON_EAST",
    "SDL_CONTROLLER_BUTTON_X": "SDL_GAMEPAD_BUTTON_WEST",
    "SDL_CONTROLLER_BUTTON_Y": "SDL_GAMEPAD_BUTTON_NORTH",
    "SDL_CONTROLLER_BUTTON_BACK": "SDL_GAMEPAD_BUTTON_BACK",
    "SDL_CONTROLLER_BUTTON_GUIDE": "SDL_GAMEPAD_BUTTON_GUIDE",
    "SDL_CONTROLLER_BUTTON_START": "SDL_GAMEPAD_BUTTON_START",
    "SDL_CONTROLLER_BUTTON_LEFTSTICK": "SDL_GAMEPAD_BUTTON_LEFT_STICK",
    "SDL_CONTROLLER_BUTTON_RIGHTSTICK": "SDL_GAMEPAD_BUTTON_RIGHT_STICK",
    "SDL_CONTROLLER_BUTTON_LEFTSHOULDER": "SDL_GAMEPAD_BUTTON_LEFT_SHOULDER",
    "SDL_CONTROLLER_BUTTON_RIGHTSHOULDER": "SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER",
    "SDL_CONTROLLER_BUTTON_DPAD_UP": "SDL_GAMEPAD_BUTTON_DPAD_UP",
    "SDL_CONTROLLER_BUTTON_DPAD_DOWN": "SDL_GAMEPAD_BUTTON_DPAD_DOWN",
    "SDL_CONTROLLER_BUTTON_DPAD_LEFT": "SDL_GAMEPAD_BUTTON_DPAD_LEFT",
    "SDL_CONTROLLER_BUTTON_DPAD_RIGHT": "SDL_GAMEPAD_BUTTON_DPAD_RIGHT",
    "SDL_CONTROLLER_BUTTON_MISC1": "SDL_GAMEPAD_BUTTON_MISC1",
    "SDL_CONTROLLER_BUTTON_PADDLE1": "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1",
    "SDL_CONTROLLER_BUTTON_PADDLE2": "SDL_GAMEPAD_BUTTON_LEFT_PADDLE1",
    "SDL_CONTROLLER_BUTTON_PADDLE3": "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2",
    "SDL_CONTROLLER_BUTTON_PADDLE4": "SDL_GAMEPAD_BUTTON_LEFT_PADDLE2",
    "SDL_CONTROLLER_BUTTON_TOUCHPAD": "SDL_GAMEPAD_BUTTON_TOUCHPAD",
    "SDL_CONTROLLER_BUTTON_MAX": "SDL_GAMEPAD_BUTTON_COUNT",
    "SDL_CONTROLLER_AXIS_INVALID": "SDL_GAMEPAD_AXIS_INVALID",
    "SDL_CONTROLLER_AXIS_LEFTX": "SDL_GAMEPAD_AXIS_LEFTX",
    "SDL_CONTROLLER_AXIS_LEFTY": "SDL_GAMEPAD_AXIS_LEFTY",
    "SDL_CONTROLLER_AXIS_RIGHTX": "SDL_GAMEPAD_AXIS_RIGHTX",
    "SDL_CONTROLLER_AXIS_RIGHTY": "SDL_GAMEPAD_AXIS_RIGHTY",
    "SDL_CONTROLLER_AXIS_TRIGGERLEFT": "SDL_GAMEPAD_AXIS_LEFT_TRIGGER",
    "SDL_CONTROLLER_AXIS_TRIGGERRIGHT": "SDL_GAMEPAD_AXIS_RIGHT_TRIGGER",
    "SDL_CONTROLLER_AXIS_MAX": "SDL_GAMEPAD_AXIS_COUNT",
}

invalid_generated_gamepad_constants = {
    "SDL_GAMEPAD_BUTTON_A": "SDL_GAMEPAD_BUTTON_SOUTH",
    "SDL_GAMEPAD_BUTTON_B": "SDL_GAMEPAD_BUTTON_EAST",
    "SDL_GAMEPAD_BUTTON_X": "SDL_GAMEPAD_BUTTON_WEST",
    "SDL_GAMEPAD_BUTTON_Y": "SDL_GAMEPAD_BUTTON_NORTH",
    "SDL_GAMEPAD_BUTTON_EASTACK": "SDL_GAMEPAD_BUTTON_BACK",
    "SDL_GAMEPAD_BUTTON_LEFTSTICK": "SDL_GAMEPAD_BUTTON_LEFT_STICK",
    "SDL_GAMEPAD_BUTTON_RIGHTSTICK": "SDL_GAMEPAD_BUTTON_RIGHT_STICK",
    "SDL_GAMEPAD_BUTTON_LEFTSHOULDER": "SDL_GAMEPAD_BUTTON_LEFT_SHOULDER",
    "SDL_GAMEPAD_BUTTON_RIGHTSHOULDER": "SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER",
    "SDL_GAMEPAD_BUTTON_PADDLE1": "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1",
    "SDL_GAMEPAD_BUTTON_PADDLE2": "SDL_GAMEPAD_BUTTON_LEFT_PADDLE1",
    "SDL_GAMEPAD_BUTTON_PADDLE3": "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2",
    "SDL_GAMEPAD_BUTTON_PADDLE4": "SDL_GAMEPAD_BUTTON_LEFT_PADDLE2",
    "SDL_GAMEPAD_BUTTON_MAX": "SDL_GAMEPAD_BUTTON_COUNT",
    "SDL_GAMEPAD_AXIS_TRIGGERLEFT": "SDL_GAMEPAD_AXIS_LEFT_TRIGGER",
    "SDL_GAMEPAD_AXIS_TRIGGERRIGHT": "SDL_GAMEPAD_AXIS_RIGHT_TRIGGER",
    "SDL_GAMEPAD_AXIS_MAX": "SDL_GAMEPAD_AXIS_COUNT",
}

source_gamepad_transforms = list(invalid_generated_gamepad_constants.items()) + list(old_gamepad_constants.items())
for source in iter_sources():
    rewrite_identifiers(source, source_gamepad_transforms)

migration_text = MIGRATION.read_text(encoding="utf-8")
constant_anchor = '        "SDL_GameControllerType": "SDL_GamepadType",\n'
if constant_anchor in migration_text and '        "SDL_CONTROLLER_BUTTON_A": "SDL_GAMEPAD_BUTTON_SOUTH",\n' not in migration_text:
    exact_lines = "".join(f'        "{old}": "{new}",\n' for old, new in old_gamepad_constants.items())
    migration_text = migration_text.replace(constant_anchor, constant_anchor + exact_lines, 1)

# Remove the incorrect prefix substitutions now that every SDL2 gamepad enum
# spelling has an exact native SDL3 mapping above.
migration_text = migration_text.replace(
    '        updated = re.sub(r"\\bSDL_CONTROLLER_BUTTON_", "SDL_GAMEPAD_BUTTON_", updated)\n', ""
)
migration_text = migration_text.replace(
    '        updated = re.sub(r"\\bSDL_CONTROLLER_AXIS_", "SDL_GAMEPAD_AXIS_", updated)\n', ""
)

# The broad simple-rename table contains C/C++ identifiers. Make that pass
# identifier-aware too, so shorter names can never mutate a longer valid name.
old_simple_loop = '''        for old, new in replacements.items():
            updated = updated.replace(old, new)
'''
new_simple_loop = '''        for old, new in replacements.items():
            updated = re.sub(
                rf"(?<![A-Za-z0-9_]){re.escape(old)}(?![A-Za-z0-9_])", new, updated
            )
'''
if old_simple_loop in migration_text:
    migration_text = migration_text.replace(old_simple_loop, new_simple_loop, 1)

# Strengthen the semantic audit so exact invalid identifiers do not trigger on
# valid longer identifiers (for example BUTTON_B inside BUTTON_BACK). Prefix
# checks ending in '_' and invocation checks ending in '(' remain substring
# checks by design.
old_audit_loop = '''        for token, reason in forbidden_tokens.items():
            if token in text:
                offenders.append(f"{path.relative_to(ROOT)}: {reason}: {token}")
'''
new_audit_loop = '''        for token, reason in forbidden_tokens.items():
            if token.endswith("_") or token.endswith("("):
                present = token in text
            else:
                present = re.search(
                    rf"(?<![A-Za-z0-9_]){re.escape(token)}(?![A-Za-z0-9_])", text
                ) is not None
            if present:
                offenders.append(f"{path.relative_to(ROOT)}: {reason}: {token}")
'''
if old_audit_loop in migration_text:
    migration_text = migration_text.replace(old_audit_loop, new_audit_loop, 1)

# Preserve the known corruption signature in the main fail-closed audit as an
# explicit regression guard.
forbidden_anchor = '        "SDL_UNKNOWN": "invalid SDL3 keycode token",\n'
if forbidden_anchor in migration_text and '        "SDL_CONTROLLER_BUTTON_": "SDL2 controller button enum remains",\n' not in migration_text:
    audit_lines = (
        '        "SDL_CONTROLLER_BUTTON_": "SDL2 controller button enum remains",\n'
        '        "SDL_CONTROLLER_AXIS_": "SDL2 controller axis enum remains",\n'
        '        "SDL_GAMEPAD_BUTTON_A": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_B": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_X": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_Y": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_LEFTSTICK": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_RIGHTSTICK": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_LEFTSHOULDER": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_RIGHTSHOULDER": "invalid mechanical SDL3 gamepad button spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_PADDLE": "invalid mechanical SDL3 gamepad paddle spelling",\n'
        '        "SDL_GAMEPAD_BUTTON_MAX": "invalid SDL3 gamepad button count spelling",\n'
        '        "SDL_GAMEPAD_AXIS_TRIGGER": "invalid mechanical SDL3 gamepad trigger spelling",\n'
        '        "SDL_GAMEPAD_AXIS_MAX": "invalid SDL3 gamepad axis count spelling",\n'
    )
    migration_text = migration_text.replace(forbidden_anchor, forbidden_anchor + audit_lines, 1)
if '        "SDL_GAMEPAD_BUTTON_EASTACK": "corrupted SDL3 BACK enum spelling",\n' not in migration_text:
    anchor = '        "SDL_GAMEPAD_BUTTON_B": "invalid mechanical SDL3 gamepad button spelling",\n'
    if anchor in migration_text:
        migration_text = migration_text.replace(
            anchor,
            anchor + '        "SDL_GAMEPAD_BUTTON_EASTACK": "corrupted SDL3 BACK enum spelling",\n',
            1,
        )
MIGRATION.write_text(migration_text, encoding="utf-8")

# Validate every gamepad button/axis spelling against SDL 3.4.10's public enum
# contract. This catches any future mechanical typo even if it was not already
# named in the migration audit.
allowed_gamepad_buttons = {
    "SDL_GAMEPAD_BUTTON_INVALID",
    "SDL_GAMEPAD_BUTTON_SOUTH",
    "SDL_GAMEPAD_BUTTON_EAST",
    "SDL_GAMEPAD_BUTTON_WEST",
    "SDL_GAMEPAD_BUTTON_NORTH",
    "SDL_GAMEPAD_BUTTON_BACK",
    "SDL_GAMEPAD_BUTTON_GUIDE",
    "SDL_GAMEPAD_BUTTON_START",
    "SDL_GAMEPAD_BUTTON_LEFT_STICK",
    "SDL_GAMEPAD_BUTTON_RIGHT_STICK",
    "SDL_GAMEPAD_BUTTON_LEFT_SHOULDER",
    "SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER",
    "SDL_GAMEPAD_BUTTON_DPAD_UP",
    "SDL_GAMEPAD_BUTTON_DPAD_DOWN",
    "SDL_GAMEPAD_BUTTON_DPAD_LEFT",
    "SDL_GAMEPAD_BUTTON_DPAD_RIGHT",
    "SDL_GAMEPAD_BUTTON_MISC1",
    "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1",
    "SDL_GAMEPAD_BUTTON_LEFT_PADDLE1",
    "SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2",
    "SDL_GAMEPAD_BUTTON_LEFT_PADDLE2",
    "SDL_GAMEPAD_BUTTON_TOUCHPAD",
    "SDL_GAMEPAD_BUTTON_MISC2",
    "SDL_GAMEPAD_BUTTON_MISC3",
    "SDL_GAMEPAD_BUTTON_MISC4",
    "SDL_GAMEPAD_BUTTON_MISC5",
    "SDL_GAMEPAD_BUTTON_MISC6",
    "SDL_GAMEPAD_BUTTON_COUNT",
}
allowed_gamepad_axes = {
    "SDL_GAMEPAD_AXIS_INVALID",
    "SDL_GAMEPAD_AXIS_LEFTX",
    "SDL_GAMEPAD_AXIS_LEFTY",
    "SDL_GAMEPAD_AXIS_RIGHTX",
    "SDL_GAMEPAD_AXIS_RIGHTY",
    "SDL_GAMEPAD_AXIS_LEFT_TRIGGER",
    "SDL_GAMEPAD_AXIS_RIGHT_TRIGGER",
    "SDL_GAMEPAD_AXIS_COUNT",
}
unknown_gamepad_tokens: list[str] = []
for source in iter_sources():
    text = source.read_text(encoding="utf-8")
    for token in sorted(set(re.findall(r"\bSDL_GAMEPAD_BUTTON_[A-Z0-9_]+\b", text))):
        if token not in allowed_gamepad_buttons:
            unknown_gamepad_tokens.append(f"{source.relative_to(ROOT)}: unknown SDL3 gamepad button {token}")
    for token in sorted(set(re.findall(r"\bSDL_GAMEPAD_AXIS_[A-Z0-9_]+\b", text))):
        if token not in allowed_gamepad_axes:
            unknown_gamepad_tokens.append(f"{source.relative_to(ROOT)}: unknown SDL3 gamepad axis {token}")
if unknown_gamepad_tokens:
    raise RuntimeError("CP1B SDL3 gamepad enum audit failed:\n" + "\n".join(unknown_gamepad_tokens))

# The later cursor semantic pass replaces SDL2's removed global render-scale
# hint with SDL3's per-texture scale mode. Normalize both the already-
# materialized source and the main migration's accepted output before the main
# migration executes. This makes the first pass and the idempotency re-pass
# converge on exactly the same SDL3 cursor block.
cursor_old = '''        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cursorTexture(
            SDL_CreateTextureFromSurface(renderer.get(), cursorSurface.get()), SDL_DestroyTexture);
        if (!cursorTexture)
            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));

        if (!SDL_RenderTextureRotated(
'''
cursor_new = '''        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cursorTexture(
            SDL_CreateTextureFromSurface(renderer.get(), cursorSurface.get()), SDL_DestroyTexture);
        if (!cursorTexture)
            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));
        if (!SDL_SetTextureScaleMode(cursorTexture.get(), SDL_SCALEMODE_LINEAR))
            throw std::runtime_error("Failed to set SDL3 cursor texture scale mode: " + std::string(SDL_GetError()));

        if (!SDL_RenderTextureRotated(
'''
rewrite(MIGRATION, [(cursor_old, cursor_new)])
rewrite(ROOT / "components/sdlutil/sdlcursormanager.cpp", [(cursor_old, cursor_new)])

# Lua UI synthesizes a keyboard event from a MyGUI key. SDL3 removed
# SDL_Keysym and SDL_GetScancodeFromKey now takes a modifier out-parameter.
widget = ROOT / "components/lua_ui/widget.cpp"
text = widget.read_text(encoding="utf-8")
if "#include <components/sdlutil/events.hpp>" not in text:
    text = text.replace(
        "#include <components/sdlutil/sdlmappings.hpp>",
        "#include <components/sdlutil/events.hpp>\n#include <components/sdlutil/sdlmappings.hpp>",
        1,
    )
text = text.replace("auto keySym = SDL_Keysym();", "auto keySym = SDLUtil::KeyEvent();")
text = text.replace(
    "keySym.scancode = SDL_GetScancodeFromKey(keySym.sym);",
    "SDL_Keymod keycodeMod = SDL_KMOD_NONE;\n        keySym.scancode = SDL_GetScancodeFromKey(keySym.sym, &keycodeMod);",
)
text = text.replace(
    "keySym.mod = static_cast<Uint16>(SDL_GetModState());",
    "keySym.mod = SDL_GetModState();",
)
widget.write_text(text, encoding="utf-8")

# The launcher had the same SDL2 integer-success convention as the game
# engine. SDL3 returns true on success.
launcher = ROOT / "apps/launcher/sdlinit.cpp"
text = launcher.read_text(encoding="utf-8")
text = text.replace("if (SDL_Init(SDL_INIT_VIDEO) != 0)", "if (!SDL_Init(SDL_INIT_VIDEO))")
launcher.write_text(text, encoding="utf-8")

print("CP1B semantic pre-pass complete.")
