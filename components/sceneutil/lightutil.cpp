#include "lightutil.hpp"

#include <osg/Group>

#include <osgParticle/ParticleSystem>

#include <components/esm3/loadligh.hpp>
#include <components/fallback/fallback.hpp>
#include <components/sceneutil/lightcommon.hpp>

#include "lightcontroller.hpp"
#include "lightmanager.hpp"
#include "visitor.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    class CheckEmptyLightVisitor : public osg::NodeVisitor
    {
    public:
        CheckEmptyLightVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Drawable& drawable) override
        {
            if (!mEmpty)
                return;

            if (dynamic_cast<const osgParticle::ParticleSystem*>(&drawable))
                mEmpty = false;
            else
                traverse(drawable);
        }

        void apply(osg::Geometry& geometry) override { mEmpty = false; }

        bool mEmpty = true;
    };
}

namespace SceneUtil
{
    LightAttenuation resolveLightAttenuation(float radius, bool isExterior)
    {
        LightAttenuation result{ 0.f, 0.f, 0.f };

        static const bool useConstant = Fallback::Map::getBool("LightAttenuation_UseConstant");
        static const bool useLinear = Fallback::Map::getBool("LightAttenuation_UseLinear");
        static const bool useQuadratic = Fallback::Map::getBool("LightAttenuation_UseQuadratic");
        // User file might provide nonsense values
        // Clamp these settings to prevent badness (e.g. illegal OpenGL calls)
        static const float constantValue = std::max(Fallback::Map::getFloat("LightAttenuation_ConstantValue"), 0.f);
        static const float linearValue = std::max(Fallback::Map::getFloat("LightAttenuation_LinearValue"), 0.f);
        static const float quadraticValue = std::max(Fallback::Map::getFloat("LightAttenuation_QuadraticValue"), 0.f);
        static const float linearRadiusMult
            = std::max(Fallback::Map::getFloat("LightAttenuation_LinearRadiusMult"), 0.f);
        static const float quadraticRadiusMult
            = std::max(Fallback::Map::getFloat("LightAttenuation_QuadraticRadiusMult"), 0.f);
        static const int linearMethod = Fallback::Map::getInt("LightAttenuation_LinearMethod");
        static const int quadraticMethod = Fallback::Map::getInt("LightAttenuation_QuadraticMethod");
        static const bool outQuadInLin = Fallback::Map::getBool("LightAttenuation_OutQuadInLin");

        if (useConstant)
            result.constant = constantValue;

        if (useLinear)
        {
            result.linear = linearMethod == 0 ? linearValue : 0.01f;
            float r = radius * linearRadiusMult;
            if (r > 0.f && (linearMethod == 1 || linearMethod == 2))
                result.linear = linearValue / std::pow(r, static_cast<float>(linearMethod));
        }

        if (useQuadratic && (!outQuadInLin || isExterior))
        {
            result.quadratic = quadraticMethod == 0 ? quadraticValue : 0.01f;
            float r = radius * quadraticRadiusMult;
            if (r > 0.f && (quadraticMethod == 1 || quadraticMethod == 2))
                result.quadratic = quadraticValue / std::pow(r, static_cast<float>(quadraticMethod));
        }

        // If the values are still nonsense, try to at least prevent UB and disable attenuation
        if (result.constant == 0.f && result.linear == 0.f && result.quadratic == 0.f)
            result.constant = 1.f;

        return result;
    }

    void configureLight(SceneUtil::Light* light, float radius, bool isExterior)
    {
        const LightAttenuation attenuation = resolveLightAttenuation(radius, isExterior);
        light->setConstantAttenuation(attenuation.constant);
        light->setLinearAttenuation(attenuation.linear);
        light->setQuadraticAttenuation(attenuation.quadratic);
    }

    osg::ref_ptr<LightSource> addLight(
        osg::Group* node, const SceneUtil::LightCommon& esmLight, unsigned int lightMask, bool isExterior)
    {
        SceneUtil::FindByNameVisitor visitor("AttachLight");
        node->accept(visitor);

        osg::Group* attachTo = visitor.mFoundNode ? visitor.mFoundNode : node;
        osg::ref_ptr<LightSource> lightSource
            = createLightSource(esmLight, lightMask, isExterior, osg::Vec4f(0, 0, 0, 1));
        attachTo->addChild(lightSource);

        CheckEmptyLightVisitor emptyVisitor;
        node->accept(emptyVisitor);

        lightSource->setEmpty(emptyVisitor.mEmpty);

        return lightSource;
    }

    osg::ref_ptr<LightSource> createLightSource(
        const SceneUtil::LightCommon& esmLight, unsigned int lightMask, bool isExterior, const osg::Vec4f& ambient)
    {
        osg::ref_ptr<SceneUtil::LightSource> lightSource(new SceneUtil::LightSource);
        osg::ref_ptr<SceneUtil::Light> light(new SceneUtil::Light);
        lightSource->setNodeMask(lightMask);

        // The minimum scene light radius is 16 in Morrowind
        const float radius = std::max(esmLight.mRadius, 16.f);
        lightSource->setRadius(radius);

        configureLight(light, radius, isExterior);

        osg::Vec4f diffuse = esmLight.mColor;
        osg::Vec4f specular = esmLight.mColor; // ESM format doesn't provide specular
        if (esmLight.mNegative)
        {
            diffuse *= -1;
            diffuse.a() = 1;
            // Using specular lighting for negative lights is unreasonable
            specular = osg::Vec4f();
        }
        light->setDiffuse(diffuse);
        light->setAmbient(ambient);
        light->setSpecular(specular);

        lightSource->setLight(light);

        osg::ref_ptr<SceneUtil::LightController> ctrl(new SceneUtil::LightController);
        ctrl->setDiffuse(light->getDiffuse());
        ctrl->setSpecular(light->getSpecular());
        if (esmLight.mFlicker)
            ctrl->setType(SceneUtil::LightController::LT_Flicker);
        if (esmLight.mFlickerSlow)
            ctrl->setType(SceneUtil::LightController::LT_FlickerSlow);
        if (esmLight.mPulse)
            ctrl->setType(SceneUtil::LightController::LT_Pulse);
        if (esmLight.mPulseSlow)
            ctrl->setType(SceneUtil::LightController::LT_PulseSlow);

        lightSource->addUpdateCallback(ctrl);

        return lightSource;
    }
}
