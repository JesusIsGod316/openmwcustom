#include "sdlvideowrapper.hpp"

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
