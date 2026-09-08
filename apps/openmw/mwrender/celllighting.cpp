#include "celllighting.hpp"

#include "../mwworld/cell.hpp"

#include <components/sceneutil/util.hpp>
#include <components/settings/values.hpp>

#include <osg/Math>

namespace MWRender
{
    CellLightingState resolveCellLighting(const MWWorld::Cell& cell)
    {
        CellLightingState result;
        result.ambient = SceneUtil::colourFromRGB(cell.getMood().mAmbiantColor);

        const bool trueInterior = !cell.isExterior() && !cell.isQuasiExterior();
        if (trueInterior && (!Settings::shaders().mClassicFalloff || Settings::shaders().mClusteredLighting))
        {
            constexpr float pR = 0.2126f;
            constexpr float pG = 0.7152f;
            constexpr float pB = 0.0722f;
            const float relativeLuminance
                = pR * result.ambient.r() + pG * result.ambient.g() + pB * result.ambient.b();
            const float minimumAmbientLuminance = Settings::shaders().mMinimumInteriorBrightness;
            if (relativeLuminance < minimumAmbientLuminance)
            {
                if (result.ambient.r() == 0.f && result.ambient.g() == 0.f && result.ambient.b() == 0.f)
                    result.ambient = osg::Vec4f(minimumAmbientLuminance, minimumAmbientLuminance,
                        minimumAmbientLuminance, result.ambient.a());
                else
                    result.ambient *= minimumAmbientLuminance / relativeLuminance;
            }
        }

        result.directional = SceneUtil::colourFromRGB(cell.getMood().mDirectionalColor);
        // This odd vector is the established Morrowind interior-light behavior.
        result.directionalPosition
            = osg::Vec4f(-1.f, osg::DegreesToRadians(45.f), osg::DegreesToRadians(45.f), 0.f);
        return result;
    }

    osg::Vec4f applyNightEyeToAmbient(osg::Vec4f ambient, float nightEyeFactor) noexcept
    {
        if (nightEyeFactor > 0.f)
            ambient += osg::Vec4f(0.7f, 0.7f, 0.7f, 0.0f) * nightEyeFactor;
        return ambient;
    }
}
