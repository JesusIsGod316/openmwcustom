#include "nifcontrollerprogram.hpp"

#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nif/node.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/nifkey.hpp>
#include <components/nifrender/translationbundle.hpp>

#include <cstddef>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace
{
    [[nodiscard]] RenderNative::ControllerInterpolation interpolation(std::uint32_t value) noexcept
    {
        switch (value)
        {
            case Nif::InterpolationType_Constant:
                return RenderNative::ControllerInterpolation::Constant;
            case Nif::InterpolationType_Quadratic:
            case Nif::InterpolationType_TCB:
                return RenderNative::ControllerInterpolation::Hermite;
            case Nif::InterpolationType_Linear:
            case Nif::InterpolationType_XYZ:
            case Nif::InterpolationType_Unknown:
            default:
                return RenderNative::ControllerInterpolation::Linear;
        }
    }

    template <class Value>
    [[nodiscard]] glm::vec3 vec3(const Value& value) noexcept
    {
        return { static_cast<float>(value.x()), static_cast<float>(value.y()), static_cast<float>(value.z()) };
    }

    template <class Value>
    [[nodiscard]] glm::quat quat(const Value& value) noexcept
    {
        return { static_cast<float>(value.w()), static_cast<float>(value.x()), static_cast<float>(value.y()), static_cast<float>(value.z()) };
    }

    template <class MapPtr>
    void copyFloatTrack(const MapPtr& source, RenderNative::FloatControllerTrack& target)
    {
        if (!source)
            return;
        target.interpolation = interpolation(source->mInterpolationType);
        target.keys.reserve(source->mKeys.size());
        const bool tangents = target.interpolation == RenderNative::ControllerInterpolation::Hermite;
        for (const auto& [time, key] : source->mKeys)
        {
            RenderNative::ControllerKey<float> out;
            out.time = time;
            out.value = key.mValue;
            if (tangents)
            {
                out.inTangent = key.mInTan;
                out.outTangent = key.mOutTan;
            }
            target.keys.push_back(out);
        }
    }

    template <class MapPtr>
    void copyVec3Track(const MapPtr& source, RenderNative::Vec3ControllerTrack& target)
    {
        if (!source)
            return;
        target.interpolation = interpolation(source->mInterpolationType);
        target.keys.reserve(source->mKeys.size());
        const bool tangents = target.interpolation == RenderNative::ControllerInterpolation::Hermite;
        for (const auto& [time, key] : source->mKeys)
        {
            RenderNative::ControllerKey<glm::vec3> out;
            out.time = time;
            out.value = vec3(key.mValue);
            if (tangents)
            {
                out.inTangent = vec3(key.mInTan);
                out.outTangent = vec3(key.mOutTan);
            }
            target.keys.push_back(out);
        }
    }

    template <class MapPtr>
    void copyQuaternionTrack(const MapPtr& source, RenderNative::QuaternionControllerTrack& target)
    {
        if (!source)
            return;
        target.interpolation = interpolation(source->mInterpolationType);
        target.keys.reserve(source->mKeys.size());
        for (const auto& [time, key] : source->mKeys)
        {
            RenderNative::ControllerKey<glm::quat> out;
            out.time = time;
            out.value = quat(key.mValue);
            target.keys.push_back(out);
        }
    }

    template <class MapPtr>
    void copyBoolTrack(const MapPtr& source, RenderNative::BoolControllerTrack& target)
    {
        if (!source)
            return;
        target.interpolation = interpolation(source->mInterpolationType);
        target.keys.reserve(source->mKeys.size());
        const bool tangents = target.interpolation == RenderNative::ControllerInterpolation::Hermite;
        for (const auto& [time, key] : source->mKeys)
        {
            RenderNative::BoolControllerKey out;
            out.time = time;
            out.value = key.mValue;
            if (tangents)
            {
                out.inTangent = key.mInTan ? 1.0f : 0.0f;
                out.outTangent = key.mOutTan ? 1.0f : 0.0f;
            }
            target.keys.push_back(out);
        }
    }

    [[nodiscard]] RenderNative::ControllerTiming timing(const Nif::NiTimeController& source) noexcept
    {
        RenderNative::ControllerTiming result;
        result.frequency = source.mFrequency;
        result.phase = source.mPhase;
        result.start = source.mTimeStart;
        result.stop = source.mTimeStop;
        switch (source.extrapolationMode())
        {
            case Nif::NiTimeController::ExtrapolationMode::Cycle:
                result.extrapolation = RenderNative::ControllerExtrapolation::Cycle;
                break;
            case Nif::NiTimeController::ExtrapolationMode::Reverse:
                result.extrapolation = RenderNative::ControllerExtrapolation::Reverse;
                break;
            case Nif::NiTimeController::ExtrapolationMode::Constant:
            default:
                result.extrapolation = RenderNative::ControllerExtrapolation::Constant;
                break;
        }
        return result;
    }

    [[nodiscard]] RenderNative::ControllerAxisOrder axisOrder(Nif::NiKeyframeData::AxisOrder value) noexcept
    {
        using Source = Nif::NiKeyframeData::AxisOrder;
        using Target = RenderNative::ControllerAxisOrder;
        switch (value)
        {
            case Source::Order_XYZ: return Target::XYZ;
            case Source::Order_XZY: return Target::XZY;
            case Source::Order_YZX: return Target::YZX;
            case Source::Order_YXZ: return Target::YXZ;
            case Source::Order_ZXY: return Target::ZXY;
            case Source::Order_ZYX: return Target::ZYX;
            case Source::Order_XYX: return Target::XYX;
            case Source::Order_YZY: return Target::YZY;
            case Source::Order_ZXZ: return Target::ZXZ;
        }
        return Target::XYZ;
    }

    [[nodiscard]] RenderNative::TransformTrackCompileStatus compileTransform(
        const Nif::NiKeyframeController& source, RenderNative::TransformControllerTrack& target)
    {
        const Nif::NiKeyframeData* data = nullptr;
        if (!source.mInterpolator.empty())
        {
            if (source.mInterpolator->mRecordType != Nif::RC_NiTransformInterpolator)
                return RenderNative::TransformTrackCompileStatus::UnsupportedInterpolator;
            const auto* interpolator
                = static_cast<const Nif::NiTransformInterpolator*>(source.mInterpolator.getPtr());
            if (!interpolator->mData.empty())
                data = interpolator->mData.getPtr();
            else
                return RenderNative::TransformTrackCompileStatus::Empty;
        }
        else if (!source.mData.empty())
            data = source.mData.getPtr();
        else
            return RenderNative::TransformTrackCompileStatus::MissingSourceData;

        // A supported controller whose present data contains no populated key
        // tracks is a valid no-op. The compatibility KF loader still binds it.
        if (!data)
            return RenderNative::TransformTrackCompileStatus::MissingSourceData;

        copyQuaternionTrack(data->mRotations, target.rotations);
        copyFloatTrack(data->mXRotations, target.xRotations);
        copyFloatTrack(data->mYRotations, target.yRotations);
        copyFloatTrack(data->mZRotations, target.zRotations);
        copyVec3Track(data->mTranslations, target.translations);
        copyFloatTrack(data->mScales, target.scales);
        target.axisOrder = axisOrder(data->mAxisOrder);

        const bool populated = !target.rotations.empty() || !target.xRotations.empty() || !target.yRotations.empty()
            || !target.zRotations.empty() || !target.translations.empty() || !target.scales.empty();
        return populated ? RenderNative::TransformTrackCompileStatus::Compiled
                         : RenderNative::TransformTrackCompileStatus::Empty;
    }

    [[nodiscard]] bool compileVisibility(
        const Nif::NiVisController& source, RenderNative::BoolControllerTrack& target)
    {
        if (!source.mInterpolator.empty())
        {
            if (source.mInterpolator->mRecordType != Nif::RC_NiBoolInterpolator)
                return false;
            const auto* interpolator = static_cast<const Nif::NiBoolInterpolator*>(source.mInterpolator.getPtr());
            if (interpolator->mData.empty())
                return false;
            copyBoolTrack(interpolator->mData->mKeyList, target);
            return !target.empty();
        }

        if (source.mData.empty() || !source.mData->mKeys)
            return false;
        target.legacyStepPrevious = true;
        target.keys.reserve(source.mData->mKeys->size());
        for (const auto& [time, value] : *source.mData->mKeys)
            target.keys.push_back(RenderNative::BoolControllerKey{ .time = time, .value = value });
        return !target.empty();
    }

    [[nodiscard]] bool compileMorph(
        const Nif::NiGeomMorpherController& source, std::vector<RenderNative::MorphControllerChannel>& target)
    {
        if (source.mInterpolators.empty())
        {
            if (source.mData.empty() || source.mData->mMorphs.size() <= 1)
                return false;
            target.reserve(source.mData->mMorphs.size() - 1);
            for (std::size_t i = 1; i < source.mData->mMorphs.size(); ++i)
            {
                RenderNative::MorphControllerChannel channel;
                channel.sourceIndex = static_cast<std::uint32_t>(i);
                copyFloatTrack(source.mData->mMorphs[i].mKeyFrames, channel.track);
                target.push_back(std::move(channel));
            }
            return !target.empty();
        }

        if (source.mInterpolators.size() <= 1)
            return false;
        target.reserve(source.mInterpolators.size() - 1);
        for (std::size_t i = 1; i < source.mInterpolators.size(); ++i)
        {
            RenderNative::MorphControllerChannel channel;
            channel.sourceIndex = static_cast<std::uint32_t>(i);
            if (i < source.mWeights.size())
                channel.weightMultiplier = source.mWeights[i];
            const auto& interpolator = source.mInterpolators[i];
            if (!interpolator.empty() && interpolator->mRecordType == Nif::RC_NiFloatInterpolator)
            {
                const auto* value = static_cast<const Nif::NiFloatInterpolator*>(interpolator.getPtr());
                if (!value->mData.empty())
                    copyFloatTrack(value->mData->mKeyList, channel.track);
            }
            target.push_back(std::move(channel));
        }
        return !target.empty();
    }

    [[nodiscard]] glm::quat xyzRotation(
        const RenderNative::TransformControllerTrack& track, float time) noexcept
    {
        const float x = track.xRotations.sample(time).value_or(0.0f);
        const float y = track.yRotations.sample(time).value_or(0.0f);
        const float z = track.zRotations.sample(time).value_or(0.0f);
        const glm::quat xr = glm::angleAxis(x, glm::vec3(1.0f, 0.0f, 0.0f));
        const glm::quat yr = glm::angleAxis(y, glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::quat zr = glm::angleAxis(z, glm::vec3(0.0f, 0.0f, 1.0f));
        using Order = RenderNative::ControllerAxisOrder;
        switch (track.axisOrder)
        {
            case Order::XYZ: return xr * yr * zr;
            case Order::XZY: return xr * zr * yr;
            case Order::YZX: return yr * zr * xr;
            case Order::YXZ: return yr * xr * zr;
            case Order::ZXY: return zr * xr * yr;
            case Order::ZYX: return zr * yr * xr;
            case Order::XYX: return xr * yr * xr;
            case Order::YZY: return yr * zr * yr;
            case Order::ZXZ: return zr * xr * zr;
        }
        return xr * yr * zr;
    }

    class Compiler
    {
    public:
        Compiler(Nif::FileView file, const NifRender::TranslationBundle& bundle)
            : mFile(file)
        {
            for (std::size_t i = 0; i < bundle.model.nodes.size(); ++i)
            {
                if (bundle.model.nodes[i].sourceRecordId)
                    mTargets.emplace(*bundle.model.nodes[i].sourceRecordId,
                        RenderCore::ModelNodeIndex{ static_cast<std::uint32_t>(i) });
            }
        }

        [[nodiscard]] RenderNative::NifControllerProgram run()
        {
            for (std::size_t i = 0; i < mFile.numRoots(); ++i)
            {
                const Nif::Record* record = mFile.getRoot(i);
                if (const auto* node = dynamic_cast<const Nif::NiAVObject*>(record))
                    visit(*node, 0);
            }
            return std::move(mResult);
        }

    private:
        void diagnostic(const Nif::NiTimeController& controller, std::string_view reason)
        {
            std::ostringstream stream;
            stream << "record " << controller.mRecordIndex << " " << controller.mRecordName << ": " << reason;
            mResult.diagnostics.push_back(stream.str());
        }

        void visit(const Nif::NiAVObject& node, int animationFlags)
        {
            if (node.mRecordType == Nif::RC_NiBSAnimationNode || node.mRecordType == Nif::RC_NiBSParticleNode)
                animationFlags = static_cast<int>(node.mFlags);

            const auto found = mTargets.find(static_cast<std::uint32_t>(node.mRecordIndex));
            if (found != mTargets.end())
                compileNode(node, found->second, animationFlags);

            if (const auto* group = dynamic_cast<const Nif::NiNode*>(&node))
            {
                for (const Nif::NiAVObjectPtr& child : group->mChildren)
                {
                    if (!child.empty())
                        visit(*child.getPtr(), animationFlags);
                }
            }
        }

        void compileNode(const Nif::NiAVObject& node, RenderCore::ModelNodeIndex target, int animationFlags)
        {
            const bool autoPlay = (animationFlags & Nif::NiNode::AnimFlag_AutoPlay) != 0;
            for (Nif::NiTimeControllerPtr current = node.mController; !current.empty(); current = current->mNext)
            {
                const Nif::NiTimeController& controller = *current.getPtr();
                if (!controller.isActive())
                    continue;

                switch (controller.mRecordType)
                {
                    case Nif::RC_NiKeyframeController:
                    case Nif::RC_BSKeyframeController:
                    {
                        RenderNative::TransformControllerProgram program;
                        program.node = target;
                        program.timing = timing(controller);
                        program.autoPlay = autoPlay;
                        const RenderNative::TransformTrackCompileStatus transformStatus
                            = compileTransform(static_cast<const Nif::NiKeyframeController&>(controller), program.track);
                        if (transformStatus == RenderNative::TransformTrackCompileStatus::Compiled
                            || transformStatus == RenderNative::TransformTrackCompileStatus::Empty)
                            mResult.transforms.push_back(std::move(program));
                        else if (transformStatus == RenderNative::TransformTrackCompileStatus::UnsupportedInterpolator)
                            diagnostic(controller, "transform controller uses an unsupported interpolator");
                        break;
                    }
                    case Nif::RC_NiVisController:
                    {
                        RenderNative::VisibilityControllerProgram program;
                        program.node = target;
                        program.timing = timing(controller);
                        program.autoPlay = autoPlay;
                        if (compileVisibility(static_cast<const Nif::NiVisController&>(controller), program.track))
                            mResult.visibility.push_back(std::move(program));
                        else
                            diagnostic(controller, "visibility track is empty or uses an unsupported interpolator");
                        break;
                    }
                    case Nif::RC_NiGeomMorpherController:
                    {
                        RenderNative::MorphControllerProgram program;
                        program.node = target;
                        program.timing = timing(controller);
                        program.autoPlay = autoPlay;
                        if (compileMorph(static_cast<const Nif::NiGeomMorpherController&>(controller), program.channels))
                            mResult.morphs.push_back(std::move(program));
                        else
                            diagnostic(controller, "morph track is empty or unsupported");
                        break;
                    }
                    default:
                        ++mResult.unsupportedControllers;
                        diagnostic(controller, "controller type remains outside the Phase 3A native subset");
                        break;
                }
            }
        }

        Nif::FileView mFile;
        std::unordered_map<std::uint32_t, RenderCore::ModelNodeIndex> mTargets;
        RenderNative::NifControllerProgram mResult;
    };
}

namespace RenderNative
{
    NifControllerFrame NifControllerProgram::evaluateAutoplay(float sourceTime) const noexcept
    {
        NifControllerFrame frame;

        for (const TransformControllerProgram& controller : transforms)
        {
            if (!controller.autoPlay)
                continue;
            const float time = controller.timing.map(sourceTime);
            ControllerTransformSample sample;
            sample.node = controller.node;
            sample.translation = controller.track.translations.sample(time);
            sample.scale = controller.track.scales.sample(time);
            sample.rotation = controller.track.rotations.sample(time);
            if (!sample.rotation
                && (!controller.track.xRotations.empty() || !controller.track.yRotations.empty()
                    || !controller.track.zRotations.empty()))
                sample.rotation = xyzRotation(controller.track, time);
            if (sample.translation || sample.rotation || sample.scale)
                frame.transforms.push_back(std::move(sample));
        }

        for (const VisibilityControllerProgram& controller : visibility)
        {
            if (!controller.autoPlay)
                continue;
            const std::optional<bool> value = controller.track.sample(controller.timing.map(sourceTime));
            if (value)
                frame.visibility.push_back(ControllerVisibilitySample{ controller.node, *value });
        }

        for (const MorphControllerProgram& controller : morphs)
        {
            if (!controller.autoPlay)
                continue;
            ControllerMorphSample sample;
            sample.node = controller.node;
            const float time = controller.timing.map(sourceTime);
            sample.weights.reserve(controller.channels.size());
            for (const MorphControllerChannel& channel : controller.channels)
            {
                const float value = channel.track.sample(time).value_or(0.0f) * channel.weightMultiplier;
                sample.weights.emplace_back(channel.sourceIndex, value);
            }
            if (!sample.weights.empty())
                frame.morphs.push_back(std::move(sample));
        }

        return frame;
    }

    NifControllerProgram NifControllerCompiler::compile(
        Nif::FileView file, const NifRender::TranslationBundle& bundle)
    {
        return Compiler(file, bundle).run();
    }

    ControllerTiming NifControllerCompiler::compileTiming(const Nif::NiTimeController& source) noexcept
    {
        return timing(source);
    }

    TransformTrackCompileResult NifControllerCompiler::compileTransformTrack(
        const Nif::NiKeyframeController& source)
    {
        TransformTrackCompileResult result;
        result.status = compileTransform(source, result.track);
        return result;
    }
}
