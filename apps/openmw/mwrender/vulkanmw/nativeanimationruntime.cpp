#include "nativeanimationruntime.hpp"

#include "../animation.hpp"

#include <components/misc/strings/lower.hpp>
#include <components/render/native/nifkeyframeclip.hpp>
#include <components/rendercore/records.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/pathutil.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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

    // Bone sourceLocal is emitted from one authored NiTransform. Refuse the
    // uncommon reflected/non-uniform matrix forms which cannot be represented
    // by the KF controller's translation/quaternion/uniform-scale contract.
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
            std::vector<std::optional<NifLocalTransform>> sourceLocal;
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
            RenderNative::NifKeyframeCompileResult compiled
                = compiler.compile(VFS::Path::NormalizedView(sourcePath));
            if (!compiled.usable())
            {
                cached.diagnostic = compiled.diagnostic.empty()
                    ? "native KF clip is not usable"
                    : std::move(compiled.diagnostic);
                return cached;
            }

            cached.clip = std::make_shared<const RenderNative::NifKeyframeClip>(std::move(compiled.clip));
            for (const auto& [name, track] : cached.clip->controllers)
            {
                cached.foldedControllers.emplace(Misc::StringUtils::lowerCase(name), &track);
            }
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
                return binding;
            binding.sourceLocal.reserve(payload->bones.size());
            for (std::size_t i = 0; i < payload->bones.size(); ++i)
            {
                const RenderCore::BoneRecord& bone = payload->bones[i];
                binding.indices.emplace(Misc::StringUtils::lowerCase(bone.name), i);
                binding.sourceLocal.push_back(decomposeNifLocal(bone.sourceLocal));
            }
            return binding;
        }

        const VFS::Manager& mVfs;
        std::map<std::string, CachedClip, std::less<>> mClips;
        std::map<std::string, BoneBinding, std::less<>> mSkeletons;
    };

    V4NativeAnimationRuntime::V4NativeAnimationRuntime(const VFS::Manager& vfs)
        : mImpl(std::make_unique<Impl>(vfs))
    {
    }

    V4NativeAnimationRuntime::~V4NativeAnimationRuntime() = default;

    V4NativeAnimationPoseResult V4NativeAnimationRuntime::captureSkeletonPose(
        const Animation& animation, const RenderCore::SkeletonRecord& skeleton,
        std::vector<glm::mat4>& localTransforms)
    {
        V4NativeAnimationPoseResult result;
        if (!skeleton.payload)
        {
            result.diagnostic = "native animation received no skeleton payload";
            return result;
        }

        Animation::V4NativeAnimationState state;
        if (!animation.captureV4NativeAnimationState(state, result.diagnostic))
            return result;

        Impl::BoneBinding& binding = mImpl->bones(skeleton);
        if (binding.payload != skeleton.payload.get()
            || binding.sourceLocal.size() != skeleton.payload->bones.size())
        {
            result.diagnostic = "native animation skeleton binding cache is incoherent";
            return result;
        }

        localTransforms.clear();
        localTransforms.reserve(skeleton.payload->bones.size());
        for (const RenderCore::BoneRecord& bone : skeleton.payload->bones)
            localTransforms.push_back(bone.bindLocal);

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
                    continue; // The active KF may control non-skin transform nodes.

                const auto trackIt = cached.foldedControllers.find(folded);
                if (trackIt == cached.foldedControllers.end() || !trackIt->second)
                {
                    result.diagnostic = "native KF clip lost compatibility-bound bone '" + folded + "'";
                    return result;
                }

                const std::size_t boneIndex = boneIt->second;
                if (boneIndex >= binding.sourceLocal.size() || !binding.sourceLocal[boneIndex])
                {
                    result.diagnostic = "authored bone local transform is not safely decomposable: " + folded;
                    return result;
                }

                NifLocalTransform local = *binding.sourceLocal[boneIndex];
                const RenderNative::NamedTransformTrack& controller = *trackIt->second;
                const float keyTime = controller.timing.map(layer.time);
                const RenderNative::TransformTrackSample sampled = controller.track.sample(keyTime);
                if (sampled.translation)
                    local.translation = *sampled.translation;
                if (sampled.rotation)
                    local.rotation = glm::normalize(*sampled.rotation);
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

                const glm::mat4 animatedSourceLocal = composeNifLocal(local);
                const RenderCore::BoneRecord& bone = skeleton.payload->bones[boneIndex];
                const glm::mat4 evaluated = bone.sourceParentPath * animatedSourceLocal;
                if (!finite(evaluated))
                {
                    result.diagnostic = "native KF produced a non-finite skeleton-local matrix: " + folded;
                    return result;
                }
                localTransforms[boneIndex] = evaluated;
                ++result.sampledTracks;
            }
        }

        if (result.sampledTracks == 0)
        {
            localTransforms.clear();
            result.diagnostic = "active actor has no native KF skeleton tracks to substitute";
            return result;
        }

        result.status = V4NativeAnimationPoseStatus::Applied;
        return result;
    }

    void V4NativeAnimationRuntime::clear()
    {
        mImpl->mClips.clear();
        mImpl->mSkeletons.clear();
    }
}
