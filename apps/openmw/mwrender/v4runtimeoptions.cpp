#include "v4runtimeoptions.hpp"

#include <components/settings/values.hpp>

namespace MWRender
{
    RenderVsg::VsgRuntimeBootstrapOptions makeV4RuntimeBootstrapOptions()
    {
        RenderVsg::VsgRuntimeBootstrapOptions result;
        result.title = "OpenMW";
        result.width = static_cast<std::uint32_t>(Settings::video().mResolutionX);
        result.height = static_cast<std::uint32_t>(Settings::video().mResolutionY);
        result.displayIndex = Settings::video().mScreen;
        result.windowBorder = Settings::video().mWindowBorder;
        result.minimizeOnFocusLoss = Settings::video().mMinimizeOnFocusLoss;
        switch (static_cast<Settings::WindowMode>(Settings::video().mWindowMode))
        {
            case Settings::WindowMode::Fullscreen: result.windowMode = RenderVsg::VsgWindowMode::Fullscreen; break;
            case Settings::WindowMode::WindowedFullscreen:
                result.windowMode = RenderVsg::VsgWindowMode::WindowedFullscreen;
                break;
            case Settings::WindowMode::Windowed: result.windowMode = RenderVsg::VsgWindowMode::Windowed; break;
        }
        switch (static_cast<SDLUtil::VSyncMode>(Settings::video().mVsyncMode))
        {
            case SDLUtil::Disabled: result.presentMode = RenderVsg::VsgPresentMode::Immediate; break;
            case SDLUtil::Adaptive: result.presentMode = RenderVsg::VsgPresentMode::Adaptive; break;
            case SDLUtil::Enabled: result.presentMode = RenderVsg::VsgPresentMode::VSync; break;
        }
        return result;
    }
}
