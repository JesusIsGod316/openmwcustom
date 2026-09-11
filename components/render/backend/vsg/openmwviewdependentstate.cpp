#include "openmwviewdependentstate.hpp"

#include <vsg/app/Camera.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/app/View.h>
#include <vsg/core/Array.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/vk/ResourceRequirements.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace RenderVsg
{
    OpenMwEnvironmentValues packOpenMwEnvironment(const RenderCore::FrameEnvironmentState& environment,
        const RenderCore::ProjectionState& projection, const glm::vec4& eyeClipPlane) noexcept
    {
        return { vsg::vec4(environment.fogColor.r, environment.fogColor.g, environment.fogColor.b,
                     environment.fogColor.a),
            vsg::vec4(environment.fogStart, environment.fogEnd, environment.fogEnabled ? 1.0f : 0.0f,
                static_cast<float>(environment.fogDistanceMode)),
            vsg::vec4(static_cast<float>(projection.nearPlane), static_cast<float>(projection.farPlane),
                static_cast<float>(environment.fogFalloffMode), 0.0f),
            vsg::vec4(environment.skyColor.r, environment.skyColor.g, environment.skyColor.b, environment.skyColor.a),
            vsg::vec4(environment.nightSkyFactor, environment.cloudBlendFactor, environment.cloudSpeed,
                environment.precipitationIntensity),
            vsg::vec4(environment.windDirection.x, environment.windDirection.y, environment.windDirection.z,
                environment.windSpeed),
            vsg::vec4(environment.precipitationEnabled ? 1.0f : 0.0f, environment.storm ? 1.0f : 0.0f,
                environment.skyEnabled ? 1.0f : 0.0f, environment.shadowsEnabled ? 1.0f : 0.0f),
            vsg::vec4(eyeClipPlane.x, eyeClipPlane.y, eyeClipPlane.z, eyeClipPlane.w) };
    }

    OpenMwViewDependentState::OpenMwViewDependentState(vsg::View* view)
        : Inherit(view)
    {
    }

    void OpenMwViewDependentState::init(vsg::ResourceRequirements& requirements)
    {
        if (mOpenMwLightData)
            return;

        vsg::ViewDependentState::init(requirements);
        if (!descriptorSet || !descriptorSetLayout)
            return;
        if (std::any_of(descriptorSetLayout->bindings.begin(), descriptorSetLayout->bindings.end(),
                [](const VkDescriptorSetLayoutBinding& binding) {
                    return binding.binding == OpenMwLocalLightDescriptorBinding
                        || binding.binding == OpenMwEnvironmentDescriptorBinding;
                }))
            throw std::runtime_error("VSG view descriptor binding 5 or 6 conflicts with OpenMW per-view state");

        const std::size_t vec4Count
            = 1u + DefaultMaximumPackedLocalLights * OpenMwLocalLightVec4Stride;
        mOpenMwLightData = vsg::vec4Array::create(vec4Count);
        mOpenMwLightData->setValue("name", "openmwLocalLightData");
        mOpenMwLightData->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
        mOpenMwLightBufferInfo = vsg::BufferInfo::create(mOpenMwLightData.get());
        descriptorSet->descriptors.push_back(vsg::DescriptorBuffer::create(vsg::BufferInfoList{ mOpenMwLightBufferInfo },
            OpenMwLocalLightDescriptorBinding, 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER));
        descriptorSetLayout->bindings.push_back({ OpenMwLocalLightDescriptorBinding,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });

        mOpenMwEnvironmentData = vsg::vec4Array::create(OpenMwEnvironmentVec4Count);
        mOpenMwEnvironmentData->setValue("name", "openmwEnvironmentData");
        mOpenMwEnvironmentData->properties.dataVariance = vsg::DYNAMIC_DATA_TRANSFER_AFTER_RECORD;
        mOpenMwEnvironmentBufferInfo = vsg::BufferInfo::create(mOpenMwEnvironmentData.get());
        descriptorSet->descriptors.push_back(vsg::DescriptorBuffer::create(
            vsg::BufferInfoList{ mOpenMwEnvironmentBufferInfo }, OpenMwEnvironmentDescriptorBinding, 0,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER));
        descriptorSetLayout->bindings.push_back({ OpenMwEnvironmentDescriptorBinding,
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr });
    }

    bool OpenMwViewDependentState::setLocalLights(LocalLightBufferPlan plan)
    {
        if (!plan.ready() || plan.lights.size() > DefaultMaximumPackedLocalLights)
            return false;
        mPlan = std::move(plan);
        return true;
    }

    bool OpenMwViewDependentState::localLightsCurrent(const RenderCore::RenderWorld& world) const noexcept
    {
        return localLightBufferPlanCurrent(world, mPlan);
    }

    void OpenMwViewDependentState::setEnvironment(const RenderCore::FrameEnvironmentState& environment,
        const RenderCore::ProjectionState& projection) noexcept
    {
        mEnvironment = environment;
        mProjection = projection;
    }

    void OpenMwViewDependentState::setClipPlane(
        const std::optional<RenderCore::WorldClipPlane>& clipPlane, const RenderCore::CameraState& camera) noexcept
    {
        mEyeClipPlane = {};
        if (!clipPlane)
            return;
        const glm::vec4 worldPlane(
            clipPlane->normal, static_cast<float>(clipPlane->distance));
        mEyeClipPlane = glm::transpose(glm::inverse(camera.view)) * worldPlane;
    }

    void OpenMwViewDependentState::traverse(vsg::RecordTraversal& traversal) const
    {
        vsg::ViewDependentState::traverse(traversal);
        if (mOpenMwEnvironmentData)
        {
            const OpenMwEnvironmentValues values
                = packOpenMwEnvironment(mEnvironment, mProjection, mEyeClipPlane);
            bool environmentChanged = false;
            auto environmentOutput = mOpenMwEnvironmentData->begin();
            for (const vsg::vec4& value : values)
            {
                environmentChanged = environmentChanged || *environmentOutput != value;
                *environmentOutput++ = value;
            }
            if (environmentChanged)
                mOpenMwEnvironmentData->dirty();
        }

        if (!mOpenMwLightData || !view || !view->camera)
            return;

        auto output = mOpenMwLightData->begin();
        const vsg::vec4 header(static_cast<float>(mPlan.lights.size()), mRadiusFadeEnabled ? 1.0f : 0.0f,
            static_cast<float>(OpenMwLocalLightVec4Stride), 0.0f);
        bool changed = *output != header;
        *output++ = header;

        const vsg::dmat4 viewMatrix = view->camera->viewMatrix->transform();
        for (const PackedLocalLightEntry& entry : mPlan.lights)
        {
            const PackedLocalLight& source = entry.data;
            const glm::dvec3 worldPosition = mPlan.coordinateOrigin
                + glm::dvec3(source.positionRadius.x, source.positionRadius.y, source.positionRadius.z);
            const vsg::dvec3 eyePosition
                = viewMatrix * vsg::dvec3(worldPosition.x, worldPosition.y, worldPosition.z);
            const float enabledFade = source.semantics.x == 0u ? 0.0f : source.attenuationFade.w;
            const vsg::vec4 values[OpenMwLocalLightVec4Stride] = {
                { static_cast<float>(eyePosition.x), static_cast<float>(eyePosition.y),
                    static_cast<float>(eyePosition.z), source.positionRadius.w },
                { source.diffuse.r, source.diffuse.g, source.diffuse.b, source.diffuse.a },
                { source.specular.r, source.specular.g, source.specular.b, source.specular.a },
                { source.ambient.r, source.ambient.g, source.ambient.b, source.ambient.a },
                { source.attenuationFade.x, source.attenuationFade.y, source.attenuationFade.z, enabledFade },
            };
            for (const vsg::vec4& value : values)
            {
                changed = changed || *output != value;
                *output++ = value;
            }
        }

        if (changed)
            mOpenMwLightData->dirty();
    }
}
