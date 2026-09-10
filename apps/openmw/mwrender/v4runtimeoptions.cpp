#include "v4runtimeoptions.hpp"

#include <components/settings/values.hpp>

#include <algorithm>

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
        result.host.shadows.enabled = Settings::shadows().mEnableShadows;
        result.host.shadows.cascadeCount = static_cast<std::uint32_t>(
            std::clamp(Settings::shadows().mNumberOfShadowMaps.get(), 1, 8));
        result.host.shadows.mapResolution = static_cast<std::uint32_t>(
            std::clamp(Settings::shadows().mShadowMapResolution.get(), 256, 4096));
        const float maximumShadowDistance = Settings::shadows().mMaximumShadowMapDistance;
        result.host.shadows.maximumDistance = maximumShadowDistance > 0.0f ? maximumShadowDistance : 1e8;
        result.host.shadows.splitLambda = Settings::shadows().mSplitPointUniformLogarithmicRatio;
        result.host.shadows.actorCasters = Settings::shadows().mActorShadows || Settings::shadows().mPlayerShadows;
        result.host.shadows.terrainCasters = Settings::shadows().mTerrainShadows;
        result.host.shadows.objectCasters = Settings::shadows().mObjectShadows;
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
