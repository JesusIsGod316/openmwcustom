#ifndef OPENMW_RENDER_NATIVE_SKELETALOBJECTPROGRAM_H
#define OPENMW_RENDER_NATIVE_SKELETALOBJECTPROGRAM_H

#include "nifkeyframeclip.hpp"
#include <components/rendercore/renderworld.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <span>
#include <array>
#include <limits>
#include <vector>

namespace RenderNative
{
    // Load-bound native producer for transform-only skeletal props.
    // No live scenegraph, mesh inspection, or material capture during playback.
    // Gameplay supplies clocks and selected KF tracks, not a live scenegraph.
    // Visibility, morph, particle and material controllers retain compatibility.
    class SkeletalObjectProgram
    {
        struct Channel
        {
            TransformControllerProgram controller;
            glm::vec3 translation;
            glm::quat rotation;
            float scale;
        };
        std::vector<glm::mat4> mLocals;
        std::vector<Channel> mChannels;
        std::vector<std::vector<std::size_t>> mBonePaths;
        std::vector<std::string> mNodeNames;
        std::vector<bool> mUsed;
        std::vector<bool> mRequiredSeed;
        std::vector<glm::quat> mBaseRotations;
        bool mValid = false;
        std::string mDiagnostic;

        static std::string folded(std::string value)
        {
            for (char& c : value)
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
            return value;
        }

    public:
        struct State { std::vector<glm::mat4> locals; };
        struct ExternalChannel
        {
            std::string_view node;
            const NamedTransformTrack* controller = nullptr;
            float time = 0;
        };
        bool valid() const noexcept { return mValid; }
        const std::string& diagnostic() const noexcept { return mDiagnostic; }
        std::size_t channelCount() const noexcept { return mChannels.size(); }
        const std::vector<std::string>& nodeNames() const noexcept { return mNodeNames; }
        const std::vector<bool>& usedNodes() const noexcept { return mUsed; }
        const std::vector<bool>& requiredSeedNodes() const noexcept { return mRequiredSeed; }
        State initialState() const { return {mLocals}; }

        bool seed(std::span<const glm::mat4> locals, State& state) const
        {
            if (!mValid || locals.size() != mLocals.size()) return false;
            for (std::size_t i = 0; i < locals.size(); ++i)
                if (mUsed[i] && !semanticLocal(locals[i])) return false;
            state.locals.assign(locals.begin(), locals.end());
            return true;
        }

        static SkeletalObjectProgram bind(const RenderCore::RenderWorld& world,
            const RenderCore::ModelRecord& model, const RenderCore::SkeletonRecord& skeleton,
            const NifControllerProgram& controllers)
        {
            using namespace RenderCore;
            SkeletalObjectProgram result;
            const auto reject = [&](const char* reason) {
                result.mDiagnostic = reason;
                return result;
            };
            if (!model.payload || !skeleton.payload || model.dynamicRequirements != 0
                || !controllers.valid() || controllers.unsupportedControllers != 0
                || !controllers.diagnostics.empty() || !controllers.visibility.empty() || !controllers.morphs.empty())
                return reject("asset requires controllers outside native skeletal props");
            const auto& nodes = model.payload->nodes;
            const auto& bones = skeleton.payload->bones;
            if (nodes.empty() || bones.empty()) return reject("missing native hierarchy");
            bool hasSkin = false;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                const auto& node = nodes[i];
                if (!semantic_detail::finite(node.localTransform)
                    || node.kind == ModelNodeKind::Switch || node.kind == ModelNodeKind::Lod
                    || node.kind == ModelNodeKind::Billboard
                    || (node.parent.valid() && node.parent.value() >= i) || node.activeSwitchChild || node.lod
                    || node.billboard || (node.controllerFlags & ~modelControllerFlag(ModelControllerFlag::Transform)))
                    return reject("unsupported node selection or controller contract");
                if (node.mesh)
                {
                    const auto* mesh = world.get(*node.mesh);
                    if (!mesh || mesh->morphed)
                        return reject("skeletal prop includes missing or morphed geometry");
                    hasSkin = hasSkin || mesh->skinned;
                }
                result.mLocals.push_back(node.localTransform);
                result.mNodeNames.push_back(folded(node.name));
            }
            if (!hasSkin) return reject("asset has no skin");

            // Reconcile canonical bone names once. Ambiguity never resolves by
            // guessing; the legacy producer remains available for that asset.
            std::vector<std::size_t> boneNodes;
            for (const auto& bone : bones)
            {
                if (bone.name.empty() || !semantic_detail::finite(bone.bindLocal))
                    return reject("invalid native bone");
                std::size_t match = nodes.size();
                for (std::size_t n = 0; n < nodes.size(); ++n)
                    if (folded(nodes[n].name) == folded(bone.name))
                    {
                        if (match != nodes.size()) return reject("ambiguous native bone name");
                        match = n;
                    }
                if (match == nodes.size()) return reject("missing native bone node");
                boneNodes.push_back(match);
            }
            std::vector<bool> used(nodes.size());
            for (std::size_t b = 0; b < bones.size(); ++b)
            {
                const auto parent = bones[b].parent;
                if (parent < -1 || (parent >= 0 && static_cast<std::size_t>(parent) >= b))
                    return reject("invalid native bone parent");
                const std::size_t stop = parent < 0 ? nodes.size() : boneNodes[static_cast<std::size_t>(parent)];
                std::size_t n = boneNodes[b];
                std::vector<std::size_t> path;
                while (n != stop && n < nodes.size())
                {
                    path.push_back(n);
                    used[n] = true;
                    n = nodes[n].parent.valid() ? nodes[n].parent.value() : nodes.size();
                }
                if (n != stop) return reject("bone ancestry differs from native model");
                std::reverse(path.begin(), path.end());
                glm::mat4 bind(1.0f);
                for (const auto index : path) bind *= nodes[index].localTransform;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        if (std::abs(bind[c][r] - bones[b].bindLocal[c][r]) > 1e-4f)
                            return reject("bone bind space differs from native model");
                result.mBonePaths.push_back(std::move(path));
            }
            std::vector<unsigned> channelCounts(nodes.size());
            for (const auto& controller : controllers.transforms)
            {
                const auto index = controller.node.value();
                if (index >= nodes.size() || !used[index] || channelCounts[index]++)
                    return reject("uncovered or duplicate transform controller");
                const auto& matrix = nodes[index].localTransform;
                if (!semantic_detail::finite(matrix) || matrix[0][3] != 0 || matrix[1][3] != 0
                    || matrix[2][3] != 0 || matrix[3][3] != 1)
                    return reject("non-affine controller base");
                const float scale = std::cbrt(glm::determinant(glm::mat3(matrix)));
                if (!std::isfinite(scale) || std::abs(scale) < 1e-7f)
                    return reject("singular controller base");
                const glm::mat3 rotation = glm::mat3(matrix) / scale;
                const glm::quat q = glm::normalize(glm::quat_cast(rotation));
                const glm::mat3 reconstructed = glm::mat3_cast(q) * scale;
                for (int c = 0; c < 3; ++c)
                    for (int r = 0; r < 3; ++r)
                        if (std::abs(matrix[c][r] - reconstructed[c][r]) > 1e-4f)
                            return reject("controller base is not uniform NIF TRS");
                result.mChannels.push_back({ controller, glm::vec3(matrix[3]), q, scale });
            }
            for (std::size_t n = 0; n < nodes.size(); ++n)
            {
                if ((nodes[n].controllerFlags != 0) != (channelCounts[n] != 0))
                    return reject("source controller coverage is incomplete");
                if (used[n] && (nodes[n].name.empty() || !semanticLocal(nodes[n].localTransform)))
                    return reject("source node cannot be safely bound by name and NIF TRS");
                if (used[n] && std::count(result.mNodeNames.begin(), result.mNodeNames.end(), result.mNodeNames[n]) != 1)
                    return reject("ambiguous source node name");
                result.mBaseRotations.push_back(used[n] ? rotationOf(nodes[n].localTransform) : glm::quat(1,0,0,0));
                // The loader can represent immutable identity NiNodes as plain
                // groups. There is no matrix to seed for those nodes. Animated
                // nodes and nonidentity transforms must still bind exactly.
                result.mRequiredSeed.push_back(used[n]
                    && (nodes[n].controllerFlags != 0 || nodes[n].localTransform != glm::mat4(1.0f)));
            }
            result.mUsed = std::move(used);
            result.mValid = true;
            return result;
        }

        // Scratch lives with the instance/caller, never in the shared program.
        // A rejected evaluation publishes no partial pose.
        bool evaluate(float sourceTime, std::vector<glm::mat4>& pose) const
        {
            auto state = initialState();
            return evaluate(sourceTime, std::nullopt, {}, {}, {}, state, pose);
        }

        bool evaluate(float sourceTime, std::optional<float> controlledTime,
            std::span<const ExternalChannel> external, std::string_view accumulationBone,
            const std::array<float, 3>& accumulationAxes, State& state, std::vector<glm::mat4>& pose) const
        {
            using namespace RenderCore;
            if (!mValid || !std::isfinite(sourceTime) || (controlledTime && !std::isfinite(*controlledTime))
                || state.locals.size() != mLocals.size()) return false;
            auto locals = state.locals;
            const auto apply = [&](std::size_t index, const TransformTrackSample& sample) {
                // KeyframeController retains translation/scale on missing keys,
                // but resets missing rotation to the authored NIF rotation.
                auto& local = locals[index];
                const auto rotation = sample.rotation.value_or(mBaseRotations[index]);
                const float norm = glm::dot(rotation, rotation);
                if (!std::isfinite(norm) || norm < 1e-12f) return false;
                const float scale = sample.scale.value_or(std::cbrt(glm::determinant(glm::mat3(local))));
                const auto translation = sample.translation.value_or(glm::vec3(local[3]));
                local = glm::mat4_cast(glm::normalize(rotation));
                for (int c = 0; c < 3; ++c) local[c] *= scale;
                local[3] = glm::vec4(translation, 1.0f);
                return semantic_detail::finite(local);
            };
            for (const auto& channel : mChannels)
            {
                const auto time = channel.controller.autoPlay ? std::optional<float>(sourceTime) : controlledTime;
                TransformTrackSample sample;
                if (time)
                {
                    if (!safeTime(channel.controller.timing, *time)) return false;
                    sample = channel.controller.track.sample(channel.controller.timing.map(*time));
                }
                if (!apply(channel.controller.node.value(), sample)) return false;
            }
            std::vector<bool> externallyBound(mLocals.size());
            for (const auto& channel : external)
            {
                const auto found = std::find(mNodeNames.begin(), mNodeNames.end(), folded(std::string(channel.node)));
                if (found == mNodeNames.end()) return false;
                const auto index = static_cast<std::size_t>(found - mNodeNames.begin());
                if (!mUsed[index] || externallyBound[index] || !channel.controller
                    || !safeTime(channel.controller->timing, channel.time)) return false;
                externallyBound[index] = true;
                if (!apply(index, channel.controller->track.sample(channel.controller->timing.map(channel.time))))
                    return false;
                if (mNodeNames[index] == folded(std::string(accumulationBone)))
                    for (int axis = 0; axis < 3; ++axis)
                        if (accumulationAxes[axis] != 0) locals[index][3][axis] = 0;
            }
            std::vector<glm::mat4> output;
            output.reserve(mBonePaths.size());
            for (const auto& path : mBonePaths)
            {
                glm::mat4 local(1.0f);
                for (const auto n : path) local *= locals[n];
                if (!semantic_detail::finite(local)) return false;
                output.push_back(local);
            }
            pose = std::move(output);
            state.locals = std::move(locals);
            return true;
        }

    private:
        static glm::quat rotationOf(const glm::mat4& matrix)
        {
            const float scale = std::cbrt(glm::determinant(glm::mat3(matrix)));
            return glm::normalize(glm::quat_cast(glm::mat3(matrix) / scale));
        }
        static bool semanticLocal(const glm::mat4& matrix)
        {
            if (!RenderCore::semantic_detail::finite(matrix) || matrix[0][3] != 0 || matrix[1][3] != 0
                || matrix[2][3] != 0 || matrix[3][3] != 1) return false;
            const float scale = std::cbrt(glm::determinant(glm::mat3(matrix)));
            if (!std::isfinite(scale) || std::abs(scale) < 1e-7f) return false;
            const auto reconstructed = glm::mat3_cast(rotationOf(matrix)) * scale;
            for (int c = 0; c < 3; ++c)
                for (int r = 0; r < 3; ++r)
                    if (std::abs(matrix[c][r] - reconstructed[c][r]) > 1e-4f) return false;
            return true;
        }
        static bool safeTime(const ControllerTiming& timing, float input)
        {
            const float time = timing.frequency * input + timing.phase;
            const float delta = timing.stop - timing.start;
            if (!timing.valid() || !std::isfinite(time) || !std::isfinite(delta)) return false;
            if (delta > 0 && (time < timing.start || time > timing.stop))
            {
                const float cycles = (time - timing.start) / delta;
                if (!std::isfinite(cycles)) return false;
                if (timing.extrapolation == ControllerExtrapolation::Reverse
                    && std::abs(cycles) >= static_cast<float>(std::numeric_limits<int>::max())) return false;
            }
            return true;
        }
    };
}
#endif
