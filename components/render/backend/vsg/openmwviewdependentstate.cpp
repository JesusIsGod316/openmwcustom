#include "openmwviewdependentstate.hpp"

#include <vsg/app/Camera.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/app/View.h>
#include <vsg/core/Array.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/ui/FrameStamp.h>
#include <vsg/vk/ResourceRequirements.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace RenderVsg
{
    namespace
    {
        [[nodiscard]] std::uint64_t localLightKey(RenderCore::LightHandle light) noexcept
        {
            return (static_cast<std::uint64_t>(light.slot()) << 32u)
                | static_cast<std::uint64_t>(light.generation());
        }

        [[nodiscard]] std::uint32_t localLightTemporalSeed(
            RenderCore::WorldEpoch epoch, RenderCore::LightHandle light) noexcept
        {
            // Renderer-local deterministic phase ownership keeps reflection,
            // refraction and debug/preview views identical without consuming the
            // gameplay PRNG from VSG record threads. SplitMix64 is used only to
            // derive a stable non-zero minstd_rand-compatible state.
            std::uint64_t value = localLightKey(light) ^ (epoch.value() * 0x9e3779b97f4a7c15ull);
            value = (value ^ (value >> 30u)) * 0xbf58476d1ce4e5b9ull;
            value = (value ^ (value >> 27u)) * 0x94d049bb133111ebull;
            value ^= value >> 31u;
            constexpr std::uint64_t range = 2147483645ull;
            return static_cast<std::uint32_t>(value % range) + 1u;
        }

        [[nodiscard]] float nextClosedProbability(std::uint32_t& state) noexcept
        {
            // std::minstd_rand uses the Park-Miller 48271 multiplier. Keep a
            // private stream per semantic light so temporal rendering never
            // perturbs OpenMW's gameplay RNG. Mapping [1,max] onto [0,1]
            // preserves the legacy LightController target range exactly.
            constexpr std::uint64_t multiplier = 48271ull;
            constexpr std::uint64_t modulus = 2147483647ull;
            state = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state) * multiplier) % modulus);
            return static_cast<float>(state - 1u) / 2147483645.0f;
        }
    }

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
            vsg::vec4(eyeClipPlane.x, eyeClipPlane.y, eyeClipPlane.z, eyeClipPlane.w),
            vsg::vec4(0.0f, 0.0f, 0.0f, 0.0f) };
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

        if (mPlan.worldEpoch.valid() && mPlan.worldEpoch != plan.worldEpoch)
            mLocalLightTemporalStates.clear();
        else
        {
            for (auto it = mLocalLightTemporalStates.begin(); it != mLocalLightTemporalStates.end();)
            {
                const bool retained = std::any_of(plan.lights.begin(), plan.lights.end(), [&](const auto& entry) {
                    return localLightKey(entry.light) == it->first
                        && entry.data.semantics.y != static_cast<std::uint32_t>(RenderCore::LightModulation::Constant);
                });
                if (retained)
                    ++it;
                else
                    it = mLocalLightTemporalStates.erase(it);
            }
        }
        mPlan = std::move(plan);
        return true;
    }

    bool OpenMwViewDependentState::localLightsCurrent(const RenderCore::RenderWorld& world) const noexcept
    {
        return localLightBufferPlanCurrent(world, mPlan);
    }

    float OpenMwViewDependentState::evaluateLocalLightModulation(
        const PackedLocalLightEntry& entry, double simulationTime) const noexcept
    {
        const std::uint32_t encoded = entry.data.semantics.y;
        if (encoded > static_cast<std::uint32_t>(RenderCore::LightModulation::PulseSlow))
            return 1.0f;
        const auto modulation = static_cast<RenderCore::LightModulation>(encoded);
        if (modulation == RenderCore::LightModulation::Constant)
            return 1.0f;

        const std::uint64_t key = localLightKey(entry.light);
        auto [it, inserted] = mLocalLightTemporalStates.try_emplace(key);
        LocalLightTemporalState& state = it->second;
        if (inserted || state.modulation != modulation)
        {
            state = {};
            state.modulation = modulation;
            state.rngState = localLightTemporalSeed(mPlan.worldEpoch, entry.light);
            state.phase = 0.25f + nextClosedProbability(state.rngState) * 0.75f;
        }

        if (!std::isfinite(simulationTime) || simulationTime < 0.0)
            return state.brightness;
        if (!state.started)
        {
            state.started = true;
            state.startTime = simulationTime;
            state.lastTime = 0.0;
            state.ticksToAdvance = 0.0f;
            return state.brightness;
        }

        const double previousAbsoluteTime = state.startTime + state.lastTime;
        if (simulationTime < previousAbsoluteTime)
        {
            // A discontinuous clock belongs to a new temporal segment. World
            // replacement normally clears the state through worldEpoch; this
            // fallback keeps menu/debug clocks finite without applying a
            // negative legacy update step.
            state.startTime = simulationTime;
            state.lastTime = 0.0;
            state.ticksToAdvance = 0.0f;
            return state.brightness;
        }

        // SceneUtil::LightController's current V3.25 behavior: vanilla-like
        // 15 Hz updates with a 0.25/0.75 smoothed tick advance, 0.1 fast and
        // 0.05 slow brightness speed, random flicker targets in [0.25,1], and
        // pulse targets alternating between 0.25 and 1.0.
        constexpr float updateRate = 15.0f;
        state.ticksToAdvance = static_cast<float>(simulationTime - state.startTime - state.lastTime)
                * updateRate * 0.25f
            + state.ticksToAdvance * 0.75f;
        state.lastTime = simulationTime - state.startTime;

        const bool fast = modulation == RenderCore::LightModulation::Flicker
            || modulation == RenderCore::LightModulation::Pulse;
        const float speed = fast ? 0.1f : 0.05f;
        if (state.brightness >= state.phase)
            state.brightness -= state.ticksToAdvance * speed;
        else
            state.brightness += state.ticksToAdvance * speed;

        if (std::abs(state.brightness - state.phase) < speed)
        {
            if (modulation == RenderCore::LightModulation::Flicker
                || modulation == RenderCore::LightModulation::FlickerSlow)
                state.phase = 0.25f + nextClosedProbability(state.rngState) * 0.75f;
            else
                state.phase = state.phase <= 0.5f ? 1.0f : 0.25f;
        }
        return state.brightness;
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
        const vsg::FrameStamp* const frameStamp = traversal.getFrameStamp();
        const double simulationTime = frameStamp ? frameStamp->simulationTime : 0.0;
        if (mOpenMwEnvironmentData)
        {
            OpenMwEnvironmentValues values
                = packOpenMwEnvironment(mEnvironment, mProjection, mEyeClipPlane);
            if (frameStamp)
            {
                const double time = frameStamp->simulationTime;
                if (std::isfinite(time) && time >= 0.0)
                {
                    // SceneUtil::GlowUpdater uses exactly
                    // static_cast<int>(simulationTime * 16) % 32. Keep that
                    // arithmetic while it is representable by int; sessions
                    // beyond that range use the mathematically equivalent
                    // bounded phase without invoking signed-overflow UB.
                    int frameIndex = 0;
                    constexpr double exactLimit
                        = static_cast<double>(std::numeric_limits<int>::max()) / 16.0;
                    if (time <= exactLimit)
                        frameIndex = static_cast<int>(time * 16.0) % 32;
                    else
                        frameIndex = static_cast<int>(std::fmod(time * 16.0, 32.0));
                    values.back().x = static_cast<float>(frameIndex);
                }
            }

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
            const float modulation = evaluateLocalLightModulation(entry, simulationTime);
            const vsg::vec4 values[OpenMwLocalLightVec4Stride] = {
                { static_cast<float>(eyePosition.x), static_cast<float>(eyePosition.y),
                    static_cast<float>(eyePosition.z), source.positionRadius.w },
                { source.diffuse.r * modulation, source.diffuse.g * modulation,
                    source.diffuse.b * modulation, source.diffuse.a * modulation },
                { source.specular.r * modulation, source.specular.g * modulation,
                    source.specular.b * modulation, source.specular.a * modulation },
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
