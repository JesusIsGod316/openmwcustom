#!/usr/bin/env python3
# V4 CP1B: finish SDL3 cursor filtering and display-enumeration semantics,
# then fail closed on SDL2 video APIs that SDL3 removed outright.

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    path = ROOT / rel
    old = path.read_text(encoding="utf-8")
    if old != text:
        path.write_text(text, encoding="utf-8")


def port_cursor_scaling() -> None:
    rel = "components/sdlutil/sdlcursormanager.cpp"
    text = read(rel)

    # SDL3 removed SDL_HINT_RENDER_SCALE_QUALITY. SDL3 textures default to
    # linear filtering, but keep the CP1B parity intent explicit on this cursor
    # texture and fail if SDL rejects the requested scale mode.
    text = text.replace('        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");\n', "")

    old = '''        if (!cursorTexture)\n            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));\n\n        if (!SDL_RenderTextureRotated(\n'''
    new = '''        if (!cursorTexture)\n            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));\n        if (!SDL_SetTextureScaleMode(cursorTexture.get(), SDL_SCALEMODE_LINEAR))\n            throw std::runtime_error("Failed to set SDL3 cursor texture scale mode: " + std::string(SDL_GetError()));\n\n        if (!SDL_RenderTextureRotated(\n'''
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: cursor texture creation block not found")

    write(rel, text)


def port_launcher_displays() -> None:
    rel = "apps/launcher/graphicspage.cpp"
    text = read(rel)

    old_setup = '''bool Launcher::GraphicsPage::setupSDL()\n{\n    bool sdlConnectSuccessful = initSDL();\n    if (!sdlConnectSuccessful)\n    {\n        return false;\n    }\n\n    int displays = SDL_GetNumVideoDisplays();\n\n    if (displays < 0)\n    {\n        QMessageBox msgBox;\n        msgBox.setWindowTitle(tr("Error receiving number of screens"));\n        msgBox.setIcon(QMessageBox::Critical);\n        msgBox.setStandardButtons(QMessageBox::Ok);\n        msgBox.setText(\n            tr("<br><b>SDL_GetNumVideoDisplays failed:</b><br><br>") + QString::fromUtf8(SDL_GetError()) + "<br>");\n        msgBox.exec();\n        return false;\n    }\n\n    screenComboBox->clear();\n    mResolutionsPerScreen.clear();\n    for (int i = 0; i < displays; i++)\n    {\n        mResolutionsPerScreen.append(getAvailableResolutions(i));\n        screenComboBox->addItem(QString(tr("Screen ")) + QString::number(i + 1));\n    }\n    screenChanged(0);\n\n    // Disconnect from SDL processes\n    quitSDL();\n\n    return true;\n}\n'''
    new_setup = '''bool Launcher::GraphicsPage::setupSDL()\n{\n    const bool sdlConnectSuccessful = initSDL();\n    if (!sdlConnectSuccessful)\n        return false;\n\n    int displayCount = 0;\n    SDL_DisplayID* displayIds = SDL_GetDisplays(&displayCount);\n    if (!displayIds || displayCount <= 0)\n    {\n        QMessageBox msgBox;\n        msgBox.setWindowTitle(tr("Error receiving number of screens"));\n        msgBox.setIcon(QMessageBox::Critical);\n        msgBox.setStandardButtons(QMessageBox::Ok);\n        msgBox.setText(\n            tr("<br><b>SDL_GetDisplays failed:</b><br><br>") + QString::fromUtf8(SDL_GetError()) + "<br>");\n        msgBox.exec();\n        SDL_free(displayIds);\n        quitSDL();\n        return false;\n    }\n    SDL_free(displayIds);\n\n    screenComboBox->clear();\n    mResolutionsPerScreen.clear();\n    for (int i = 0; i < displayCount; ++i)\n    {\n        mResolutionsPerScreen.append(getAvailableResolutions(i));\n        screenComboBox->addItem(QString(tr("Screen ")) + QString::number(i + 1));\n    }\n    screenChanged(0);\n\n    // Disconnect from SDL processes\n    quitSDL();\n\n    return true;\n}\n'''
    if old_setup in text:
        text = text.replace(old_setup, new_setup, 1)
    elif new_setup not in text:
        raise RuntimeError(f"{rel}: SDL display enumeration block not found")

    start = text.find("QStringList Launcher::GraphicsPage::getAvailableResolutions(int screen)\n{")
    end_marker = "\nQRect Launcher::GraphicsPage::getMaximumResolution()"
    end = text.find(end_marker, start)
    if start < 0 or end < 0:
        raise RuntimeError(f"{rel}: getAvailableResolutions function boundary not found")

    new_resolutions = '''QStringList Launcher::GraphicsPage::getAvailableResolutions(int screen)\n{\n    QStringList result;\n\n    int displayCount = 0;\n    SDL_DisplayID* displayIds = SDL_GetDisplays(&displayCount);\n    if (!displayIds || screen < 0 || screen >= displayCount)\n    {\n        QMessageBox msgBox;\n        msgBox.setWindowTitle(tr("Error receiving resolutions"));\n        msgBox.setIcon(QMessageBox::Critical);\n        msgBox.setStandardButtons(QMessageBox::Ok);\n        msgBox.setText(\n            tr("<br><b>SDL_GetDisplays failed:</b><br><br>") + QString::fromUtf8(SDL_GetError()) + "<br>");\n        msgBox.exec();\n        SDL_free(displayIds);\n        return result;\n    }\n\n    const SDL_DisplayID displayId = displayIds[screen];\n    SDL_free(displayIds);\n\n    int modeCount = 0;\n    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(displayId, &modeCount);\n    if (!modes)\n    {\n        QMessageBox msgBox;\n        msgBox.setWindowTitle(tr("Error receiving resolutions"));\n        msgBox.setIcon(QMessageBox::Critical);\n        msgBox.setStandardButtons(QMessageBox::Ok);\n        msgBox.setText(tr("<br><b>SDL_GetFullscreenDisplayModes failed:</b><br><br>")\n            + QString::fromUtf8(SDL_GetError()) + "<br>");\n        msgBox.exec();\n        return result;\n    }\n\n    for (int modeIndex = 0; modeIndex < modeCount; ++modeIndex)\n    {\n        const SDL_DisplayMode* mode = modes[modeIndex];\n        if (!mode)\n            continue;\n        auto str = Misc::getResolutionText(mode->w, mode->h);\n        result.append(QString(str.c_str()));\n    }\n    SDL_free(modes);\n\n    result.removeDuplicates();\n    return result;\n}\n'''

    existing = text[start:end]
    if existing != new_resolutions:
        text = text[:start] + new_resolutions + text[end:]

    write(rel, text)


def port_ingame_display_modes() -> None:
    rel = "apps/openmw/mwgui/settingswindow.cpp"
    text = read(rel)

    old = '''        // fill resolution list\n        const int screen = Settings::video().mScreen;\n        int numDisplayModes = SDL_GetNumDisplayModes(screen);\n        std::vector<std::pair<int, int>> resolutions;\n        for (int i = 0; i < numDisplayModes; i++)\n        {\n            SDL_DisplayMode mode;\n            SDL_GetDisplayMode(screen, i, &mode);\n            resolutions.emplace_back(mode.w, mode.h);\n        }\n        std::sort(resolutions.begin(), resolutions.end(), sortResolutions);\n'''
    new = '''        // fill resolution list\n        const int screen = Settings::video().mScreen;\n        std::vector<std::pair<int, int>> resolutions;\n\n        int displayCount = 0;\n        SDL_DisplayID* displayIds = SDL_GetDisplays(&displayCount);\n        if (displayIds && screen >= 0 && screen < displayCount)\n        {\n            const SDL_DisplayID displayId = displayIds[screen];\n            int modeCount = 0;\n            SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(displayId, &modeCount);\n            if (modes)\n            {\n                for (int i = 0; i < modeCount; ++i)\n                {\n                    const SDL_DisplayMode* mode = modes[i];\n                    if (mode)\n                        resolutions.emplace_back(mode->w, mode->h);\n                }\n                SDL_free(modes);\n            }\n            else\n                Log(Debug::Warning) << "SDL_GetFullscreenDisplayModes failed: " << SDL_GetError();\n        }\n        else\n            Log(Debug::Warning) << "SDL_GetDisplays failed or configured screen is unavailable: " << SDL_GetError();\n        SDL_free(displayIds);\n\n        std::sort(resolutions.begin(), resolutions.end(), sortResolutions);\n'''

    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: in-game resolution enumeration block not found")

    write(rel, text)


def verify_removed_video_contract() -> None:
    # These APIs are explicitly removed by SDL3 rather than simple renames.
    # Search invocation spellings so comments mentioning an API do not trip the
    # gate. Any real hit must be ported before Windows compilation is allowed.
    forbidden = {
        "SDL_HINT_RENDER_SCALE_QUALITY": "removed render-scale-quality hint",
        "SDL_GetDisplayDPI(": "removed display DPI API",
        "SDL_GetDisplayMode(": "removed indexed display-mode API",
        "SDL_GetNumDisplayModes(": "removed indexed display-mode count API",
        "SDL_GetNumVideoDisplays(": "removed display-count API",
        "SDL_SetWindowGrab(": "removed combined window-grab API",
        "SDL_GetWindowGrab(": "removed combined window-grab API",
        "SDL_GetWindowData(": "removed window-data API",
        "SDL_SetWindowData(": "removed window-data API",
        "SDL_CreateWindowFrom(": "removed foreign-window constructor",
        "SDL_SetWindowInputFocus(": "removed input-focus API",
        "SDL_SetWindowModalFor(": "removed modal-parent API",
        "SDL_SetWindowBrightness(": "removed window-brightness API",
        "SDL_GetWindowBrightness(": "removed window-brightness API",
        "SDL_SetWindowGammaRamp(": "removed SDL3 gamma-ramp API",
        "SDL_GetWindowGammaRamp(": "removed SDL3 gamma-ramp API",
    }

    offenders: list[str] = []
    for base in (ROOT / "apps", ROOT / "components", ROOT / "extern" / "oics"):
        for path in sorted(base.rglob("*")):
            if path.suffix not in {".cpp", ".hpp", ".h", ".c", ".m", ".mm"}:
                continue
            text = path.read_text(encoding="utf-8")
            for token, reason in forbidden.items():
                if token in text:
                    offenders.append(f"{path.relative_to(ROOT)}: {reason}: {token}")

    if offenders:
        raise RuntimeError("CP1B SDL3 removed-video API audit failed:\n" + "\n".join(offenders))


def main() -> None:
    port_cursor_scaling()
    port_launcher_displays()
    port_ingame_display_modes()
    verify_removed_video_contract()
    print("CP1B SDL3 cursor/display semantic port complete; removed-video API audit clean.")


if __name__ == "__main__":
    main()
