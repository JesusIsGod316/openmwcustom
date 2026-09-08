#include "v4semanticsource.hpp"

#include "camera.hpp"
#include "celllighting.hpp"
#include "fogmanager.hpp"

#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include <components/misc/convert.hpp>
#include <components/esm3/loadligh.hpp>
#include <components/esm4/loadligh.hpp>
#include <components/sceneutil/lightcommon.hpp>
#include <components/sceneutil/lightutil.hpp>
#include <components/settings/values.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace MWRender
{
    namespace
    {
        [[nodiscard]] glm::mat4 toGlmView(const osg::Matrixf& source) noexcept
        {
            // OSG uses row vectors. Transposing its mathematical matrix gives
            // the equivalent GLM column-vector transform; GLM's [column][row]
            // indexing therefore reads OSG at (column,row).
            glm::mat4 result(1.0f);
            for (std::size_t column = 0; column < 4; ++column)
            {
                for (std::size_t row = 0; row < 4; ++row)
                    result[column][row] = source(column, row);
            }
            return result;
        }

        [[nodiscard]] RenderCore::Color toColor(const osg::Vec4f& source) noexcept
        {
            return { source.r(), source.g(), source.b(), source.a() };
        }

        [[nodiscard]] std::string cellIdentity(const MWWorld::Cell& cell)
        {
            std::string result = "world:" + cell.getWorldSpace().serializeText();
            if (cell.isExterior())
            {
                result += "/exterior:" + std::to_string(cell.getGridX()) + "," + std::to_string(cell.getGridY());
                return result;
            }
            return result + "/interior:" + cell.getId().serializeText();
        }

        [[nodiscard]] RenderCore::LightModulation lightModulation(const SceneUtil::LightCommon& light) noexcept
        {
            // Match the established controller's assignment precedence.
            if (light.mPulseSlow)
                return RenderCore::LightModulation::PulseSlow;
            if (light.mPulse)
                return RenderCore::LightModulation::Pulse;
            if (light.mFlickerSlow)
                return RenderCore::LightModulation::FlickerSlow;
            if (light.mFlicker)
                return RenderCore::LightModulation::Flicker;
            return RenderCore::LightModulation::Constant;
        }

        [[nodiscard]] std::optional<RenderCore::CellLightSource> makeCellLightSource(const MWWorld::Ptr& ptr,
            const SceneUtil::LightCommon& light, std::uint64_t semanticFlags)
        {
            const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
            const std::optional<RenderCore::ActiveCellSource> cell = makeV4ActiveCellSource(*ptr.getCell());
            if (!identity || !cell)
                return std::nullopt;

            const ESM::Position& position = ptr.getRefData().getPosition();
            const float radius = std::max(light.mRadius, 16.f);
            const bool exterior = ptr.getCell()->getCell()->isExterior();
            const SceneUtil::LightAttenuation attenuation = SceneUtil::resolveLightAttenuation(radius, exterior);

            RenderCore::CellLightSource result;
            result.identity = *identity;
            result.cellIdentity = cell->identity;
            result.light.position = { position.pos[0], position.pos[1], position.pos[2] };
            result.light.diffuse = { light.mColor.r(), light.mColor.g(), light.mColor.b(), light.mColor.a() };
            result.light.specular = result.light.diffuse;
            if (light.mNegative)
            {
                result.light.diffuse.r *= -1.f;
                result.light.diffuse.g *= -1.f;
                result.light.diffuse.b *= -1.f;
                result.light.diffuse.a = 1.f;
                result.light.specular = {};
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Negative);
            }
            result.light.constantAttenuation = attenuation.constant;
            result.light.linearAttenuation = attenuation.linear;
            result.light.quadraticAttenuation = attenuation.quadratic;
            result.light.effectiveRadius = radius;
            result.light.modulation = lightModulation(light);
            result.light.semanticFlags = semanticFlags;
            result.light.enabled = !light.mOffDefault;
            if (light.mOffDefault)
                result.light.semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::OffDefault);
            return result;
        }
    }

    std::optional<std::string> makeV4CellIdentity(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell* source = cell.getCell();
        if (!source)
            return std::nullopt;
        return cellIdentity(*source);
    }

    std::optional<std::string> makeV4ReferenceIdentity(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty())
            return std::nullopt;
        const ESM::RefNum refNum = ptr.getCellRef().getRefNum();
        if (!refNum.isSet())
            return std::nullopt;
        return "ref:" + refNum.toString();
    }

    std::optional<RenderCore::ActiveCellSource> makeV4ActiveCellSource(const MWWorld::CellStore& cell)
    {
        const MWWorld::Cell* source = cell.getCell();
        if (!source || cell.getState() != MWWorld::CellStore::State_Loaded)
            return std::nullopt;

        RenderCore::ActiveCellSource result;
        result.identity = *makeV4CellIdentity(cell);
        result.worldspaceIdentity = source->getWorldSpace().serializeText();
        return result;
    }

    std::optional<RenderCore::StaticInstanceSource> makeV4StaticInstanceSource(
        const MWWorld::Ptr& ptr, RenderCore::ModelHandle model, RenderCore::AxisAlignedBounds localBounds)
    {
        if (ptr.isEmpty() || !ptr.getCell() || !model.valid() || !ptr.getRefData().isEnabled()
            || ptr.getClass().isActor() || ptr.getClass().useAnim())
            return std::nullopt;
        const std::optional<std::string> identity = makeV4ReferenceIdentity(ptr);
        const std::optional<RenderCore::ActiveCellSource> cell = makeV4ActiveCellSource(*ptr.getCell());
        if (!identity || !cell)
            return std::nullopt;

        const ESM::Position& position = ptr.getRefData().getPosition();
        const osg::Quat rotation = Misc::Convert::makeOsgQuat(position);
        const float scale = ptr.getCellRef().getScale();
        RenderCore::StaticInstanceSource result;
        result.identity = *identity;
        result.cellIdentity = cell->identity;
        result.model = model;
        result.transform.translation = { position.pos[0], position.pos[1], position.pos[2] };
        result.transform.rotation = { static_cast<float>(rotation.w()), static_cast<float>(rotation.x()),
            static_cast<float>(rotation.y()), static_cast<float>(rotation.z()) };
        result.transform.scale = { scale, scale, scale };
        result.localBounds = localBounds;
        return result;
    }

    std::optional<RenderCore::CellLightSource> makeV4CellLightSource(const MWWorld::Ptr& ptr)
    {
        if (ptr.isEmpty() || !ptr.getCell() || !ptr.getRefData().isEnabled())
            return std::nullopt;

        std::uint64_t semanticFlags = 0;
        if (ptr.getType() == ESM::Light::sRecordId)
        {
            const ESM::Light& source = *ptr.get<ESM::Light>()->mBase;
            if (source.mData.mFlags & ESM::Light::Dynamic)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Dynamic);
            if (source.mData.mFlags & ESM::Light::Carry)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Carryable);
            return makeCellLightSource(ptr, SceneUtil::LightCommon(source), semanticFlags);
        }
        if (ptr.getType() == ESM4::Light::sRecordId)
        {
            const ESM4::Light& source = *ptr.get<ESM4::Light>()->mBase;
            if (source.mData.flags & ESM4::Light::Dynamic)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Dynamic);
            if (source.mData.flags & ESM4::Light::Carryable)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Carryable);
            if (source.mData.flags & ESM4::Light::SpotLight)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::Spot);
            if (source.mData.flags & ESM4::Light::SpotShadow)
                semanticFlags |= RenderCore::lightSemanticFlag(RenderCore::LightSemanticFlag::SpotShadow);
            return makeCellLightSource(ptr, SceneUtil::LightCommon(source), semanticFlags);
        }
        return std::nullopt;
    }

    std::optional<RenderCore::FrameEnvironmentState> makeV4InteriorEnvironmentState(
        const MWWorld::Cell& cell, const FogManager& fog, bool underwater, float nightEyeFactor)
    {
        if (cell.isExterior() || cell.isQuasiExterior() || !std::isfinite(nightEyeFactor))
            return std::nullopt;

        const CellLightingState lighting = resolveCellLighting(cell);
        const FogManager::State fogState = fog.getState(underwater);

        RenderCore::FrameEnvironmentState result;
        result.interior = true;
        result.ambient = toColor(applyNightEyeToAmbient(lighting.ambient, nightEyeFactor));
        result.fogColor = toColor(fogState.color);
        result.fogStart = fogState.start;
        result.fogEnd = fogState.end;
        result.fogFalloffMode = Settings::fog().mExponentialFog ? RenderCore::FogFalloffMode::Exponential
                                                                : RenderCore::FogFalloffMode::Linear;
        // OpenGL forces radial distance whenever exponential fog is selected.
        result.fogDistanceMode = (Settings::fog().mExponentialFog || Settings::fog().mRadialFog)
            ? RenderCore::FogDistanceMode::Radial
            : RenderCore::FogDistanceMode::Planar;
        result.fogEnabled = fogState.enabled;

        const osg::Vec4f direction = -lighting.directionalPosition;
        const glm::vec3 rawDirection{ direction.x(), direction.y(), direction.z() };
        result.sunDirection = glm::normalize(rawDirection);
        result.sunDiffuse = toColor(lighting.directional);
        result.sunSpecular = toColor(lighting.directional);
        result.sunSpecular.a = 0.0f;
        result.sunLightEnabled = true;
        result.sunVisible = false;
        result.skyEnabled = false;
        result.waterEnabled = cell.hasWater();
        result.waterHeight = cell.getWaterHeight();
        result.underwater = underwater;
        return result;
    }

    std::optional<RenderCore::CameraState> makeV4MainCameraState(const Camera& camera,
        RenderCore::Extent2D extent, double verticalFieldOfViewDegrees, double nearPlane, double farPlane)
    {
        if (!extent.valid() || !std::isfinite(verticalFieldOfViewDegrees) || verticalFieldOfViewDegrees <= 0.0
            || verticalFieldOfViewDegrees >= 180.0 || !std::isfinite(nearPlane) || !std::isfinite(farPlane)
            || nearPlane <= 0.0 || farPlane <= nearPlane)
            return std::nullopt;

        RenderCore::CameraState result;
        result.view = toGlmView(camera.getViewMatrix());
        const glm::mat4 cameraWorld = glm::inverse(result.view);
        result.worldPosition = glm::dvec3(cameraWorld[3]);
        result.worldOrientation = glm::normalize(glm::quat_cast(glm::mat3(cameraWorld)));

        constexpr double pi = 3.14159265358979323846;
        const float radians = static_cast<float>(verticalFieldOfViewDegrees * pi / 180.0);
        const float aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        // Swapping far/near is the finite reversed-Z RH_ZO form. Vulkan's
        // framebuffer convention then requires the explicit Y inversion.
        result.projection.matrix
            = glm::perspectiveRH_ZO(radians, aspect, static_cast<float>(farPlane), static_cast<float>(nearPlane));
        result.projection.matrix[1][1] *= -1.0f;
        result.projection.depthRange = RenderCore::ClipDepthRange::ZeroToOne;
        result.projection.depthDirection = RenderCore::DepthDirection::Reversed;
        result.projection.yDirection = RenderCore::ClipYDirection::Down;
        result.projection.nearPlane = nearPlane;
        result.projection.farPlane = farPlane;
        return result;
    }
}
