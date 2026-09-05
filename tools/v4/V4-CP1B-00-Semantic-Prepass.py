#!/usr/bin/env python3
# CP1B pre-pass: repair migration-audit false positives and semantic stragglers
# that must be corrected before the main SDL3 materializer can validate the tree.

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MIGRATION = ROOT / "tools/v4/V4-CP1B-Migrate-SDL3.py"


def rewrite(path: Path, transforms: list[tuple[str, str]]) -> None:
    text = path.read_text(encoding="utf-8")
    updated = text
    for old, new in transforms:
        updated = updated.replace(old, new)
    if updated != text:
        path.write_text(updated, encoding="utf-8")


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

# The later cursor semantic pass replaces SDL2's removed global render-scale
# hint with SDL3's per-texture scale mode. Fold that final form into the main
# migration's own accepted output so a second materializer pass is truly
# idempotent instead of rejecting the already-correct cursor block.
rewrite(
    MIGRATION,
    [
        (
            '''        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cursorTexture(
            SDL_CreateTextureFromSurface(renderer.get(), cursorSurface.get()), SDL_DestroyTexture);
        if (!cursorTexture)
            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));

        if (!SDL_RenderTextureRotated(
''',
            '''        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cursorTexture(
            SDL_CreateTextureFromSurface(renderer.get(), cursorSurface.get()), SDL_DestroyTexture);
        if (!cursorTexture)
            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));
        if (!SDL_SetTextureScaleMode(cursorTexture.get(), SDL_SCALEMODE_LINEAR))
            throw std::runtime_error("Failed to set SDL3 cursor texture scale mode: " + std::string(SDL_GetError()));

        if (!SDL_RenderTextureRotated(
''',
        ),
    ],
)

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
