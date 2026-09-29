#include "nativeanimationruntime.hpp"

#include "../animation.hpp"

#include <components/misc/strings/lower.hpp>
#include <components/render/native/nifkeyframeclip.hpp>
#include <components/render/native/skeletalobjectprogram.hpp>
#include <components/rendercore/records.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] bool finite(float value) noexcept
    {
        return std::isfinite(value);
    }

    [[nodiscard]] bool finite(const glm::vec3& value) noexcept
    {
        return finite(value.x) && finite(value.y) && finite(value.z);
    }

    [[nodiscard]] bool finite(const glm::quat& value) noexcept
    {
        return finite(value.w) && finite(value.x) && finite(value.y) && finite(value.z);
    }

    [[nodiscard]] bool finite(const glm::mat4& value) noexcept
    {
        for (glm::length_t column = 0; column < 4; ++column)
            for (glm::length_t row = 0; row < 4; ++row)
                if (!finite(value[column][row]))
                    return false;
        return true;
    }

    struct NifLocalTransform
    {
        glm::vec3 translation{ 0.0f };
        glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f };
        float scale = 1.0f;
    };

    // sourceLocal is emitted from one authored NiTransform. Refuse uncommon
    // reflected/non-uniform forms which cannot be represented by the external
    // KF translation/quaternion/uniform-scale controller contract.
    [[nodiscard]] std::optional<NifLocalTransform> decomposeNifLocal(const glm::mat4& value) noexcept
    {
        if (!finite(value))
            return std::nullopt;

        constexpr float affineTolerance = 1e-4f;
        if (std::abs(value[0][3]) > affineTolerance || std::abs(value[1][3]) > affineTolerance
            || std::abs(value[2][3]) > affineTolerance || std::abs(value[3][3] - 1.0f) > affineTolerance)
            return std::nullopt;

        const glm::vec3 columns[3] = {
            glm::vec3(value[0]),
            glm::vec3(value[1]),
            glm::vec3(value[2]),
        };
        const float lengths[3] = {
            glm::length(columns[0]),
            glm::length(columns[1]),
            glm::length(columns[2]),
        };
        if (lengths[0] <= 1e-7f || lengths[1] <= 1e-7f || lengths[2] <= 1e-7f)
            return std::nullopt;

        const float magnitude = (lengths[0] + lengths[1] + lengths[2]) / 3.0f;
        const float scaleTolerance = std::max(1e-4f, magnitude * 1e-4f);
        if (std::abs(lengths[0] - magnitude) > scaleTolerance
            || std::abs(lengths[1] - magnitude) > scaleTolerance
            || std::abs(lengths[2] - magnitude) > scaleTolerance)
            return std::nullopt;

        glm::mat3 upper(value);
        const float determinant = glm::determinant(upper);
        if (!finite(determinant) || std::abs(determinant) <= 1e-8f)
            return std::nullopt;
        const float scale = determinant < 0.0f ? -magnitude : magnitude;

        glm::mat3 rotation;
        rotation[0] = columns[0] / scale;
        rotation[1] = columns[1] / scale;
        rotation[2] = columns[2] / scale;
        if (std::abs(glm::dot(rotation[0], rotation[1])) > 2e-3f
            || std::abs(glm::dot(rotation[0], rotation[2])) > 2e-3f
            || std::abs(glm::dot(rotation[1], rotation[2])) > 2e-3f
            || std::abs(glm::determinant(rotation) - 1.0f) > 2e-3f)
            return std::nullopt;

        NifLocalTransform result;
        result.translation = glm::vec3(value[3]);
        result.rotation = glm::normalize(glm::quat_cast(rotation));
        result.scale = scale;
        if (!finite(result.translation) || !finite(result.rotation) || !finite(result.scale))
            return std::nullopt;
        return result;
    }

    [[nodiscard]] glm::mat4 composeNifLocal(const NifLocalTransform& value) noexcept
    {
        glm::mat4 result = glm::mat4_cast(value.rotation);
        result[0] *= value.scale;
        result[1] *= value.scale;
        result[2] *= value.scale;
        result[3] = glm::vec4(value.translation, 1.0f);
        return result;
    }
}

namespace MWRender
{
    struct V4NativeAnimationRuntime::Impl
    {
        struct CachedClip
        {
            const ToUTF8::StatelessUtf8Encoder* encoder = nullptr;
            std::shared_ptr<const RenderNative::NifKeyframeClip> clip;
            std::map<std::string, const RenderNative::NamedTransformTrack*, std::less<>> foldedControllers;
            std::string diagnostic;
        };

        struct BoneBinding
        {
            const RenderCore::SkeletonPayload* payload = nullptr;
            std::map<std::string, std::size_t, std::less<>> indices;
            std::vector<std::optional<NifLocalTransform>> sourceBase;
            std::set<std::string, std::less<>> collapsedParentNodes;
            std::string diagnostic;
        };

        struct ActorPose
        {
            const RenderCore::SkeletonPayload* payload = nullptr;
            std::string skeletonIdentity;
            std::vector<NifLocalTransform> sourceLocal;
            std::uint64_t selectionSignature = 0;
            bool collapsedParentAnimation = false;
        };
        struct ObjectPose
        {
            const Animation* owner = nullptr;
            std::uint64_t rootRevision = 0;
            const RenderNative::SkeletalObjectProgram* program = nullptr;
            RenderNative::SkeletalObjectProgram::State state;
        };

        explicit Impl(const VFS::Manager& vfs)
            : mVfs(vfs)
        {
        }

        [[nodiscard]] CachedClip& clip(
            std::string_view sourcePath, const ToUTF8::StatelessUtf8Encoder* encoder)
        {
            const std::string identity(sourcePath);
            auto [it, inserted] = mClips.try_emplace(identity);
            CachedClip& cached = it->second;
            if (!inserted && cached.encoder == encoder && (cached.clip || !cached.diagnostic.empty()))
                return cached;

            cached = {};
            cached.encoder = encoder;
            RenderNative::NifKeyframeClipCompiler compiler(mVfs, encoder);
            const VFS::Path::Normalized normalized(sourcePath);
            RenderNative::NifKeyframeCompileResult compiled
                = compiler.compile(VFS::Path::NormalizedView(normalized));
            if (!compiled.usable())
            {
                cached.diagnostic = compiled.diagnostic.empty()
                    ? "native KF clip is not usable"
                    : std::move(compiled.diagnostic);
                return cached;
            }

            cached.clip = std::make_shared<const RenderNative::NifKeyframeClip>(std::move(compiled.clip));
            for (const auto& [name, track] : cached.clip->controllers)
                cached.foldedControllers.emplace(Misc::StringUtils::lowerCase(name), &track);
            return cached;
        }

        [[nodiscard]] BoneBinding& bones(const RenderCore::SkeletonRecord& skeleton)
        {
            auto [it, inserted] = mSkeletons.try_emplace(skeleton.sourceIdentity);
            BoneBinding& binding = it->second;
            const RenderCore::SkeletonPayload* const payload = skeleton.payload.get();
            if (!inserted && binding.payload == payload)
                return binding;

            binding = {};
            binding.payload = payload;
            if (!payload)
            {
                binding.diagnostic = "native animation received no skeleton payload";
                return binding;
            }

            binding.sourceBase.reserve(payload->bones.size());
            for (std::size_t i = 0; i < payload->bones.size(); ++i)
            {
                const RenderCore::BoneRecord& bone = payload->bones[i];
                const std::string folded = Misc::StringUtils::lowerCase(bone.name);
                if (!binding.indices.emplace(folded, i).second)
                {
                    binding.diagnostic = "native animation skeleton contains duplicate bone name: " + folded;
                    return binding;
                }
                if (!bone.sourceAnimationBoundary)
                {
                    binding.diagnostic = "skeleton lacks authored source-animation boundary metadata";
                    return binding;
                }
                if (bone.sourceControllerFlags != 0 || bone.sourceParentControllerFlags != 0)
                {
                    binding.diagnostic = "skeleton source path contains embedded controller semantics";
                    return binding;
                }

                std::optional<NifLocalTransform> base = decomposeNifLocal(bone.sourceLocal);
                if (!base)
                {
                    binding.diagnostic = "authored bone local transform is not safely decomposable: " + folded;
                    return binding;
                }
                binding.sourceBase.push_back(std::move(base));
                for (const std::string& parent : bone.sourceParentPathNodes)
                    binding.collapsedParentNodes.insert(Misc::StringUtils::lowerCase(parent));
            }
            return binding;
        }

        const VFS::Manager& mVfs;
        std::map<std::string, CachedClip, std::less<>> mClips;
        std::map<std::string, BoneBinding, std::less<>> mSkeletons;
        std::map<std::string, ActorPose, std::less<>> mActors;
        std::map<std::string, std::uint64_t, std::less<>> mPendingActorSelections;
        std::set<std::string, std::less<>> mSeenActors;
        std::map<std::string, ObjectPose, std::less<>> mObjects;
        std::set<std::string, std::less<>> mSeenObjects;
    };

    V4NativeAnimationRuntime::V4NativeAnimationRuntime(const VFS::Manager& vfs)
        : mImpl(std::make_unique<Impl>(vfs))
    {
    }

    V4NativeAnimationRuntime::~V4NativeAnimationRuntime() = default;

    void V4NativeAnimationRuntime::beginFrame()
    {
        mImpl->mSeenActors.clear();
        mImpl->mSeenObjects.clear();
    }

    V4NativeAnimationPoseResult V4NativeAnimationRuntime::captureSkeletalObjectPose(std::string_view identity,
        const Animation& animation, const RenderNative::SkeletalObjectProgram& program,
        float simulationTime, std::vector<glm::mat4>& localTransforms)
    {
        V4NativeAnimationPoseResult result;
        if (!program.valid()) { result.diagnostic = program.diagnostic(); return result; }
        Animation::V4NativeAnimationState selection;
        std::optional<float> clock;
        if (!animation.captureV4NativeAnimationState(selection, result.diagnostic)
            || !animation.captureV4ObjectControllerClock(clock, result.diagnostic)) return result;
        std::vector<RenderNative::SkeletalObjectProgram::ExternalChannel> channels;
        for (const auto& layer : selection.layers)
        {
            if (!layer) continue;
            auto& cached = mImpl->clip(layer->sourcePath, selection.encoder);
            if (!cached.clip || cached.clip->unsupportedControllers)
            {
                result.diagnostic = "skeletal object KF is not completely supported: " + cached.diagnostic;
                return result;
            }
            for (const auto& name : layer->boneNames)
            {
                const auto track = cached.foldedControllers.find(name);
                if (track == cached.foldedControllers.end())
                {
                    result.diagnostic = "skeletal object KF lost bound node: " + name;
                    return result;
                }
                channels.push_back({name, track->second, layer->time});
            }
        }
        const std::string key(identity);
        auto& object = mImpl->mObjects[key];
        std::vector<glm::mat4> reference;
        if (object.owner != &animation || object.rootRevision != animation.getV4ObjectRootRevision()
            || object.program != &program)
        {
            object = {};
            reference = program.initialState().locals;
            if (!animation.seedV4ObjectNodeTransforms(program.nodeNames(), program.requiredSeedNodes(), reference, result.diagnostic))
                return result;
            auto state = program.initialState();
            for (std::size_t i = 0; i < reference.size(); ++i)
                if (program.usedNodes()[i]) state.locals[i] = reference[i];
            if (!program.seed(state.locals, object.state))
            {
                result.diagnostic = "skeletal object seed is not uniform NIF TRS";
                return result;
            }
            object.owner = &animation;
            object.rootRevision = animation.getV4ObjectRootRevision();
            object.program = &program;
        }
        if (!program.evaluate(simulationTime, clock, channels, selection.accumulationBone,
                selection.accumulationAxes, object.state, localTransforms))
        {
            result.diagnostic = "skeletal object playback has uncovered tracks or invalid transforms";
            for (const auto& channel : channels)
            {
                const auto found = std::find(program.nodeNames().begin(), program.nodeNames().end(), channel.node);
                if (found == program.nodeNames().end()) result.diagnostic += " missing=" + std::string(channel.node);
                else if (!program.usedNodes()[static_cast<std::size_t>(found - program.nodeNames().begin())])
                    result.diagnostic += " outside-skin=" + std::string(channel.node);
                if (result.diagnostic.size() > 700) break;
            }
            return result;
        }
        const char* validation = std::getenv("OPENMW_VK_VALIDATE_SKELETAL_OBJECT_POSES");
        if (reference.empty() && validation && validation[0] == '1')
        {
            reference = program.initialState().locals;
            if (!animation.seedV4ObjectNodeTransforms(program.nodeNames(), program.requiredSeedNodes(), reference, result.diagnostic))
                return result;
        }
        // Always validate admission; the separate diagnostic arm additionally
        // checks every frame. Its overhead is excluded from performance arms.
        if (!reference.empty())
        {
            for (std::size_t i = 0; i < reference.size(); ++i)
                if (program.usedNodes()[i])
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r)
                        {
                            const float expected = reference[i][c][r];
                            const float error = std::abs(object.state.locals[i][c][r] - expected);
                            result.poseMaximumError = std::max(result.poseMaximumError, error);
                            if (!finite(expected) || error > 1e-3f + 1e-4f * std::abs(expected))
                            {
                                result.diagnostic = "skeletal object pose differs at " + program.nodeNames()[i]
                                    + " error=" + std::to_string(error);
                                return result;
                            }
                        }
            result.poseValidated = true;
        }
        // Only successful frames retain history. A compatibility frame forces a
        // fresh seed next time; partial channels must never resume stale state.
        mImpl->mSeenObjects.insert(key);
        result.sampledTracks = static_cast<std::uint32_t>(program.channelCount() + channels.size());
        result.status = V4NativeAnimationPoseStatus::Applied;
        return result;
    }

    V4NativeAnimationPoseResult V4NativeAnimationRuntime::captureSkeletonPose(
        std::string_view actorIdentity, const Animation& animation, const RenderCore::SkeletonRecord& skeleton,
        std::vector<glm::mat4>& localTransforms)
    {
        V4NativeAnimationPoseResult result;
        const std::string actorKey(actorIdentity);
        mImpl->mSeenActors.insert(actorKey);

        Animation::V4NativeAnimationState state;
        if (!animation.captureV4NativeAnimationState(state, result.diagnostic))
            return result;

        // Native history is valid only while the active animation selection has
        // the same group/source/bone coverage. OSG detaches and replaces
        // controllers on transitions; retaining untouched native bone channels
        // across that boundary can leave one body section in the previous pose.
        // A changed signature therefore requests exactly one evaluated OSG seed
        // frame, after which native sampling resumes from the authoritative pose.
        std::uint64_t selectionSignature = 1469598103934665603ull;
        const auto hashText = [&](std::string_view value) {
            for (const unsigned char c : value)
            {
                selectionSignature ^= c;
                selectionSignature *= 1099511628211ull;
            }
            selectionSignature ^= 0xffu;
            selectionSignature *= 1099511628211ull;
        };
        for (std::size_t layerIndex = 0; layerIndex < state.layers.size(); ++layerIndex)
        {
            selectionSignature ^= static_cast<std::uint64_t>(layerIndex + 1);
            selectionSignature *= 1099511628211ull;
            const auto& optionalLayer = state.layers[layerIndex];
            if (!optionalLayer)
                continue;
            hashText(optionalLayer->groupName);
            hashText(optionalLayer->sourcePath);
            for (const std::string& bone : optionalLayer->boneNames)
                hashText(bone);
        }
        mImpl->mPendingActorSelections.insert_or_assign(actorKey, selectionSignature);

        Impl::BoneBinding& binding = mImpl->bones(skeleton);
        if (!binding.diagnostic.empty())
        {
            result.diagnostic = binding.diagnostic;
            return result;
        }
        if (binding.payload != skeleton.payload.get()
            || binding.sourceBase.size() != skeleton.payload->bones.size())
        {
            result.diagnostic = "native animation skeleton binding cache is incoherent";
            return result;
        }

        auto actorIt = mImpl->mActors.find(actorKey);
        if (actorIt == mImpl->mActors.end() || actorIt->second.payload != skeleton.payload.get()
            || actorIt->second.skeletonIdentity != skeleton.sourceIdentity)
        {
            result.diagnostic = "native animation pose requires one compatibility seed frame";
            return result;
        }
        Impl::ActorPose& actorPose = actorIt->second;
        if (actorPose.selectionSignature != selectionSignature)
        {
            actorPose.selectionSignature = selectionSignature;
            result.diagnostic = "active animation selection changed; exact compatibility reseed required";
            return result;
        }
        if (actorPose.collapsedParentAnimation)
        {
            result.diagnostic = "actor previously animated a collapsed skeleton parent node";
            return result;
        }
        if (actorPose.sourceLocal.size() != skeleton.payload->bones.size())
        {
            result.diagnostic = "native actor pose history has the wrong bone count";
            return result;
        }

        const std::string accumulationBone
            = state.accumulationBone.empty() ? std::string() : Misc::StringUtils::lowerCase(state.accumulationBone);

        for (const auto& optionalLayer : state.layers)
        {
            if (!optionalLayer)
                continue;
            const Animation::V4NativeAnimationLayer& layer = *optionalLayer;
            Impl::CachedClip& cached = mImpl->clip(layer.sourcePath, state.encoder);
            if (!cached.clip)
            {
                result.diagnostic = "native animation clip '" + std::string(layer.sourcePath)
                    + "' unavailable: " + cached.diagnostic;
                return result;
            }

            for (const std::string& boundName : layer.boneNames)
            {
                const std::string folded = Misc::StringUtils::lowerCase(boundName);
                const auto boneIt = binding.indices.find(folded);
                if (boneIt == binding.indices.end())
                {
                    if (binding.collapsedParentNodes.contains(folded))
                    {
                        actorPose.collapsedParentAnimation = true;
                        result.diagnostic = "active KF animates collapsed skeleton parent node: " + folded;
                        return result;
                    }
                    continue; // Active KF may control an attachment/non-skin node.
                }

                const auto trackIt = cached.foldedControllers.find(folded);
                if (trackIt == cached.foldedControllers.end() || !trackIt->second)
                {
                    result.diagnostic = "native KF clip lost compatibility-bound bone '" + folded + "'";
                    return result;
                }

                const std::size_t boneIndex = boneIt->second;
                if (boneIndex >= actorPose.sourceLocal.size() || boneIndex >= binding.sourceBase.size()
                    || !binding.sourceBase[boneIndex])
                {
                    result.diagnostic = "native animation bone history is unavailable: " + folded;
                    return result;
                }

                NifLocalTransform local = actorPose.sourceLocal[boneIndex];
                const NifLocalTransform& base = *binding.sourceBase[boneIndex];
                const RenderNative::NamedTransformTrack& controller = *trackIt->second;
                const float keyTime = controller.timing.map(layer.time);
                const RenderNative::TransformTrackSample sampled = controller.track.sample(keyTime);

                // Match NifOsg::KeyframeController exactly: translation/scale
                // without keys retain the previous value, while absent rotation
                // is explicitly reset to the authored MatrixTransform rotation.
                if (sampled.translation)
                    local.translation = *sampled.translation;
                if (sampled.rotation)
                    local.rotation = glm::normalize(*sampled.rotation);
                else
                    local.rotation = base.rotation;
                if (sampled.scale)
                    local.scale = *sampled.scale;

                if (!finite(local.translation) || !finite(local.rotation) || !finite(local.scale))
                {
                    result.diagnostic = "native KF produced a non-finite bone transform: " + folded;
                    return result;
                }

                if (!accumulationBone.empty() && folded == accumulationBone)
                {
                    for (std::size_t axis = 0; axis < state.accumulationAxes.size(); ++axis)
                    {
                        if (state.accumulationAxes[axis] != 0.0f)
                            local.translation[static_cast<glm::length_t>(axis)] = 0.0f;
                    }
                }

                actorPose.sourceLocal[boneIndex] = local;
                ++result.sampledTracks;
            }
        }

        localTransforms.clear();
        localTransforms.reserve(skeleton.payload->bones.size());
        for (std::size_t i = 0; i < skeleton.payload->bones.size(); ++i)
        {
            const RenderCore::BoneRecord& bone = skeleton.payload->bones[i];
            const glm::mat4 evaluated = bone.sourceParentPath * composeNifLocal(actorPose.sourceLocal[i]);
            if (!finite(evaluated))
            {
                localTransforms.clear();
                result.diagnostic = "native KF produced a non-finite skeleton-local matrix: " + bone.name;
                return result;
            }
            localTransforms.push_back(evaluated);
        }

        result.status = V4NativeAnimationPoseStatus::Applied;
        return result;
    }

    bool V4NativeAnimationRuntime::seedSkeletonPose(std::string_view actorIdentity,
        const RenderCore::SkeletonRecord& skeleton, std::span<const glm::mat4> localTransforms,
        std::string& diagnostic)
    {
        diagnostic.clear();
        const std::string actorKey(actorIdentity);
        mImpl->mSeenActors.insert(actorKey);

        Impl::BoneBinding& binding = mImpl->bones(skeleton);
        if (!binding.diagnostic.empty())
        {
            diagnostic = binding.diagnostic;
            return false;
        }
        if (!skeleton.payload || localTransforms.size() != skeleton.payload->bones.size())
        {
            diagnostic = "compatibility seed pose does not match skeleton bone count";
            return false;
        }

        Impl::ActorPose seeded;
        seeded.payload = skeleton.payload.get();
        seeded.skeletonIdentity = skeleton.sourceIdentity;
        seeded.sourceLocal.reserve(localTransforms.size());

        const auto previous = mImpl->mActors.find(actorKey);
        if (previous != mImpl->mActors.end())
        {
            seeded.collapsedParentAnimation = previous->second.collapsedParentAnimation;
            seeded.selectionSignature = previous->second.selectionSignature;
        }
        else if (const auto pending = mImpl->mPendingActorSelections.find(actorKey);
                 pending != mImpl->mPendingActorSelections.end())
            seeded.selectionSignature = pending->second;

        for (std::size_t i = 0; i < localTransforms.size(); ++i)
        {
            const RenderCore::BoneRecord& bone = skeleton.payload->bones[i];
            const float determinant = glm::determinant(bone.sourceParentPath);
            if (!finite(determinant) || std::abs(determinant) <= 1e-8f)
            {
                diagnostic = "skeleton source-parent path is non-invertible: " + bone.name;
                return false;
            }
            const glm::mat4 sourceLocal = glm::inverse(bone.sourceParentPath) * localTransforms[i];
            std::optional<NifLocalTransform> decomposed = decomposeNifLocal(sourceLocal);
            if (!decomposed)
            {
                diagnostic = "compatibility seed cannot be represented as NIF local transform: " + bone.name;
                return false;
            }
            seeded.sourceLocal.push_back(std::move(*decomposed));
        }

        mImpl->mActors.insert_or_assign(actorKey, std::move(seeded));
        return true;
    }

    void V4NativeAnimationRuntime::endFrame()
    {
        std::erase_if(mImpl->mObjects, [&](const auto& entry) { return !mImpl->mSeenObjects.contains(entry.first); });
        for (auto it = mImpl->mActors.begin(); it != mImpl->mActors.end();)
        {
            if (!mImpl->mSeenActors.contains(it->first))
            {
                mImpl->mPendingActorSelections.erase(it->first);
                it = mImpl->mActors.erase(it);
            }
            else
                ++it;
        }
    }

    void V4NativeAnimationRuntime::clear()
    {
        mImpl->mClips.clear();
        mImpl->mSkeletons.clear();
        mImpl->mActors.clear();
        mImpl->mPendingActorSelections.clear();
        mImpl->mSeenActors.clear();
        mImpl->mObjects.clear();
        mImpl->mSeenObjects.clear();
    }
}
