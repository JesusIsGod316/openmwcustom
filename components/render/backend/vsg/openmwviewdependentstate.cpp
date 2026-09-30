#include "openmwviewdependentstate.hpp"
#include "locallighttiles.hpp"

#include <vsg/app/Camera.h>
#include <vsg/app/RecordTraversal.h>
#include <vsg/app/View.h>
#include <vsg/core/Array.h>
#include <vsg/state/BufferInfo.h>
#include <vsg/state/DescriptorBuffer.h>
#include <vsg/state/DescriptorSet.h>
#include <vsg/state/DescriptorSetLayout.h>
#include <vsg/state/ViewportState.h>
#include <vsg/ui/FrameStamp.h>
#include <vsg/lighting/AmbientLight.h>
#include <vsg/lighting/DirectionalLight.h>
#include <vsg/lighting/HardShadows.h>
#include <vsg/lighting/PointLight.h>
#include <vsg/lighting/SpotLight.h>
#include <vsg/maths/transform.h>
#include <vsg/vk/ResourceRequirements.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdlib>
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
            std::getenv("OPENMW_V4_LEGACY_SUN_SPECULAR_CONTROL")
                ? vsg::vec4(0.0f, 1.0f, 1.0f, 1.0f)
                : vsg::vec4(0.0f, environment.sunSpecular.r, environment.sunSpecular.g,
                    environment.sunSpecular.b) };
    }

    OpenMwViewDependentState::OpenMwViewDependentState(vsg::View* view)
        : Inherit(view)
    {
    }

    void OpenMwViewDependentState::init(vsg::ResourceRequirements& requirements)
    {
        if (mOpenMwLightData)
            return;

        // Reflection/refraction/maps do not record shadow passes. Let VSG make
        // its initialized one-pixel fallback, rather than reserving an array
        // whose initial layout is only established by RECORD_SHADOW_MAPS.
        const auto shadowRange = requirements.numShadowMapsRange;
        if ((view->features & vsg::RECORD_SHADOW_MAPS) == 0)
            requirements.numShadowMapsRange = {0, 0};
        try { vsg::ViewDependentState::init(requirements); }
        catch (...) { requirements.numShadowMapsRange = shadowRange; throw; }
        requirements.numShadowMapsRange = shadowRange;
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

        // VSG creates stock per-view state for its shadow cameras. Our shared
        // legacy pipelines still bind the OpenMW view layout there, including
        // bindings 5/6 (also required by alpha-tested shadow draws).
        for (auto& shadow : shadowMaps)
        {
            auto state = OpenMwViewDependentState::create(shadow.view.get());
            state->shaderSet = shaderSet;
            shadow.view->viewDependentState = state;
        }
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

    bool OpenMwViewDependentState::localLightsCurrent(
        const RenderCore::RenderWorld& world, std::uint64_t sourceSerial) const noexcept
    {
        return sourceSerial != 0 && mPlan.ready() && mPlan.worldEpoch == world.epoch()
            && mPlan.sourceSerial == sourceSerial;
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

    void OpenMwViewDependentState::updateUnshadowedLightData() const
    {
        if (!lightData)
            return;
        // Pinned VSG 1.1.15 returns before packing even ambient/directional
        // lights when RECORD_SHADOW_MAPS is absent. Previews, reflection and
        // refraction intentionally omit that flag. Populate the same supported
        // light-buffer layout here WITHOUT enabling or recording shadow maps.
        const std::size_t required = 1u + ambientLights.size() + 3u * directionalLights.size()
            + 2u * pointLights.size() + 4u * spotLights.size();
        if (required > lightData->size())
            throw std::runtime_error("unshadowed view light data exceeds its compiled buffer capacity"
                " [view=" + std::to_string(view->viewID) + ", features="
                + std::to_string(static_cast<unsigned int>(view->features)) + ", required_vec4="
                + std::to_string(required) + ", capacity_vec4=" + std::to_string(lightData->size())
                + ", ambient=" + std::to_string(ambientLights.size()) + ", directional="
                + std::to_string(directionalLights.size()) + ", point=" + std::to_string(pointLights.size())
                + ", spot=" + std::to_string(spotLights.size()) + "]");
        auto output = lightData->begin();
        bool changed = false;
        const auto write = [&](const vsg::vec4& value) {
            changed = changed || *output != value;
            *output++ = value;
        };
        const auto color = [&](const auto& light) {
            write({ light->color.r, light->color.g, light->color.b, light->intensity });
        };
        write({ static_cast<float>(ambientLights.size()), static_cast<float>(directionalLights.size()),
            static_cast<float>(pointLights.size()), static_cast<float>(spotLights.size()) });
        for (const auto& entry : ambientLights)
            color(entry.second);
        for (const auto& [matrix, light] : directionalLights)
        {
            const auto direction = vsg::normalize(light->direction * vsg::inverse_3x3(matrix));
            color(light);
            write({ static_cast<float>(direction.x), static_cast<float>(direction.y),
                static_cast<float>(direction.z), 0.0f });
            write({ 0.0f, 0.0f, 0.0f, 0.0f }); // No shadow descriptors/matrices follow this light.
        }
        for (const auto& [matrix, light] : pointLights)
        {
            const auto position = matrix * light->position;
            color(light);
            write({ static_cast<float>(position.x), static_cast<float>(position.y),
                static_cast<float>(position.z), 0.0f });
        }
        for (const auto& [matrix, light] : spotLights)
        {
            const auto position = matrix * light->position;
            const auto direction = vsg::normalize(light->direction * vsg::inverse_3x3(matrix));
            color(light);
            write({ static_cast<float>(position.x), static_cast<float>(position.y),
                static_cast<float>(position.z), static_cast<float>(std::cos(light->innerAngle)) });
            write({ static_cast<float>(direction.x), static_cast<float>(direction.y),
                static_cast<float>(direction.z), static_cast<float>(std::cos(light->outerAngle)) });
            write({ 0.0f, 0.0f, 0.0f, 0.0f });
        }
        if (changed)
            lightData->dirty();
    }

    bool OpenMwViewDependentState::recordStableSunShadows(vsg::RecordTraversal& traversal) const
    {
        if (!view || !view->camera || !lightData || !(view->features & vsg::RECORD_SHADOW_MAPS))
            return false;
        // Keep the library path for shadow modes not implemented by this fitter.
        for (const auto& entry : directionalLights)
            if (auto settings = getActiveShadowSettings(entry.second);
                settings && settings->type_info() != typeid(vsg::HardShadows))
                return false;
        for (const auto& entry : spotLights)
            if (auto settings = getActiveShadowSettings(entry.second); settings && settings->shadowMapCount)
                return false;

        const auto projection = view->camera->projectionMatrix->transform();
        const auto viewMatrix = view->camera->viewMatrix->transform();
        const auto inverseView = vsg::inverse(viewMatrix);
        const auto inverseProjection = vsg::inverse(projection);
        const double nearDistance = -(inverseProjection * vsg::dvec3(0, 0, 1)).z;
        const double farDistance = std::min(maxShadowDistance,
            -(inverseProjection * vsg::dvec3(0, 0, 0)).z);
        if (!(nearDistance > 0 && farDistance > nearDistance && std::isfinite(farDistance)))
            return false;

        if (preRenderSwitch) preRenderSwitch->setAllChildren(false);
        const auto capacity = preRenderSwitch ? std::min(shadowMaps.size(), preRenderSwitch->children.size()) : 0;
        std::size_t shadowIndex = 0;
        auto output = lightData->begin();
        bool changed = false;
        const auto write = [&](const vsg::vec4& value) {
            if (output == lightData->end()) throw std::runtime_error("stable shadow light buffer capacity exceeded");
            changed = changed || *output != value;
            *output++ = value;
        };
        const auto color = [&](const vsg::Light* light) {
            write({light->color.r, light->color.g, light->color.b, light->intensity});
        };
        const auto writeMatrix = [&](const vsg::dmat4& matrix) {
            for (unsigned i = 0; i < 4; ++i) write(vsg::vec4(matrix[i]));
        };
        write({float(ambientLights.size()), float(directionalLights.size()),
            float(pointLights.size()), float(spotLights.size())});
        for (const auto& entry : ambientLights) color(entry.second);
        for (const auto& [modelView, light] : directionalLights)
        {
            color(light);
            const auto eyeDirection = vsg::normalize(light->direction * vsg::inverse_3x3(modelView));
            write({float(eyeDirection.x), float(eyeDirection.y), float(eyeDirection.z), 0});
            const auto settings = getActiveShadowSettings(light);
            const std::size_t count = settings ? std::min<std::size_t>(settings->shadowMapCount, capacity - shadowIndex) : 0;
            write({float(count), -1, -1, 0});
            if (!count) continue;

            // A direction has w=0. Never project it as a position: the stock
            // fitter's vec3 * projection-view expression divides by a camera-
            // dependent homogeneous w, producing NaNs even at the world origin.
            // A world-fixed light basis also avoids rotating the shadow grid
            // with the main camera, independently of that singularity.
            const auto direction = vsg::normalize(light->direction * vsg::inverse_3x3(modelView * inverseView));
            const vsg::dvec3 axis = std::abs(direction.z) < .9 ? vsg::dvec3(0, 0, 1) : vsg::dvec3(0, 1, 0);
            const auto side = vsg::normalize(vsg::cross(direction, axis));
            const auto up = vsg::cross(side, direction);
            const auto split = [&](std::size_t index) {
                const double fraction = double(index) / double(count);
                return std::clamp(lambda, 0.0, 1.0) * nearDistance * std::pow(farDistance / nearDistance, fraction)
                    + (1.0 - std::clamp(lambda, 0.0, 1.0)) * (nearDistance + (farDistance - nearDistance) * fraction);
            };
            for (std::size_t cascade = 0; cascade < count; ++cascade, ++shadowIndex)
            {
                std::array<vsg::dvec3, 8> eyeCorners;
                unsigned corner = 0;
                for (const double distance : {split(cascade), split(cascade + 1)})
                {
                    const double depth = (projection * vsg::dvec3(0, 0, -distance)).z;
                    for (double x : {-1.0, 1.0})
                        for (double y : {-1.0, 1.0})
                            eyeCorners[corner++] = inverseProjection * vsg::dvec3(x, y, depth);
                }
                vsg::dvec3 center;
                for (const auto& point : eyeCorners) center += point / 8.0;
                double radius = 0;
                for (const auto& point : eyeCorners) radius = std::max(radius, vsg::length(point - center));
                radius = std::ceil(std::max(.01, radius) * 16.0) / 16.0;
                center = inverseView * center;

                const auto& camera = shadowMaps[shadowIndex].view->camera;
                const auto extent = shadowMaps[shadowIndex].renderGraph->renderArea.extent;
                // Reserve one texel around the enclosing sphere before snapping
                // its center so receiver coverage cannot shrink at a grid edge.
                const double resolution = std::max(4u, std::min(extent.width, extent.height));
                radius *= resolution / (resolution - 2.0);
                const double texel = 2.0 * radius / resolution;
                center += side * (std::round(vsg::dot(center, side) / texel) * texel - vsg::dot(center, side));
                center += up * (std::round(vsg::dot(center, up) / texel) * texel - vsg::dot(center, up));
                auto lookAt = camera->viewMatrix.cast<vsg::LookAt>();
                auto ortho = camera->projectionMatrix.cast<vsg::Orthographic>();
                if (!lookAt) camera->viewMatrix = lookAt = vsg::LookAt::create();
                if (!ortho) camera->projectionMatrix = ortho = vsg::Orthographic::create();
                // Include off-camera casters upstream of the visible receivers.
                // Main-view visibility must not decide shadow participation.
                const double casterReach = farDistance;
                lookAt->eye = center - direction * (radius + casterReach);
                lookAt->center = lookAt->eye + direction;
                lookAt->up = up;
                ortho->left = ortho->bottom = -radius;
                ortho->right = ortho->top = radius;
                ortho->nearDistance = .001;
                ortho->farDistance = 2.0 * radius + casterReach;
                const double bias = shadowMapBias * (2.0 * radius) / ortho->farDistance;
                const auto matrix = vsg::scale(.5, .5, 1.0) * vsg::translate(1.0, 1.0, bias)
                    * ortho->transform() * lookAt->transform() * inverseView;
                writeMatrix(matrix);
                writeMatrix(vsg::inverse(matrix));
                preRenderSwitch->children[shadowIndex].mask = vsg::MASK_ALL;
            }
        }
        for (const auto& [matrix, light] : pointLights)
        {
            const auto position = matrix * light->position;
            color(light);
            write({float(position.x), float(position.y), float(position.z), 0});
        }
        for (const auto& [matrix, light] : spotLights)
        {
            const auto position = matrix * light->position;
            const auto direction = vsg::normalize(light->direction * vsg::inverse_3x3(matrix));
            color(light);
            write({float(position.x), float(position.y), float(position.z), float(std::cos(light->innerAngle))});
            write({float(direction.x), float(direction.y), float(direction.z), float(std::cos(light->outerAngle))});
            write({0, 0, 0, 0});
        }
        if (changed) lightData->dirty();
        if (shadowIndex && preRenderCommandGraph)
        {
            if (traversal.instrumentation && !preRenderCommandGraph->instrumentation)
                preRenderCommandGraph->instrumentation = traversal.instrumentation->shareOrDuplicateForThreadSafety();
            preRenderCommandGraph->accept(traversal);
        }
        return true;
    }

    void OpenMwViewDependentState::traverse(vsg::RecordTraversal& traversal) const
    {
        // Absence of RECORD_SHADOW_MAPS does not imply a lit view. VSG's
        // generated depth-only shadow cameras use INHERIT_VIEWPOINT, reserve
        // only the light-count header, and can still collect parent Light nodes.
        // Preserve their depth-only contract; only RECORD_LIGHTS views need our
        // unshadowed lighting upload. Do not grow buffers or allocate shadow maps.
        if (view && (view->features & vsg::RECORD_LIGHTS) != 0
            && (view->features & vsg::RECORD_SHADOW_MAPS) == 0
            && std::getenv("OPENMW_V4_LEGACY_UNSHADOWED_LIGHTS_CONTROL") == nullptr)
            updateUnshadowedLightData();
        else if (!(std::getenv("OPENMW_VK_STABLE_SUN_SHADOWS") && recordStableSunShadows(traversal)))
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
        std::vector<LocalLightTileSource> tileSources;
        if (std::getenv("OPENMW_VK_TILED_LIGHTS") && mRadiusFadeEnabled && mPlan.lights.size() >= 16)
            tileSources.reserve(mPlan.lights.size());
        struct PostLight { double distance; std::array<vsg::vec4, 3> values; };
        std::vector<PostLight> postLights;
        if (mPostProcessingLights) postLights.reserve(mPlan.lights.size());
        for (const PackedLocalLightEntry& entry : mPlan.lights)
        {
            const PackedLocalLight& source = entry.data;
            const glm::dvec3 worldPosition = mPlan.coordinateOrigin
                + glm::dvec3(source.positionRadius.x, source.positionRadius.y, source.positionRadius.z);
            const vsg::dvec3 eyePosition
                = viewMatrix * vsg::dvec3(worldPosition.x, worldPosition.y, worldPosition.z);
            const float enabledFade = source.semantics.x == 0u ? 0.0f : source.attenuationFade.w;
            const float modulation = evaluateLocalLightModulation(entry, simulationTime);
            if (mPostProcessingLights && enabledFade > 0.0f)
                postLights.push_back({vsg::length2(eyePosition), {{
                    {static_cast<float>(worldPosition.x), static_cast<float>(worldPosition.y), static_cast<float>(worldPosition.z), 1.0f},
                    {source.diffuse.r * modulation * enabledFade, source.diffuse.g * modulation * enabledFade,
                        source.diffuse.b * modulation * enabledFade, 1.0f},
                    {source.attenuationFade.x, source.attenuationFade.y, source.attenuationFade.z, source.positionRadius.w}}}});
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
            if (tileSources.capacity())
                tileSources.push_back({{values[0].x, values[0].y, values[0].z}, values[0].w, enabledFade});
            for (const vsg::vec4& value : values)
            {
                changed = changed || *output != value;
                *output++ = value;
            }
        }

        if (!tileSources.empty() && view->camera->viewportState
            && view->camera->viewportState->viewports.size() == 1)
        {
            const VkViewport& viewport = view->camera->viewportState->viewports.front();
            if (viewport.x == 0.0f && viewport.y == 0.0f && viewport.width > 0.0f
                && viewport.height > 0.0f && viewport.width <= 131072.0f
                && viewport.height <= 131072.0f
                && std::floor(viewport.width) == viewport.width
                && std::floor(viewport.height) == viewport.height)
            {
                const auto grid = buildLocalLightTileGrid(tileSources,
                    view->camera->projectionMatrix->transform(), mProjection.nearPlane,
                    static_cast<std::uint32_t>(viewport.width), static_cast<std::uint32_t>(viewport.height),
                    mOpenMwLightData->size() - 1, mRadiusFadeEnabled);
                if (grid.active())
                {
                    // Append bit masks after the five-vec4 light records in
                    // the already allocated per-view storage buffer. No new
                    // descriptor or secondary upload is needed.
                    auto* maskOutput = mOpenMwLightData->data()
                        + 1 + mPlan.lights.size() * OpenMwLocalLightVec4Stride;
                    for (std::size_t word = 0; word < grid.words.size(); word += 4)
                    {
                        maskOutput[word / 4] = vsg::vec4(
                            std::bit_cast<float>(grid.words[word]),
                            std::bit_cast<float>(grid.words[word + 1]),
                            std::bit_cast<float>(grid.words[word + 2]),
                            std::bit_cast<float>(grid.words[word + 3]));
                    }
                    mOpenMwLightData->at(0).w = static_cast<float>(grid.columns);
                    changed = true;
                }
            }
        }

        if (changed)
            mOpenMwLightData->dirty();
        if (mPostProcessingLights)
        {
            const std::size_t count = std::min<std::size_t>(40, postLights.size());
            std::partial_sort(postLights.begin(), postLights.begin() + count, postLights.end(),
                [](const PostLight& a, const PostLight& b) { return a.distance < b.distance; });
            std::array<vsg::vec4, 120> values{};
            for (std::size_t i = 0; i < count; ++i)
                std::copy(postLights[i].values.begin(), postLights[i].values.end(), values.begin() + i * 3);
            static_assert(sizeof(values) == 1920);
            const std::int32_t lightCount = static_cast<std::int32_t>(count);
            std::memcpy(mPostProcessingLights->dataPointer(), values.data(), sizeof(values));
            std::memcpy(mPostProcessingLights->data() + sizeof(values), &lightCount, sizeof(lightCount));
            mPostProcessingLights->dirty();
        }
    }
}
