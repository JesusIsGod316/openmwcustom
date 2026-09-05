#!/usr/bin/env python3
# V4 CP1B native SDL3 semantic migration and fail-closed source audit.

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SOURCE_ROOTS = (ROOT / "components", ROOT / "apps", ROOT / "extern" / "oics")
SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".c"}


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


def replace_once(rel: str, old: str, new: str) -> None:
    text = read(rel)
    count = text.count(old)
    if count == 1:
        write(rel, text.replace(old, new, 1))
        return
    if count == 0 and new in text:
        return
    raise RuntimeError(f"{rel}: expected exactly one source pattern, found {count}: {old[:120]!r}")


def normalize_sdl_includes() -> None:
    include_re = re.compile(r'^[ \t]*#include[ \t]+[<"]SDL(?:_[A-Za-z0-9_]+)?\.h[>"][ \t]*$', re.MULTILINE)
    for path in iter_sources():
        text = path.read_text(encoding="utf-8")
        updated = include_re.sub("#include <SDL3/SDL.h>", text)
        while "#include <SDL3/SDL.h>\n#include <SDL3/SDL.h>" in updated:
            updated = updated.replace(
                "#include <SDL3/SDL.h>\n#include <SDL3/SDL.h>",
                "#include <SDL3/SDL.h>",
            )
        if updated != text:
            path.write_text(updated, encoding="utf-8")


def port_image_to_surface() -> None:
    write(
        "components/sdlutil/imagetosurface.cpp",
        '''#include "imagetosurface.hpp"

#include <stdexcept>

#include <SDL3/SDL.h>
#include <osg/Image>

namespace SDLUtil
{

    SurfaceUniquePtr imageToSurface(osg::Image* image, bool flip)
    {
        const int width = image->s();
        const int height = image->t();
        const SDL_PixelFormat format
            = SDL_GetPixelFormatForMasks(32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
        if (format == SDL_PIXELFORMAT_UNKNOWN)
            throw std::runtime_error("Failed to select SDL3 RGBA surface format: " + std::string(SDL_GetError()));

        SDL_Surface* rawSurface = SDL_CreateSurface(width, height, format);
        if (!rawSurface)
            throw std::runtime_error("Failed to create SDL3 surface: " + std::string(SDL_GetError()));

        SurfaceUniquePtr surface(rawSurface, SDL_DestroySurface);
        for (int x = 0; x < width; ++x)
            for (int y = 0; y < height; ++y)
            {
                const osg::Vec4f clr = image->getColor(x, flip ? ((height - 1) - y) : y);
                auto* p = static_cast<Uint8*>(surface->pixels) + y * surface->pitch + x * 4;
                *reinterpret_cast<Uint32*>(p)
                    = SDL_MapSurfaceRGBA(surface.get(), static_cast<Uint8>(clr.r() * 255),
                        static_cast<Uint8>(clr.g() * 255), static_cast<Uint8>(clr.b() * 255),
                        static_cast<Uint8>(clr.a() * 255));
            }

        return surface;
    }

}
''',
    )


def port_cursor_surface_and_renderer() -> None:
    rel = "components/sdlutil/sdlcursormanager.cpp"
    text = read(rel)
    if "#include <memory>\n" not in text:
        text = text.replace("#include <stdexcept>\n", "#include <memory>\n#include <stdexcept>\n", 1)
    text = text.replace("SDL_FreeCursor(cursIter->second);", "SDL_DestroyCursor(cursIter->second);")
    text = text.replace(
        "            SDL_ShowCursor(SDL_FALSE);",
        "            SDL_HideCursor();",
    )
    old = '''        SDL_Surface* cursorSurface = SDL_CreateRGBSurfaceFrom(decompressedImage->data(), width, height,
            decompressedImage->getPixelSizeInBits(), decompressedImage->getRowSizeInBytes(), redMask, greenMask,
            blueMask, alphaMask);

        SDL_Surface* targetSurface
            = SDL_CreateRGBSurface(0, cursorWidth, cursorHeight, 32, redMask, greenMask, blueMask, alphaMask);
        SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(targetSurface);

        SDL_RenderClear(renderer);

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        SDL_Texture* cursorTexture = SDL_CreateTextureFromSurface(renderer, cursorSurface);

        SDL_RenderCopyEx(renderer, cursorTexture, nullptr, nullptr, -rotDegrees, nullptr, SDL_FLIP_NONE);

        SDL_DestroyTexture(cursorTexture);
        SDL_FreeSurface(cursorSurface);
        SDL_DestroyRenderer(renderer);

        return SDLUtil::SurfaceUniquePtr(targetSurface, SDL_FreeSurface);
'''
    new = '''        const SDL_PixelFormat sourceFormat = SDL_GetPixelFormatForMasks(
            decompressedImage->getPixelSizeInBits(), redMask, greenMask, blueMask, alphaMask);
        const SDL_PixelFormat targetFormat
            = SDL_GetPixelFormatForMasks(32, redMask, greenMask, blueMask, alphaMask);
        if (sourceFormat == SDL_PIXELFORMAT_UNKNOWN || targetFormat == SDL_PIXELFORMAT_UNKNOWN)
            throw std::runtime_error("Failed to select SDL3 cursor surface format: " + std::string(SDL_GetError()));

        SDLUtil::SurfaceUniquePtr cursorSurface(
            SDL_CreateSurfaceFrom(width, height, sourceFormat, decompressedImage->data(),
                static_cast<int>(decompressedImage->getRowSizeInBytes())),
            SDL_DestroySurface);
        SDLUtil::SurfaceUniquePtr targetSurface(
            SDL_CreateSurface(cursorWidth, cursorHeight, targetFormat), SDL_DestroySurface);
        if (!cursorSurface || !targetSurface)
            throw std::runtime_error("Failed to create SDL3 cursor surface: " + std::string(SDL_GetError()));

        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
            SDL_CreateSoftwareRenderer(targetSurface.get()), SDL_DestroyRenderer);
        if (!renderer)
            throw std::runtime_error("Failed to create SDL3 software renderer: " + std::string(SDL_GetError()));

        if (!SDL_RenderClear(renderer.get()))
            throw std::runtime_error("Failed to clear SDL3 cursor surface: " + std::string(SDL_GetError()));

        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> cursorTexture(
            SDL_CreateTextureFromSurface(renderer.get(), cursorSurface.get()), SDL_DestroyTexture);
        if (!cursorTexture)
            throw std::runtime_error("Failed to create SDL3 cursor texture: " + std::string(SDL_GetError()));

        if (!SDL_RenderTextureRotated(
                renderer.get(), cursorTexture.get(), nullptr, nullptr, -rotDegrees, nullptr, SDL_FLIP_NONE))
            throw std::runtime_error("Failed to rotate SDL3 cursor texture: " + std::string(SDL_GetError()));
        if (!SDL_RenderPresent(renderer.get()))
            throw std::runtime_error("Failed to present SDL3 cursor surface: " + std::string(SDL_GetError()));

        return targetSurface;
'''
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: cursor SDL2 surface/render block not found")
    write(rel, text)


def port_graphics_window() -> None:
    rel = "components/sdlutil/sdlgraphicswindow.cpp"
    text = read(rel)
    text = text.replace("SDL_SetWindowBordered(mWindow, flag ? SDL_TRUE : SDL_FALSE);", "SDL_SetWindowBordered(mWindow, flag);")
    text = text.replace("SDL_GL_GetDrawableSize(", "SDL_GetWindowSizeInPixels(")
    text = text.replace("return SDL_GL_MakeCurrent(mWindow, mContext) == 0;", "return SDL_GL_MakeCurrent(mWindow, mContext);")
    text = text.replace("return SDL_GL_MakeCurrent(nullptr, nullptr) == 0;", "return SDL_GL_MakeCurrent(nullptr, nullptr);")
    text = text.replace("SDL_GL_DeleteContext(mContext);", "SDL_GL_DestroyContext(mContext);")
    text = text.replace("if (SDL_GL_SetSwapInterval(-1) == -1)", "if (!SDL_GL_SetSwapInterval(-1))")
    text = text.replace("if (SDL_GL_SetSwapInterval(1) == -1)", "if (!SDL_GL_SetSwapInterval(1))")
    write(rel, text)


def port_key_event_contract() -> None:
    rel = "components/sdlutil/events.hpp"
    text = read(rel)
    marker = "    struct TouchEvent\n"
    key_struct = '''    struct KeyEvent
    {
        SDL_Scancode scancode = SDL_SCANCODE_UNKNOWN;
        SDL_Keycode sym = SDLK_UNKNOWN;
        SDL_Keymod mod = SDL_KMOD_NONE;

        KeyEvent() = default;
        explicit KeyEvent(const SDL_KeyboardEvent& arg)
            : scancode(arg.scancode)
            , sym(arg.key)
            , mod(arg.mod)
        {
        }
    };

'''
    if key_struct not in text:
        if "struct KeyEvent" in text:
            text = re.sub(r"    struct KeyEvent\n    \{.*?    \};\n\n", key_struct, text, count=1, flags=re.S)
        elif marker in text:
            text = text.replace(marker, key_struct + marker, 1)
        else:
            raise RuntimeError(f"{rel}: TouchEvent marker not found")
    text = text.replace("SDL_ControllerTouchpadEvent", "SDL_GamepadTouchpadEvent")
    text = text.replace("SDL_ControllerButtonEvent", "SDL_GamepadButtonEvent")
    text = text.replace("SDL_ControllerAxisEvent", "SDL_GamepadAxisEvent")
    text = text.replace("SDL_ControllerDeviceEvent", "SDL_GamepadDeviceEvent")
    write(rel, text)

    rel = "apps/openmw/mwbase/luamanager.hpp"
    text = read(rel).replace(
        "std::variant<SDL_Keysym, int, SDLUtil::TouchEvent, WheelChange> mValue;",
        "std::variant<SDLUtil::KeyEvent, int, SDLUtil::TouchEvent, WheelChange> mValue;",
    )
    write(rel, text)

    rel = "apps/openmw/mwlua/inputprocessor.hpp"
    text = read(rel).replace("std::get<SDL_Keysym>", "std::get<SDLUtil::KeyEvent>")
    write(rel, text)

    rel = "apps/openmw/mwlua/inputbindings.cpp"
    text = read(rel).replace("SDL_Keysym", "SDLUtil::KeyEvent")
    write(rel, text)

    write(
        "apps/openmw/mwinput/keyboardmanager.cpp",
        '''#include "keyboardmanager.hpp"

#include <cctype>

#include <MyGUI_InputManager.h>

#include <components/sdlutil/sdlmappings.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/inputmanager.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/windowmanager.hpp"

#include "actions.hpp"
#include "bindingsmanager.hpp"

namespace MWInput
{
    KeyboardManager::KeyboardManager(BindingsManager* bindingsManager)
        : mBindingsManager(bindingsManager)
    {
    }

    void KeyboardManager::textInput(const SDL_TextInputEvent& arg)
    {
        MyGUI::UString ustring(arg.text);
        MyGUI::UString::utf32string utf32string = ustring.asUTF32();
        for (MyGUI::UString::utf32string::const_iterator it = utf32string.begin(); it != utf32string.end(); ++it)
            MyGUI::InputManager::getInstance().injectKeyPress(MyGUI::KeyCode::None, *it);
    }

    void KeyboardManager::keyPressed(const SDL_KeyboardEvent& arg)
    {
        // HACK: to make default keybinding for the console work without printing an extra "^" upon closing.
        SDL_Window* textInputWindow = SDL_GetKeyboardFocus();
        auto kc = SDLUtil::sdlKeyToMyGUI(arg.key);
        if (mBindingsManager->getKeyBinding(A_Console) == arg.scancode
            && (arg.mod & SDL_KMOD_SHIFT) == 0 && MWBase::Environment::get().getWindowManager()->isConsoleMode()
            && textInputWindow)
            SDL_StopTextInput(textInputWindow);

        bool consumed = textInputWindow && SDL_TextInputActive(textInputWindow)
            && (!(SDLK_SCANCODE_MASK & arg.key)
                && ((kc == MyGUI::KeyCode::None && arg.key > 0xff)
                    || (arg.key >= 0 && arg.key <= 255 && std::isprint(static_cast<unsigned char>(arg.key)))));

        if (kc != MyGUI::KeyCode::None && !mBindingsManager->isDetectingBindingState())
        {
            if (MWBase::Environment::get().getWindowManager()->injectKeyPress(kc, 0, arg.repeat))
                consumed = true;
            mBindingsManager->setPlayerControlsEnabled(!consumed);
        }

        if (arg.repeat)
            return;

        MWBase::InputManager* input = MWBase::Environment::get().getInputManager();
        if (!input->controlsDisabled() && !consumed)
            mBindingsManager->keyPressed(arg);

        if (!consumed)
            MWBase::Environment::get().getLuaManager()->inputEvent(
                { MWBase::LuaManager::InputEvent::KeyPressed, SDLUtil::KeyEvent(arg) });

        input->setJoystickLastUsed(false);
    }

    void KeyboardManager::keyReleased(const SDL_KeyboardEvent& arg)
    {
        MWBase::Environment::get().getInputManager()->setJoystickLastUsed(false);
        auto kc = SDLUtil::sdlKeyToMyGUI(arg.key);

        if (!mBindingsManager->isDetectingBindingState())
            mBindingsManager->setPlayerControlsEnabled(!MyGUI::InputManager::getInstance().injectKeyRelease(kc));
        mBindingsManager->keyReleased(arg);
        MWBase::Environment::get().getLuaManager()->inputEvent(
            { MWBase::LuaManager::InputEvent::KeyReleased, SDLUtil::KeyEvent(arg) });
    }
}
''',
    )

    rel = "extern/oics/ICSInputControlSystem_keyboard.cpp"
    text = read(rel).replace("evt.keysym.scancode", "evt.scancode")
    write(rel, text)


def port_input_wrapper() -> None:
    rel = "components/sdlutil/sdlinputwrapper.cpp"
    text = read(rel)
    text = text.replace("SDL_GL_GetDrawableSize(", "SDL_GetWindowSizeInPixels(")
    text = text.replace(
        "while (SDL_PeepEvents(&evt, 1, SDL_GETEVENT, SDL_WINDOWEVENT, SDL_WINDOWEVENT) > 0)",
        "while (SDL_PeepEvents(&evt, 1, SDL_GETEVENT, SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST) > 0)",
    )
    compat = '''#if SDL_VERSION_ATLEAST(2, 30, 50)
            // SDL2-compat may pass us SDL3 display and window events alongside the SDL2 events for funsies
            // Until we are ready to move to SDL3, we'll want to prevent the noise

            // Silence 0x151 to 0x1FF range
            if (evt.type > SDL_DISPLAYEVENT && evt.type < SDL_WINDOWEVENT)
                continue;

            // Silence 0x202 to 0x2FF range
            if (evt.type > SDL_SYSWMEVENT && evt.type < SDL_KEYDOWN)
                continue;
#endif
'''
    text = text.replace(compat, "")
    poll = '''        while (SDL_PollEvent(&evt))
        {
            switch (evt.type)
'''
    poll_new = '''        while (SDL_PollEvent(&evt))
        {
            if (evt.type >= SDL_EVENT_WINDOW_FIRST && evt.type <= SDL_EVENT_WINDOW_LAST)
            {
                handleWindowEvent(evt);
                continue;
            }

            switch (evt.type)
'''
    if poll in text:
        text = text.replace(poll, poll_new, 1)
    text = text.replace("evt.key.keysym.sym", "evt.key.key")
    text = text.replace("evt.cdevice", "evt.gdevice")
    text = text.replace("evt.cbutton", "evt.gbutton")
    text = text.replace("evt.caxis", "evt.gaxis")
    text = text.replace("evt.ctouchpad", "evt.gtouchpad")
    event_map = {
        "case SDL_CONTROLLERDEVICEADDED:": "case SDL_EVENT_GAMEPAD_ADDED:",
        "case SDL_CONTROLLERDEVICEREMOVED:": "case SDL_EVENT_GAMEPAD_REMOVED:",
        "case SDL_CONTROLLERBUTTONDOWN:": "case SDL_EVENT_GAMEPAD_BUTTON_DOWN:",
        "case SDL_CONTROLLERBUTTONUP:": "case SDL_EVENT_GAMEPAD_BUTTON_UP:",
        "case SDL_CONTROLLERAXISMOTION:": "case SDL_EVENT_GAMEPAD_AXIS_MOTION:",
        "case SDL_CONTROLLERSENSORUPDATE:": "case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:",
        "case SDL_CONTROLLERTOUCHPADDOWN:": "case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:",
        "case SDL_CONTROLLERTOUCHPADMOTION:": "case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:",
        "case SDL_CONTROLLERTOUCHPADUP:": "case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:",
    }
    for old, new in event_map.items():
        text = text.replace(old, new)
    text = text.replace(
        '''                case SDL_WINDOWEVENT:
                    handleWindowEvent(evt);
                    break;
''',
        "",
    )
    display_block = '''                case SDL_DISPLAYEVENT:
                    switch (evt.display.event)
                    {
                        case SDL_DISPLAYEVENT_ORIENTATION:
                            if (mSensorListener
                                && evt.display.display == static_cast<Uint32>(Settings::video().mScreen))
                            {
                                mSensorListener->displayOrientationChanged();
                            }
                            break;
                        default:
                            break;
                    }
                    break;
'''
    display_new = '''                case SDL_EVENT_DISPLAY_ORIENTATION:
                    if (mSensorListener && evt.display.displayID == SDL_GetDisplayForWindow(mSDLWindow))
                        mSensorListener->displayOrientationChanged();
                    break;
'''
    text = text.replace(display_block, display_new)
    for old in (
        "                case SDL_DOLLARGESTURE:\n",
        "                case SDL_DOLLARRECORD:\n",
        "                case SDL_MULTIGESTURE:\n",
    ):
        text = text.replace(old, "")
    text = text.replace("switch (evt.window.event)", "switch (evt.type)")
    window_map = {
        "SDL_WINDOWEVENT_ENTER": "SDL_EVENT_WINDOW_MOUSE_ENTER",
        "SDL_WINDOWEVENT_LEAVE": "SDL_EVENT_WINDOW_MOUSE_LEAVE",
        "SDL_WINDOWEVENT_MOVED": "SDL_EVENT_WINDOW_MOVED",
        "SDL_WINDOWEVENT_SIZE_CHANGED": "SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED",
        "SDL_WINDOWEVENT_RESIZED": "SDL_EVENT_WINDOW_RESIZED",
        "SDL_WINDOWEVENT_FOCUS_GAINED": "SDL_EVENT_WINDOW_FOCUS_GAINED",
        "SDL_WINDOWEVENT_FOCUS_LOST": "SDL_EVENT_WINDOW_FOCUS_LOST",
        "SDL_WINDOWEVENT_CLOSE": "SDL_EVENT_WINDOW_CLOSE_REQUESTED",
        "SDL_WINDOWEVENT_SHOWN": "SDL_EVENT_WINDOW_SHOWN",
        "SDL_WINDOWEVENT_RESTORED": "SDL_EVENT_WINDOW_RESTORED",
        "SDL_WINDOWEVENT_HIDDEN": "SDL_EVENT_WINDOW_HIDDEN",
        "SDL_WINDOWEVENT_MINIMIZED": "SDL_EVENT_WINDOW_MINIMIZED",
    }
    for old, new in window_map.items():
        text = text.replace(old, new)
    text = text.replace(
        "SDL_SetWindowGrab(mSDLWindow, mGrabPointer && mAllowGrab ? SDL_TRUE : SDL_FALSE);",
        "SDL_SetWindowMouseGrab(mSDLWindow, mGrabPointer && mAllowGrab);",
    )
    text = text.replace(
        "bool success = mAllowGrab && SDL_SetRelativeMouseMode(relative ? SDL_TRUE : SDL_FALSE) == 0;",
        "const bool success = mAllowGrab && SDL_SetWindowRelativeMouseMode(mSDLWindow, relative);",
    )
    text = text.replace(
        "        SDL_ShowCursor(mWantMouseVisible || !mWindowHasFocus);",
        '''        if (mWantMouseVisible || !mWindowHasFocus)
            SDL_ShowCursor();
        else
            SDL_HideCursor();''',
    )
    text = text.replace("double preciseY = evt.wheel.preciseY;", "const double preciseY = evt.wheel.y;")
    old_mouse = '''#if SDL_VERSION_ATLEAST(2, 26, 0)
            packEvt.x = evt.wheel.mouseX * mScaleX;
            packEvt.y = evt.wheel.mouseY * mScaleY;
#endif
'''
    new_mouse = '''            packEvt.x = static_cast<Sint32>(evt.wheel.mouse_x * mScaleX);
            packEvt.y = static_cast<Sint32>(evt.wheel.mouse_y * mScaleY);
'''
    text = text.replace(old_mouse, new_mouse)
    text = text.replace(
        "packEvt.x = mMouseX = evt.motion.x * mScaleX;",
        "packEvt.x = mMouseX = static_cast<Sint32>(evt.motion.x * mScaleX);",
    )
    text = text.replace(
        "packEvt.y = mMouseY = evt.motion.y * mScaleY;",
        "packEvt.y = mMouseY = static_cast<Sint32>(evt.motion.y * mScaleY);",
    )
    text = text.replace(
        "packEvt.xrel = evt.motion.xrel * mScaleX;",
        "packEvt.xrel = static_cast<Sint32>(evt.motion.xrel * mScaleX);",
    )
    text = text.replace(
        "packEvt.yrel = evt.motion.yrel * mScaleY;",
        "packEvt.yrel = static_cast<Sint32>(evt.motion.yrel * mScaleY);",
    )
    write(rel, text)


def port_video_wrapper() -> None:
    write(
        "components/sdlutil/sdlvideowrapper.hpp",
        '''#ifndef OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H
#define OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H

#include <osg/ref_ptr>

#include <SDL3/SDL.h>

#include "vsyncmode.hpp"

struct SDL_Window;

namespace osgViewer
{
    class Viewer;
}

namespace Settings
{
    enum class WindowMode;
}

namespace SDLUtil
{
    class VideoWrapper
    {
    public:
        VideoWrapper(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer);
        ~VideoWrapper();

        void setSyncToVBlank(VSyncMode vsyncMode);
        void setGammaContrast(float gamma, float contrast);
        void setVideoMode(int width, int height, Settings::WindowMode windowMode, bool windowBorder);
        void centerWindow();

    private:
        SDL_Window* mWindow;
        osg::ref_ptr<osgViewer::Viewer> mViewer;

        float mGamma;
        float mContrast;
        bool mHasSetGammaContrast;
        bool mHasSystemGammaRamp;

        // SDL3 removed gamma-ramp APIs. On Windows CP1B preserves the legacy
        // behavior through the native HDC owned by the SDL window.
        Uint16 mOldSystemGammaRamp[256 * 3]{};
    };
}

#endif
''',
    )
    write(
        "components/sdlutil/sdlvideowrapper.cpp",
        '''#include "sdlvideowrapper.hpp"

#include <cmath>

#include <components/debug/debuglog.hpp>
#include <components/sdlutil/sdlgraphicswindow.hpp>
#include <components/settings/settings.hpp>

#include <osgViewer/Viewer>

#include <SDL3/SDL.h>

#ifdef _WIN32
#pragma push_macro("NOGDI")
#undef NOGDI
#include <windows.h>
#pragma pop_macro("NOGDI")
#endif

namespace SDLUtil
{
    namespace
    {
#ifdef _WIN32
        HDC getWindowHdc(SDL_Window* window)
        {
            return static_cast<HDC>(SDL_GetPointerProperty(
                SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HDC_POINTER, nullptr));
        }

        bool getWindowGammaRamp(SDL_Window* window, Uint16* ramp)
        {
            HDC hdc = getWindowHdc(window);
            return hdc != nullptr && GetDeviceGammaRamp(hdc, ramp) != FALSE;
        }

        bool setWindowGammaRamp(SDL_Window* window, const Uint16* ramp)
        {
            HDC hdc = getWindowHdc(window);
            return hdc != nullptr && SetDeviceGammaRamp(hdc, const_cast<Uint16*>(ramp)) != FALSE;
        }
#endif

        float windowPixelDensity(SDL_Window* window)
        {
            const float density = SDL_GetWindowPixelDensity(window);
            return density > 0.f ? density : 1.f;
        }
    }

    VideoWrapper::VideoWrapper(SDL_Window* window, osg::ref_ptr<osgViewer::Viewer> viewer)
        : mWindow(window)
        , mViewer(std::move(viewer))
        , mGamma(1.f)
        , mContrast(1.f)
        , mHasSetGammaContrast(false)
        , mHasSystemGammaRamp(false)
    {
#ifdef _WIN32
        mHasSystemGammaRamp = getWindowGammaRamp(mWindow, mOldSystemGammaRamp);
#endif
    }

    VideoWrapper::~VideoWrapper()
    {
        SDL_SetWindowFullscreen(mWindow, false);
#ifdef _WIN32
        if (mHasSetGammaContrast && mHasSystemGammaRamp && !setWindowGammaRamp(mWindow, mOldSystemGammaRamp))
            Log(Debug::Warning) << "Couldn't restore gamma ramp";
#endif
    }

    void VideoWrapper::setSyncToVBlank(VSyncMode vsyncMode)
    {
        osgViewer::Viewer::Windows windows;
        mViewer->getWindows(windows);
        mViewer->stopThreading();
        for (osgViewer::Viewer::Windows::iterator it = windows.begin(); it != windows.end(); ++it)
        {
            osgViewer::GraphicsWindow* win = *it;
            if (GraphicsWindowSDL2* sdl2win = dynamic_cast<GraphicsWindowSDL2*>(win))
                sdl2win->setSyncToVBlank(vsyncMode);
            else
                win->setSyncToVBlank(vsyncMode != VSyncMode::Disabled);
        }
        mViewer->startThreading();
    }

    void VideoWrapper::setGammaContrast(float gamma, float contrast)
    {
        if (gamma == mGamma && contrast == mContrast)
            return;

        mGamma = gamma;
        mContrast = contrast;

        Uint16 red[256], green[256], blue[256];
        for (int i = 0; i < 256; i++)
        {
            float k = i / 256.0f;
            k = (k - 0.5f) * contrast + 0.5f;
            k = std::pow(k, 1.f / gamma);
            k *= 256;
            float value = k * 256;
            if (value > 65535)
                value = 65535;
            else if (value < 0)
                value = 0;

            red[i] = green[i] = blue[i] = static_cast<Uint16>(value);
        }

#ifdef _WIN32
        Uint16 ramp[256 * 3];
        for (int i = 0; i < 256; ++i)
        {
            ramp[i] = red[i];
            ramp[256 + i] = green[i];
            ramp[512 + i] = blue[i];
        }
        if (!setWindowGammaRamp(mWindow, ramp))
        {
            Log(Debug::Warning) << "Couldn't set gamma ramp";
            return;
        }
        mHasSetGammaContrast = true;
#else
        Log(Debug::Warning) << "SDL3 no longer exposes a system gamma-ramp API on this platform";
#endif
    }

    void VideoWrapper::setVideoMode(int width, int height, Settings::WindowMode windowMode, bool windowBorder)
    {
        if (!SDL_SetWindowFullscreen(mWindow, false))
            Log(Debug::Warning) << "Couldn't leave fullscreen mode: " << SDL_GetError();

        if (SDL_GetWindowFlags(mWindow) & SDL_WINDOW_MAXIMIZED)
            SDL_RestoreWindow(mWindow);

        if (windowMode == Settings::WindowMode::Fullscreen)
        {
            SDL_DisplayID display = SDL_GetDisplayForWindow(mWindow);
            if (!display)
                display = SDL_GetPrimaryDisplay();

            SDL_DisplayMode mode{};
            if (!display
                || !SDL_GetClosestFullscreenDisplayMode(display, width, height, 0.f, true, &mode)
                || !SDL_SetWindowFullscreenMode(mWindow, &mode))
            {
                Log(Debug::Warning) << "Couldn't select exclusive fullscreen mode: " << SDL_GetError();
            }
            if (!SDL_SetWindowFullscreen(mWindow, true))
                Log(Debug::Warning) << "Couldn't enter fullscreen mode: " << SDL_GetError();
        }
        else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        {
            if (!SDL_SetWindowFullscreenMode(mWindow, nullptr) || !SDL_SetWindowFullscreen(mWindow, true))
                Log(Debug::Warning) << "Couldn't enter borderless fullscreen mode: " << SDL_GetError();
        }
        else
        {
            const float density = windowPixelDensity(mWindow);
            SDL_SetWindowSize(
                mWindow, static_cast<int>(width / density), static_cast<int>(height / density));
            SDL_SetWindowBordered(mWindow, windowBorder);
            centerWindow();
        }
    }

    void VideoWrapper::centerWindow()
    {
        SDL_Rect rect{};
        int w = 0;
        int h = 0;
        SDL_DisplayID display = SDL_GetDisplayForWindow(mWindow);
        if (!display)
            display = SDL_GetPrimaryDisplay();
        if (!display || !SDL_GetDisplayBounds(display, &rect))
            return;

        SDL_GetWindowSize(mWindow, &w, &h);
        int x = rect.x;
        int y = rect.y;

        if (w < rect.w)
            x = rect.x + rect.w / 2 - w / 2;
        if (h < rect.h)
            y = rect.y + rect.h / 2 - h / 2;

        SDL_SetWindowPosition(mWindow, x, y);
    }
}
''',
    )


def port_engine_windowing() -> None:
    rel = "apps/openmw/engine.cpp"
    text = read(rel)
    text = text.replace(
        '''    void checkSDLError(int ret)
    {
        if (ret != 0)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }
''',
        '''    void checkSDLError(bool success)
    {
        if (!success)
            Log(Debug::Error) << "SDL error: " << SDL_GetError();
    }
''',
    )
    text = text.replace(
        '''    Uint32 flags
        = SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
''',
        '''    const SDL_InitFlags flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_SENSOR;
''',
    )
    text = text.replace(
        '''        if (SDL_Init(flags) != 0)
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
''',
        '''        if (!SDL_Init(flags))
        {
            throw std::runtime_error("Could not initialize SDL! " + std::string(SDL_GetError()));
        }
''',
    )
    text = text.replace("SDL_GL_GetDrawableSize(", "SDL_GetWindowSizeInPixels(")
    text = text.replace("SDL_WINDOW_ALLOW_HIGHDPI", "SDL_WINDOW_HIGH_PIXEL_DENSITY")
    text = text.replace(" | SDL_WINDOW_SHOWN", "")
    old_pos = '''    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(screen);

    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(screen);
    }

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (windowMode == Settings::WindowMode::Fullscreen)
        flags |= SDL_WINDOW_FULLSCREEN;
    else if (windowMode == Settings::WindowMode::WindowedFullscreen)
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
'''
    new_pos = '''    int displayCount = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
    SDL_DisplayID displayId = 0;
    if (displays && screen >= 0 && screen < displayCount)
        displayId = displays[screen];
    SDL_free(displays);
    if (!displayId)
        displayId = SDL_GetPrimaryDisplay();

    int posX = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    int posY = SDL_WINDOWPOS_CENTERED_DISPLAY(displayId);
    if (windowMode == Settings::WindowMode::Fullscreen || windowMode == Settings::WindowMode::WindowedFullscreen)
    {
        posX = SDL_WINDOWPOS_UNDEFINED_DISPLAY(displayId);
        posY = SDL_WINDOWPOS_UNDEFINED_DISPLAY(displayId);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
'''
    if old_pos in text:
        text = text.replace(old_pos, new_pos, 1)
    elif new_pos not in text:
        raise RuntimeError(f"{rel}: window position/flag block not found")

    old_create = '''            mWindow = SDL_CreateWindow("OpenMW", posX, posY, width, height, flags);
            if (!mWindow)
'''
    new_create = '''            mWindow = SDL_CreateWindow("OpenMW", width, height, flags);
            if (mWindow)
            {
                SDL_SetWindowPosition(mWindow, posX, posY);
                if (windowMode == Settings::WindowMode::Fullscreen)
                {
                    SDL_DisplayMode mode{};
                    if (displayId
                        && SDL_GetClosestFullscreenDisplayMode(displayId, width, height, 0.f, true, &mode))
                        checkSDLError(SDL_SetWindowFullscreenMode(mWindow, &mode));
                    checkSDLError(SDL_SetWindowFullscreen(mWindow, true));
                }
                else if (windowMode == Settings::WindowMode::WindowedFullscreen)
                {
                    checkSDLError(SDL_SetWindowFullscreenMode(mWindow, nullptr));
                    checkSDLError(SDL_SetWindowFullscreen(mWindow, true));
                }
            }
            if (!mWindow)
'''
    if old_create in text:
        text = text.replace(old_create, new_create, 1)
    elif new_create not in text:
        raise RuntimeError(f"{rel}: SDL_CreateWindow block not found")

    text = text.replace(
        "traits->screenNum = SDL_GetWindowDisplayIndex(mWindow);",
        "traits->screenNum = screen;",
    )
    old_version = '''    SDL_version sdlVersion;
    SDL_GetVersion(&sdlVersion);
    Log(Debug::Info) << "SDL version: " << (int)sdlVersion.major << "." << (int)sdlVersion.minor << "."
                     << (int)sdlVersion.patch;
'''
    new_version = '''    const int sdlVersion = SDL_GetVersion();
    Log(Debug::Info) << "SDL version: " << SDL_VERSIONNUM_MAJOR(sdlVersion) << "."
                     << SDL_VERSIONNUM_MINOR(sdlVersion) << "." << SDL_VERSIONNUM_MICRO(sdlVersion);
'''
    if old_version in text:
        text = text.replace(old_version, new_version, 1)
    elif new_version not in text:
        raise RuntimeError(f"{rel}: SDL version block not found")
    write(rel, text)


def port_controller_enumeration_and_bool_semantics() -> None:
    rel = "apps/openmw/mwinput/controllermanager.cpp"
    text = read(rel)
    old = '''        // Open all presently connected sticks
        const int numSticks = SDL_NumJoysticks();
        if (numSticks < 0)
            Log(Debug::Error) << "Failed to get number of joysticks: " << SDL_GetError();

        for (int i = 0; i < numSticks; i++)
        {
            if (SDL_IsGameController(i))
            {
                SDL_ControllerDeviceEvent evt;
                evt.which = i;
                static const int fakeDeviceID = 1;
                ControllerManager::controllerAdded(fakeDeviceID, evt);
                if (const char* name = SDL_GameControllerNameForIndex(i))
                    Log(Debug::Info) << "Detected game controller: " << name;
                else
                    Log(Debug::Warning) << "Detected game controller without a name: " << SDL_GetError();
            }
            else
            {
                if (const char* name = SDL_JoystickNameForIndex(i))
                    Log(Debug::Info) << "Detected unusable controller: " << name;
                else
                    Log(Debug::Warning) << "Detected unusable controller without a name: " << SDL_GetError();
            }
        }
'''
    new = '''        // SDL3 enumerates joysticks by stable instance ID rather than transient device index.
        int numSticks = 0;
        SDL_JoystickID* sticks = SDL_GetJoysticks(&numSticks);
        if (!sticks && numSticks != 0)
            Log(Debug::Error) << "Failed to enumerate joysticks: " << SDL_GetError();

        for (int i = 0; sticks && i < numSticks; ++i)
        {
            const SDL_JoystickID id = sticks[i];
            if (SDL_IsGamepad(id))
            {
                SDL_GamepadDeviceEvent evt{};
                evt.which = id;
                static const int fakeDeviceID = 1;
                ControllerManager::controllerAdded(fakeDeviceID, evt);
                if (const char* name = SDL_GetGamepadNameForID(id))
                    Log(Debug::Info) << "Detected game controller: " << name;
                else
                    Log(Debug::Warning) << "Detected game controller without a name: " << SDL_GetError();
            }
            else
            {
                if (const char* name = SDL_GetJoystickNameForID(id))
                    Log(Debug::Info) << "Detected unusable controller: " << name;
                else
                    Log(Debug::Warning) << "Detected unusable controller without a name: " << SDL_GetError();
            }
        }
        SDL_free(sticks);
'''
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: controller enumeration block not found")

    old_sensor_enable = '''        if (const int result = SDL_GameControllerSetSensorEnabled(cntrl, SDL_SENSOR_GYRO, SDL_TRUE); result < 0)
        {
            Log(Debug::Error) << "Failed to enable game controller sensor: " << SDL_GetError();
            return;
        }
'''
    new_sensor_enable = '''        if (!SDL_SetGamepadSensorEnabled(cntrl, SDL_SENSOR_GYRO, true))
        {
            Log(Debug::Error) << "Failed to enable game controller sensor: " << SDL_GetError();
            return;
        }
'''
    text = text.replace(old_sensor_enable, new_sensor_enable)
    old_sensor_data = '''            const int result = SDL_GameControllerGetSensorData(cntrl, SDL_SENSOR_GYRO, gyro, 3);
            if (result < 0)
                Log(Debug::Error) << "Failed to get game controller sensor data: " << SDL_GetError();
'''
    new_sensor_data = '''            if (!SDL_GetGamepadSensorData(cntrl, SDL_SENSOR_GYRO, gyro, 3))
                Log(Debug::Error) << "Failed to get game controller sensor data: " << SDL_GetError();
'''
    text = text.replace(old_sensor_data, new_sensor_data)
    write(rel, text)

    rel = "extern/oics/ICSInputControlSystem_joystick.cpp"
    text = read(rel)
    old_oics = '''\t\tSDL_GameController* cntrl = SDL_GameControllerOpen(args.which);
        int instanceID = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(cntrl));
'''
    new_oics = '''\t\tSDL_Gamepad* cntrl = SDL_OpenGamepad(args.which);
        if (!cntrl)
        {
            ICS_LOG("Failed to open gamepad");
            return;
        }
        const SDL_JoystickID instanceID = SDL_GetJoystickID(SDL_GetGamepadJoystick(cntrl));
'''
    if old_oics in text:
        text = text.replace(old_oics, new_oics, 1)
    elif new_oics not in text:
        raise RuntimeError(f"{rel}: gamepad open block not found")
    write(rel, text)


def port_sensor_manager() -> None:
    write(
        "apps/openmw/mwinput/sensormanager.cpp",
        '''#include "sensormanager.hpp"

#include <SDL3/SDL.h>

#include <components/debug/debuglog.hpp>
#include <components/settings/values.hpp>

namespace
{
    SDL_DisplayID configuredDisplayId()
    {
        int count = 0;
        SDL_DisplayID* displays = SDL_GetDisplays(&count);
        SDL_DisplayID id = 0;
        const int screen = Settings::video().mScreen;
        if (displays && screen >= 0 && screen < count)
            id = displays[screen];
        SDL_free(displays);
        return id ? id : SDL_GetPrimaryDisplay();
    }
}

namespace MWInput
{
    SensorManager::SensorManager()
        : mRotation()
        , mGyroValues()
        , mGyroUpdateTimer(0.f)
        , mGyroscope(nullptr)
    {
        init();
    }

    void SensorManager::init()
    {
        correctGyroscopeAxes();
        updateSensors();
    }

    SensorManager::~SensorManager()
    {
        if (mGyroscope != nullptr)
        {
            SDL_CloseSensor(mGyroscope);
            mGyroscope = nullptr;
        }
    }

    void SensorManager::correctGyroscopeAxes()
    {
        if (!Settings::input().mEnableGyroscope)
            return;

        mRotation = osg::Matrixf::identity();

        float angle = 0;
        const SDL_DisplayID display = configuredDisplayId();
        const SDL_DisplayOrientation currentOrientation
            = display ? SDL_GetCurrentDisplayOrientation(display) : SDL_ORIENTATION_UNKNOWN;
        switch (currentOrientation)
        {
            case SDL_ORIENTATION_UNKNOWN:
            case SDL_ORIENTATION_LANDSCAPE:
                break;
            case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
                angle = osg::PIf;
                break;
            case SDL_ORIENTATION_PORTRAIT:
                angle = -0.5 * osg::PIf;
                break;
            case SDL_ORIENTATION_PORTRAIT_FLIPPED:
                angle = 0.5 * osg::PIf;
                break;
        }

        mRotation.makeRotate(angle, osg::Vec3f(0, 0, 1));
    }

    void SensorManager::updateSensors()
    {
        if (Settings::input().mEnableGyroscope)
        {
            int count = 0;
            SDL_SensorID* sensors = SDL_GetSensors(&count);
            for (int i = 0; sensors && i < count; ++i)
            {
                const SDL_SensorID id = sensors[i];
                if (SDL_GetSensorTypeForID(id) != SDL_SENSOR_GYRO)
                    continue;

                if (mGyroscope != nullptr)
                {
                    SDL_CloseSensor(mGyroscope);
                    mGyroscope = nullptr;
                    mGyroUpdateTimer = 0.f;
                }

                SDL_Sensor* sensor = SDL_OpenSensor(id);
                if (sensor == nullptr)
                {
                    const char* name = SDL_GetSensorNameForID(id);
                    Log(Debug::Error) << "Couldn't open sensor " << (name ? name : "<unnamed>") << ": "
                                      << SDL_GetError();
                }
                else
                {
                    mGyroscope = sensor;
                    break;
                }
            }
            SDL_free(sensors);
        }
        else if (mGyroscope != nullptr)
        {
            SDL_CloseSensor(mGyroscope);
            mGyroscope = nullptr;
            mGyroUpdateTimer = 0.f;
        }
    }

    void SensorManager::processChangedSettings(const Settings::CategorySettingVector& changed)
    {
        for (const auto& setting : changed)
        {
            if (setting.first == "Input" && setting.second == "enable gyroscope")
                init();
        }
    }

    void SensorManager::displayOrientationChanged()
    {
        correctGyroscopeAxes();
    }

    void SensorManager::sensorUpdated(const SDL_SensorEvent& arg)
    {
        if (!Settings::input().mEnableGyroscope)
            return;

        SDL_Sensor* sensor = SDL_GetSensorFromID(arg.which);
        if (!sensor)
        {
            Log(Debug::Info) << "Couldn't get sensor for sensor event";
            return;
        }

        switch (SDL_GetSensorType(sensor))
        {
            case SDL_SENSOR_ACCEL:
                break;
            case SDL_SENSOR_GYRO:
            {
                osg::Vec3f gyro(arg.data[0], arg.data[1], arg.data[2]);
                mGyroValues = mRotation * gyro;
                mGyroUpdateTimer = 0.f;
                break;
            }
            default:
                break;
        }
    }

    void SensorManager::update(float dt)
    {
        mGyroUpdateTimer += dt;
        if (mGyroUpdateTimer > 0.5f)
        {
            mGyroValues = osg::Vec3f();
            mGyroUpdateTimer = 0.f;
        }
    }

    bool SensorManager::isGyroAvailable() const
    {
        return mGyroscope != nullptr;
    }

    std::array<float, 3> SensorManager::getGyroValues() const
    {
        return { mGyroValues.x(), mGyroValues.y(), mGyroValues.z() };
    }
}
''',
    )


def port_gui_text_input_and_dpi() -> None:
    rel = "apps/openmw/mwgui/windowmanagerimp.cpp"
    text = read(rel)
    text = text.replace("SDL_GL_GetDrawableSize(window, &dw, &dh);", "SDL_GetWindowSizeInPixels(window, &dw, &dh);")
    old = '''        const bool inputActive = SDL_IsTextInputActive() == SDL_TRUE;
        if (capturesInput == inputActive)
            return;

        if (capturesInput)
            SDL_StartTextInput();
        else
            SDL_StopTextInput();
'''
    new = '''        SDL_Window* inputWindow = SDL_GetKeyboardFocus();
        const bool inputActive = inputWindow && SDL_TextInputActive(inputWindow);
        if (capturesInput == inputActive)
            return;

        if (!inputWindow)
            return;
        if (capturesInput)
            SDL_StartTextInput(inputWindow);
        else
            SDL_StopTextInput(inputWindow);
'''
    if old in text:
        text = text.replace(old, new, 1)
    elif new not in text:
        raise RuntimeError(f"{rel}: text input focus block not found")
    write(rel, text)


def apply_simple_native_renames() -> None:
    replacements = {
        "SDL_ControllerTouchpadEvent": "SDL_GamepadTouchpadEvent",
        "SDL_ControllerButtonEvent": "SDL_GamepadButtonEvent",
        "SDL_ControllerAxisEvent": "SDL_GamepadAxisEvent",
        "SDL_ControllerDeviceEvent": "SDL_GamepadDeviceEvent",
        "SDL_GameControllerAxis": "SDL_GamepadAxis",
        "SDL_GameControllerButton": "SDL_GamepadButton",
        "SDL_GameControllerType": "SDL_GamepadType",
        "SDL_GameController*": "SDL_Gamepad*",
        "SDL_GameControllerAddMappingsFromFile": "SDL_AddGamepadMappingsFromFile",
        "SDL_GameControllerGetAxis": "SDL_GetGamepadAxis",
        "SDL_GameControllerGetButton": "SDL_GetGamepadButton",
        "SDL_GameControllerHasSensor": "SDL_GamepadHasSensor",
        "SDL_GameControllerGetType": "SDL_GetGamepadType",
        "SDL_GameControllerClose": "SDL_CloseGamepad",
        "SDL_GameControllerOpen": "SDL_OpenGamepad",
        "SDL_GameControllerGetJoystick": "SDL_GetGamepadJoystick",
        "SDL_JoystickInstanceID": "SDL_GetJoystickID",
        "SDL_SensorClose": "SDL_CloseSensor",
        "SDL_SensorFromInstanceID": "SDL_GetSensorFromID",
        "SDL_SensorGetType": "SDL_GetSensorType",
        "SDL_WINDOW_ALLOW_HIGHDPI": "SDL_WINDOW_HIGH_PIXEL_DENSITY",
        "SDL_SetWindowGrab(": "SDL_SetWindowMouseGrab(",
        "SDL_FreeCursor(": "SDL_DestroyCursor(",
        "SDL_FreeSurface(": "SDL_DestroySurface(",
    }
    for path in iter_sources():
        text = path.read_text(encoding="utf-8")
        updated = text
        for old, new in replacements.items():
            updated = updated.replace(old, new)
        updated = re.sub(r"\bSDL_CONTROLLER_BUTTON_", "SDL_GAMEPAD_BUTTON_", updated)
        updated = re.sub(r"\bSDL_CONTROLLER_AXIS_", "SDL_GAMEPAD_AXIS_", updated)
        if updated != text:
            path.write_text(updated, encoding="utf-8")


def port_text_input_signatures() -> None:
    # Any remaining no-argument SDL2 text-input calls are bound to the current
    # keyboard-focus window. Known GUI/keyboard paths are handled explicitly;
    # this catches stragglers elsewhere in the OpenMW source surface.
    for path in iter_sources():
        text = path.read_text(encoding="utf-8")
        updated = text.replace(
            "SDL_IsTextInputActive()",
            "(SDL_GetKeyboardFocus() != nullptr && SDL_TextInputActive(SDL_GetKeyboardFocus()))",
        )
        updated = updated.replace("SDL_StartTextInput()", "SDL_StartTextInput(SDL_GetKeyboardFocus())")
        updated = updated.replace("SDL_StopTextInput()", "SDL_StopTextInput(SDL_GetKeyboardFocus())")
        if updated != text:
            path.write_text(updated, encoding="utf-8")


def stage_sdl3_runtime_and_remove_sdl2main() -> None:
    rel = "CMakeLists.txt"
    text = read(rel)
    anchor = '''find_package(SDL3 3.4.10 CONFIG REQUIRED)
# SDL3 provides a native transition alias set while the remaining CP1B call
# sites are converted to SDL3 names and semantics. No SDL2 runtime is linked.
add_compile_definitions(SDL_ENABLE_OLD_NAMES)
'''
    replacement = '''find_package(SDL3 3.4.10 CONFIG REQUIRED)
# CP1B links the native SDL3 runtime. SDL3's old-name aliases remain enabled
# only as a compile-time bridge for harmless one-to-one renames; semantic
# changes are ported explicitly and enforced by the CP1B source audit.
add_compile_definitions(SDL_ENABLE_OLD_NAMES)

if(WIN32)
    set(_openmw_sdl3_runtime "${_openmw_sdl3_root}/lib/x64/SDL3.dll")
    if(NOT EXISTS "${_openmw_sdl3_runtime}")
        message(FATAL_ERROR "Pinned SDL3 runtime DLL not found: ${_openmw_sdl3_runtime}")
    endif()
    install(FILES "${_openmw_sdl3_runtime}" DESTINATION ".")
endif()
'''
    if anchor in text:
        text = text.replace(anchor, replacement, 1)
    elif replacement not in text:
        if 'set(_openmw_sdl3_runtime "${_openmw_sdl3_root}/lib/x64/SDL3.dll")' not in text:
            raise RuntimeError(f"{rel}: SDL3 find-package/runtime block not found")
    write(rel, text)

    rel = "apps/openmw/CMakeLists.txt"
    text = read(rel)
    text = text.replace(
        '''if (NOT UNIX)
    target_link_libraries(openmw-lib ${SDL2MAIN_LIBRARY})
endif()
''',
        "",
    )
    write(rel, text)


def verify_source_contract() -> None:
    forbidden_tokens = {
        "SDL_GL_GetDrawableSize": "removed drawable-size API",
        "SDL_Keysym": "removed SDL2 key symbol struct",
        ".keysym": "removed SDL2 keyboard-event layout",
        "SDL_WINDOWEVENT": "removed aggregate SDL2 window event",
        "SDL_DISPLAYEVENT": "removed aggregate SDL2 display event",
        "SDL_CreateRGBSurface": "removed SDL2 surface constructor",
        "SDL_FreeSurface": "removed SDL2 surface destructor",
        "SDL_GetWindowGammaRamp": "removed SDL3 gamma API",
        "SDL_SetWindowGammaRamp": "removed SDL3 gamma API",
        "SDL_NumJoysticks": "removed device-index joystick enumeration",
        "SDL_IsGameController": "SDL2 game-controller device-index API",
        "SDL_GameControllerNameForIndex": "SDL2 game-controller device-index API",
        "SDL_JoystickNameForIndex": "SDL2 joystick device-index API",
        "SDL_NumSensors": "removed device-index sensor enumeration",
        "SDL_SensorGetDeviceType": "removed device-index sensor API",
        "SDL_SensorGetDeviceName": "removed device-index sensor API",
        "SDL_SensorOpen": "removed device-index sensor API",
        "SDL_GetWindowDisplayIndex": "SDL2 display-index API",
        "SDL_WINDOW_FULLSCREEN_DESKTOP": "removed SDL2 borderless-fullscreen flag",
        "SDL_INIT_NOPARACHUTE": "removed SDL2 init flag",
        "SDL_INIT_GAMECONTROLLER": "renamed SDL2 init flag",
        "SDL_SetRelativeMouseMode(": "SDL2 global relative-mouse API",
        "SDL_SetWindowGrab": "removed SDL2 window-grab API",
        "SDL_IsTextInputActive()": "SDL2 no-window text-input API",
        "SDL_StartTextInput()": "SDL2 no-window text-input API",
        "SDL_StopTextInput()": "SDL2 no-window text-input API",
        "SDL_UNKNOWN": "invalid SDL3 keycode token",
    }
    offenders: list[str] = []
    bare_include_re = re.compile(r'#include[ \t]+[<"]SDL(?:_[A-Za-z0-9_]+)?\.h[>"]')
    bool_compare_re = re.compile(
        r"SDL_(?:GL_MakeCurrent|GL_SetSwapInterval|Init|SetWindowFullscreen|SetWindowFullscreenMode)"
        r"\([^;\n]*\)\s*(?:==|!=)\s*(?:-?1|0)"
    )
    for path in iter_sources():
        text = path.read_text(encoding="utf-8")
        if bare_include_re.search(text):
            offenders.append(f"{path.relative_to(ROOT)}: bare SDL2-layout include")
        if bool_compare_re.search(text):
            offenders.append(f"{path.relative_to(ROOT)}: SDL3 bool return compared with SDL2 integer convention")
        for token, reason in forbidden_tokens.items():
            if token in text:
                offenders.append(f"{path.relative_to(ROOT)}: {reason}: {token}")

    cmake_text = "\n".join(p.read_text(encoding="utf-8") for p in ROOT.rglob("CMakeLists.txt"))
    if "SDL2::SDL2" in cmake_text:
        offenders.append("CMakeLists: SDL2::SDL2 target remains")
    if "SDL2MAIN_LIBRARY" in read("apps/openmw/CMakeLists.txt"):
        offenders.append("apps/openmw/CMakeLists.txt: SDL2MAIN_LIBRARY remains")
    if "find_package(SDL3 3.4.10 CONFIG REQUIRED)" not in read("CMakeLists.txt"):
        offenders.append("CMakeLists.txt: pinned SDL3 3.4.10 dependency missing")
    if "SDL3::SDL3" not in read("components/CMakeLists.txt"):
        offenders.append("components/CMakeLists.txt: SDL3::SDL3 target missing")
    if offenders:
        raise RuntimeError("CP1B SDL3 semantic migration incomplete:\n" + "\n".join(offenders))


def main() -> None:
    normalize_sdl_includes()
    port_image_to_surface()
    port_cursor_surface_and_renderer()
    port_graphics_window()
    port_key_event_contract()
    port_input_wrapper()
    port_video_wrapper()
    port_engine_windowing()
    port_controller_enumeration_and_bool_semantics()
    port_sensor_manager()
    port_gui_text_input_and_dpi()
    apply_simple_native_renames()
    port_text_input_signatures()
    stage_sdl3_runtime_and_remove_sdl2main()
    normalize_sdl_includes()
    verify_source_contract()
    print("CP1B SDL3 semantic migration complete and source contract verified.")


if __name__ == "__main__":
    main()
