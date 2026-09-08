#include "fogmanager.hpp"

#include <algorithm>
#include <limits>

#include <components/esm/esmbridge.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm4/loadcell.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sceneutil/util.hpp>
#include <components/settings/values.hpp>

#include "apps/openmw/mwworld/cell.hpp"

namespace MWRender
{
    FogManager::FogManager()
        : mLandFogStart(0.f)
        , mLandFogEnd(std::numeric_limits<float>::max())
        , mUnderwaterFogStart(0.f)
        , mUnderwaterFogEnd(std::numeric_limits<float>::max())
        , mFogColor(osg::Vec4f())
        , mUnderwaterColor(Fallback::Map::getColour("Water_UnderwaterColor"))
        , mUnderwaterWeight(Fallback::Map::getFloat("Water_UnderwaterColorWeight"))
        , mUnderwaterIndoorFog(Fallback::Map::getFloat("Water_UnderwaterIndoorFog"))
    {
    }

    void FogManager::configure(float viewDistance, const MWWorld::Cell& cell)
    {
        osg::Vec4f color = SceneUtil::colourFromRGB(cell.getMood().mFogColor);

        const float fogDensity = cell.getMood().mFogDensity;
        if (Settings::fog().mUseDistantFog)
        {
            float density = std::max(0.2f, fogDensity);
            mLandFogStart = Settings::fog().mDistantInteriorFogEnd * (1.0f - density)
                + Settings::fog().mDistantInteriorFogStart * density;
            mLandFogEnd = Settings::fog().mDistantInteriorFogEnd;
            mUnderwaterFogStart = Settings::fog().mDistantUnderwaterFogStart;
            mUnderwaterFogEnd = Settings::fog().mDistantUnderwaterFogEnd;
            mFogColor = color;
        }
        else
            configure(viewDistance, fogDensity, mUnderwaterIndoorFog, 1.0f, 0.0f, color);
    }

    void FogManager::configure(float viewDistance, float fogDepth, float underwaterFog, float dlFactor, float dlOffset,
        const osg::Vec4f& color)
    {
        if (Settings::fog().mUseDistantFog)
        {
            mLandFogStart
                = dlFactor * (Settings::fog().mDistantLandFogStart - dlOffset * Settings::fog().mDistantLandFogEnd);
            mLandFogEnd = dlFactor * (1.0f - dlOffset) * Settings::fog().mDistantLandFogEnd;
            mUnderwaterFogStart = Settings::fog().mDistantUnderwaterFogStart;
            mUnderwaterFogEnd = Settings::fog().mDistantUnderwaterFogEnd;
        }
        else
        {
            if (fogDepth == 0.0)
            {
                mLandFogStart = 0.0f;
                mLandFogEnd = std::numeric_limits<float>::max();
            }
            else
            {
                mLandFogStart = viewDistance * (1 - fogDepth);
                mLandFogEnd = viewDistance;
            }
            mUnderwaterFogStart = std::min(viewDistance, 7168.f) * (1 - underwaterFog);
            mUnderwaterFogEnd = std::min(viewDistance, 7168.f);
        }
        mFogColor = color;
    }

    float FogManager::getFogStart(bool isUnderwater) const
    {
        return isUnderwater ? mUnderwaterFogStart : mLandFogStart;
    }

    float FogManager::getFogEnd(bool isUnderwater) const
    {
        return isUnderwater ? mUnderwaterFogEnd : mLandFogEnd;
    }

    osg::Vec4f FogManager::getFogColor(bool isUnderwater) const
    {
        if (isUnderwater)
        {
            return mUnderwaterColor * mUnderwaterWeight + mFogColor * (1.f - mUnderwaterWeight);
        }

        return mFogColor;
    }

    FogManager::State FogManager::getState(bool isUnderwater) const
    {
        const float start = getFogStart(isUnderwater);
        const float end = getFogEnd(isUnderwater);
        // OpenGL historically represents disabled land fog as [0, max-float].
        // Preserve that calculation while exposing the semantic toggle needed
        // by explicit APIs and avoiding an invalid zero-width fog interval.
        const bool enabled = end > start && end < std::numeric_limits<float>::max();
        return { getFogColor(isUnderwater), start, end, enabled };
    }

    FogState FogManager::getSemanticState(bool isUnderwater) const
    {
        const State state = getState(isUnderwater);
        const bool exponential = Settings::fog().mExponentialFog;
        return { { state.color.r(), state.color.g(), state.color.b(), state.color.a() }, state.start, state.end,
            (exponential || Settings::fog().mRadialFog) ? FogDistanceMode::Radial : FogDistanceMode::Planar,
            exponential ? FogFalloffMode::Exponential : FogFalloffMode::Linear, state.enabled, isUnderwater };
    }
}
