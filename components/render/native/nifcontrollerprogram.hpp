#ifndef OPENMW_COMPONENTS_RENDER_NATIVE_NIFCONTROLLERPROGRAM_H
#define OPENMW_COMPONENTS_RENDER_NATIVE_NIFCONTROLLERPROGRAM_H

#include <components/rendercore/records.hpp>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Nif
{
    class FileView;
    struct NiKeyframeController;
    struct NiTimeController;
}

namespace NifRender
{
    struct TranslationBundle;
}

namespace RenderNative
{
    enum class ControllerInterpolation : std::uint8_t
    {
        Linear,
        Hermite,
        Constant,
    };

    enum class ControllerExtrapolation : std::uint8_t
    {
        Cycle,
        Reverse,
        Constant,
    };

    enum class ControllerAxisOrder : std::uint8_t
    {
        XYZ,
        XZY,
        YZX,
        YXZ,
        ZXY,
        ZYX,
        XYX,
        YZY,
        ZXZ,
    };

    struct ControllerTiming
    {
        float frequency = 1.0f;
        float phase = 0.0f;
        float start = 0.0f;
        float stop = 0.0f;
        ControllerExtrapolation extrapolation = ControllerExtrapolation::Constant;

        [[nodiscard]] bool valid() const noexcept
        {
            return std::isfinite(frequency) && std::isfinite(phase) && std::isfinite(start) && std::isfinite(stop);
        }

        // Mirrors NifOsg::ControllerFunction exactly so native playback can be
        // compared against the current OpenGL compatibility path.
        [[nodiscard]] float map(float value) const noexcept
        {
            float time = frequency * value + phase;
            if (time >= start && time <= stop)
                return time;

            const float delta = stop - start;
            switch (extrapolation)
            {
                case ControllerExtrapolation::Cycle:
                {
                    if (delta <= 0.0f)
                        return start;
                    const float cycles = (time - start) / delta;
                    const float remainder = (cycles - std::floor(cycles)) * delta;
                    return start + remainder;
                }
                case ControllerExtrapolation::Reverse:
                {
                    if (delta <= 0.0f)
                        return start;
                    const float cycles = (time - start) / delta;
                    const float floorCycles = std::floor(cycles);
                    const float remainder = (cycles - floorCycles) * delta;
                    if ((static_cast<int>(std::fabs(floorCycles)) % 2) == 0)
                        return start + remainder;
                    return stop - remainder;
                }
                case ControllerExtrapolation::Constant:
                default:
                    if (time < start)
                        return start;
                    if (time > stop)
                        return stop;
                    return time;
            }
        }
    };

    template <class T>
    struct ControllerKey
    {
        float time = 0.0f;
        T value{};
        T inTangent{};
        T outTangent{};
    };

    namespace controller_detail
    {
        template <class T>
        [[nodiscard]] const ControllerKey<T>* highKey(
            const std::vector<ControllerKey<T>>& keys, float time) noexcept
        {
            const auto it = std::lower_bound(keys.begin(), keys.end(), time,
                [](const ControllerKey<T>& key, float value) { return key.time < value; });
            return it == keys.end() ? nullptr : &*it;
        }

        template <class T>
        [[nodiscard]] std::optional<T> sampleLinearHermite(
            const std::vector<ControllerKey<T>>& keys, ControllerInterpolation interpolation, float time) noexcept
        {
            if (keys.empty())
                return std::nullopt;
            if (time <= keys.front().time)
                return keys.front().value;

            const ControllerKey<T>* high = highKey(keys, time);
            if (!high)
                return keys.back().value;
            const std::size_t highIndex = static_cast<std::size_t>(high - keys.data());
            if (highIndex == 0)
                return high->value;
            const ControllerKey<T>& low = keys[highIndex - 1];
            if (high->time == low.time)
                return low.value;

            const float fraction = (time - low.time) / (high->time - low.time);
            if (interpolation == ControllerInterpolation::Constant)
                return fraction > 0.5f ? high->value : low.value;
            if (interpolation == ControllerInterpolation::Hermite)
            {
                const float t2 = fraction * fraction;
                const float t3 = t2 * fraction;
                const float b1 = 2.0f * t3 - 3.0f * t2 + 1.0f;
                const float b2 = -2.0f * t3 + 3.0f * t2;
                const float b3 = t3 - 2.0f * t2 + fraction;
                const float b4 = t3 - t2;
                return low.value * b1 + high->value * b2 + low.outTangent * b3 + high->inTangent * b4;
            }
            return low.value + (high->value - low.value) * fraction;
        }
    }

    struct FloatControllerTrack
    {
        ControllerInterpolation interpolation = ControllerInterpolation::Linear;
        std::vector<ControllerKey<float>> keys;

        [[nodiscard]] bool empty() const noexcept { return keys.empty(); }
        [[nodiscard]] std::optional<float> sample(float time) const noexcept
        {
            return controller_detail::sampleLinearHermite(keys, interpolation, time);
        }
    };

    struct Vec3ControllerTrack
    {
        ControllerInterpolation interpolation = ControllerInterpolation::Linear;
        std::vector<ControllerKey<glm::vec3>> keys;

        [[nodiscard]] bool empty() const noexcept { return keys.empty(); }
        [[nodiscard]] std::optional<glm::vec3> sample(float time) const noexcept
        {
            return controller_detail::sampleLinearHermite(keys, interpolation, time);
        }
    };

    struct QuaternionControllerTrack
    {
        ControllerInterpolation interpolation = ControllerInterpolation::Linear;
        std::vector<ControllerKey<glm::quat>> keys;

        [[nodiscard]] bool empty() const noexcept { return keys.empty(); }
        [[nodiscard]] std::optional<glm::quat> sample(float time) const noexcept
        {
            if (keys.empty())
                return std::nullopt;
            if (time <= keys.front().time)
                return keys.front().value;

            const ControllerKey<glm::quat>* high = controller_detail::highKey(keys, time);
            if (!high)
                return keys.back().value;
            const std::size_t highIndex = static_cast<std::size_t>(high - keys.data());
            if (highIndex == 0)
                return high->value;
            const ControllerKey<glm::quat>& low = keys[highIndex - 1];
            if (high->time == low.time)
                return low.value;

            const float fraction = (time - low.time) / (high->time - low.time);
            if (interpolation == ControllerInterpolation::Constant)
                return fraction > 0.5f ? high->value : low.value;
            // OpenMW's compatibility controller uses spherical interpolation for
            // quaternion Linear/Quadratic/TCB tracks alike.
            return glm::slerp(low.value, high->value, fraction);
        }
    };

    struct BoolControllerKey
    {
        float time = 0.0f;
        bool value = false;
        float inTangent = 0.0f;
        float outTangent = 0.0f;
    };

    struct BoolControllerTrack
    {
        ControllerInterpolation interpolation = ControllerInterpolation::Constant;
        bool legacyStepPrevious = false;
        std::vector<BoolControllerKey> keys;

        [[nodiscard]] bool empty() const noexcept { return keys.empty(); }

        [[nodiscard]] std::optional<bool> sample(float time) const noexcept
        {
            if (keys.empty())
                return std::nullopt;
            if (legacyStepPrevious)
            {
                auto it = std::upper_bound(keys.begin(), keys.end(), time,
                    [](float value, const BoolControllerKey& key) { return value < key.time; });
                if (it != keys.begin())
                    --it;
                return it->value;
            }
            if (time <= keys.front().time)
                return keys.front().value;
            const auto it = std::lower_bound(keys.begin(), keys.end(), time,
                [](const BoolControllerKey& key, float value) { return key.time < value; });
            if (it == keys.end())
                return keys.back().value;
            if (it == keys.begin())
                return it->value;
            const BoolControllerKey& high = *it;
            const BoolControllerKey& low = *(it - 1);
            if (high.time == low.time)
                return low.value;
            const float fraction = (time - low.time) / (high.time - low.time);
            if (interpolation == ControllerInterpolation::Constant)
                return fraction > 0.5f ? high.value : low.value;

            const float lowValue = low.value ? 1.0f : 0.0f;
            const float highValue = high.value ? 1.0f : 0.0f;
            float value = 0.0f;
            if (interpolation == ControllerInterpolation::Hermite)
            {
                const float t2 = fraction * fraction;
                const float t3 = t2 * fraction;
                const float b1 = 2.0f * t3 - 3.0f * t2 + 1.0f;
                const float b2 = -2.0f * t3 + 3.0f * t2;
                const float b3 = t3 - 2.0f * t2 + fraction;
                const float b4 = t3 - t2;
                value = lowValue * b1 + highValue * b2 + low.outTangent * b3 + high.inTangent * b4;
            }
            else
                value = lowValue + (highValue - lowValue) * fraction;
            return static_cast<bool>(value);
        }
    };

    struct TransformTrackSample
    {
        std::optional<glm::vec3> translation;
        std::optional<glm::quat> rotation;
        std::optional<float> scale;
    };

    struct TransformControllerTrack
    {
        QuaternionControllerTrack rotations;
        FloatControllerTrack xRotations;
        FloatControllerTrack yRotations;
        FloatControllerTrack zRotations;
        Vec3ControllerTrack translations;
        FloatControllerTrack scales;
        ControllerAxisOrder axisOrder = ControllerAxisOrder::XYZ;

        // NiTransformInterpolator supplies authored defaults for channels which
        // have no key data. The legacy NifOsg controller consumes these defaults,
        // so the native runtime must retain them before it can replace evaluated
        // scenegraph capture.
        std::optional<glm::vec3> defaultTranslation;
        std::optional<glm::quat> defaultRotation;
        std::optional<float> defaultScale;

        [[nodiscard]] TransformTrackSample sample(float time) const noexcept
        {
            TransformTrackSample result;
            result.translation = translations.sample(time);
            if (!result.translation)
                result.translation = defaultTranslation;

            result.rotation = rotations.sample(time);
            if (!result.rotation && (!xRotations.empty() || !yRotations.empty() || !zRotations.empty()))
            {
                const float x = xRotations.sample(time).value_or(0.0f);
                const float y = yRotations.sample(time).value_or(0.0f);
                const float z = zRotations.sample(time).value_or(0.0f);
                const glm::quat xr = glm::angleAxis(x, glm::vec3(1.0f, 0.0f, 0.0f));
                const glm::quat yr = glm::angleAxis(y, glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::quat zr = glm::angleAxis(z, glm::vec3(0.0f, 0.0f, 1.0f));
                switch (axisOrder)
                {
                    case ControllerAxisOrder::XYZ: result.rotation = xr * yr * zr; break;
                    case ControllerAxisOrder::XZY: result.rotation = xr * zr * yr; break;
                    case ControllerAxisOrder::YZX: result.rotation = yr * zr * xr; break;
                    case ControllerAxisOrder::YXZ: result.rotation = yr * xr * zr; break;
                    case ControllerAxisOrder::ZXY: result.rotation = zr * xr * yr; break;
                    case ControllerAxisOrder::ZYX: result.rotation = zr * yr * xr; break;
                    case ControllerAxisOrder::XYX: result.rotation = xr * yr * xr; break;
                    case ControllerAxisOrder::YZY: result.rotation = yr * zr * yr; break;
                    case ControllerAxisOrder::ZXZ: result.rotation = zr * xr * zr; break;
                }
            }
            if (!result.rotation)
                result.rotation = defaultRotation;

            result.scale = scales.sample(time);
            if (!result.scale)
                result.scale = defaultScale;
            return result;
        }
    };

    enum class TransformTrackCompileStatus : std::uint8_t
    {
        Compiled,
        Empty,
        MissingSourceData,
        UnsupportedInterpolator,
    };

    struct TransformTrackCompileResult
    {
        TransformTrackCompileStatus status = TransformTrackCompileStatus::Empty;
        TransformControllerTrack track;

        [[nodiscard]] bool bindable() const noexcept
        {
            return status == TransformTrackCompileStatus::Compiled
                || status == TransformTrackCompileStatus::Empty;
        }

        [[nodiscard]] bool unsupported() const noexcept
        {
            return status == TransformTrackCompileStatus::UnsupportedInterpolator;
        }

        [[nodiscard]] bool hasKeys() const noexcept
        {
            return status == TransformTrackCompileStatus::Compiled;
        }
    };

    struct TransformControllerProgram
    {
        RenderCore::ModelNodeIndex node;
        ControllerTiming timing;
        TransformControllerTrack track;
        bool autoPlay = false;
    };

    struct VisibilityControllerProgram
    {
        RenderCore::ModelNodeIndex node;
        ControllerTiming timing;
        BoolControllerTrack track;
        bool autoPlay = false;
    };

    struct MorphControllerChannel
    {
        std::uint32_t sourceIndex = 0;
        FloatControllerTrack track;
        float weightMultiplier = 1.0f;
    };

    struct MorphControllerProgram
    {
        RenderCore::ModelNodeIndex node;
        ControllerTiming timing;
        std::vector<MorphControllerChannel> channels;
        bool autoPlay = false;
    };

    struct ControllerTransformSample
    {
        RenderCore::ModelNodeIndex node;
        std::optional<glm::vec3> translation;
        std::optional<glm::quat> rotation;
        std::optional<float> scale;
    };

    struct ControllerVisibilitySample
    {
        RenderCore::ModelNodeIndex node;
        bool visible = true;
    };

    struct ControllerMorphSample
    {
        RenderCore::ModelNodeIndex node;
        std::vector<std::pair<std::uint32_t, float>> weights;
    };

    struct NifControllerFrame
    {
        std::vector<ControllerTransformSample> transforms;
        std::vector<ControllerVisibilitySample> visibility;
        std::vector<ControllerMorphSample> morphs;
    };

    struct NifControllerProgram
    {
        std::vector<TransformControllerProgram> transforms;
        std::vector<VisibilityControllerProgram> visibility;
        std::vector<MorphControllerProgram> morphs;
        std::uint32_t unsupportedControllers = 0;
        std::vector<std::string> diagnostics;

        [[nodiscard]] bool empty() const noexcept
        {
            return transforms.empty() && visibility.empty() && morphs.empty();
        }

        [[nodiscard]] bool valid() const noexcept
        {
            const auto targetValid = [](const auto& value) { return value.node.valid() && value.timing.valid(); };
            return std::all_of(transforms.begin(), transforms.end(), targetValid)
                && std::all_of(visibility.begin(), visibility.end(), targetValid)
                && std::all_of(morphs.begin(), morphs.end(), targetValid);
        }

        // P3A consumes only source-embedded autoplay controllers. External .kf
        // state/priority/blend evaluation is intentionally a separate P3B input.
        [[nodiscard]] NifControllerFrame evaluateAutoplay(float sourceTime) const noexcept;
    };

    class NifControllerCompiler final
    {
    public:
        // Compiles source-embedded controller data into OSG-free immutable tracks,
        // using TranslationBundle sourceRecordId values as stable model-node keys.
        [[nodiscard]] static NifControllerProgram compile(
            Nif::FileView file, const NifRender::TranslationBundle& bundle);

        // Shared Phase 3 source-compatibility primitives used by both embedded
        // model controllers and external .kf clips. Keeping these conversions in
        // one implementation prevents the native paths from drifting apart.
        [[nodiscard]] static ControllerTiming compileTiming(const Nif::NiTimeController& source) noexcept;
        [[nodiscard]] static TransformTrackCompileResult compileTransformTrack(
            const Nif::NiKeyframeController& source);
    };
}

#endif
