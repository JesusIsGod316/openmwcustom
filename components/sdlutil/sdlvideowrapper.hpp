#ifndef OPENMW_COMPONENTS_SDLUTIL_SDLVIDEOWRAPPER_H
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
