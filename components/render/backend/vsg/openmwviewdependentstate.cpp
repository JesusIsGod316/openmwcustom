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
                    return binding.binding == OpenMwLocalLightDescriptorBinding;
                }))
            throw std::runtime_error("VSG view descriptor binding 5 is no longer available for OpenMW local lights");

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

    void OpenMwViewDependentState::traverse(vsg::RecordTraversal& traversal) const
    {
        vsg::ViewDependentState::traverse(traversal);
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
