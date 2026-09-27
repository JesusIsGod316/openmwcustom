#ifndef OPENMW_MWRENDER_VULKANMW_NATIVEANIMATIONRUNTIME_H
#define OPENMW_MWRENDER_VULKANMW_NATIVEANIMATIONRUNTIME_H

#include <glm/mat4x4.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <string>
#include <vector>

namespace RenderCore
{
    struct SkeletonRecord;
}
namespace RenderNative { class SkeletalObjectProgram; }

namespace VFS
{
    class Manager;
}

namespace MWRender
{
    class Animation;

    enum class V4NativeAnimationPoseStatus : std::uint8_t
    {
        Applied,
        CompatibilityFallback,
    };

    struct V4NativeAnimationPoseResult
    {
        V4NativeAnimationPoseStatus status = V4NativeAnimationPoseStatus::CompatibilityFallback;
        std::uint32_t sampledTracks = 0;
        bool poseValidated = false;
        float poseMaximumError = 0.0f;
        std::string diagnostic;

        [[nodiscard]] bool applied() const noexcept
        {
            return status == V4NativeAnimationPoseStatus::Applied;
        }
    };

    // Phase 3C source-side runtime substitution.
    //
    // OpenMW's gameplay Animation state machine remains authoritative for group
    // priority, time, loops and text-key events. This adapter consumes only that
    // immutable selection snapshot and evaluates the winning-VFS KF source
    // directly into RenderCore skeleton-local transforms. Unsupported or
    // transition-sensitive states fail closed to the existing evaluated OSG
    // compatibility capture.
    class V4NativeAnimationRuntime final
    {
    public:
        explicit V4NativeAnimationRuntime(const VFS::Manager& vfs);
        ~V4NativeAnimationRuntime();

        V4NativeAnimationRuntime(const V4NativeAnimationRuntime&) = delete;
        V4NativeAnimationRuntime& operator=(const V4NativeAnimationRuntime&) = delete;

        void beginFrame();

        [[nodiscard]] V4NativeAnimationPoseResult captureSkeletonPose(
            std::string_view actorIdentity, const Animation& animation,
            const RenderCore::SkeletonRecord& skeleton, std::vector<glm::mat4>& localTransforms);

        [[nodiscard]] V4NativeAnimationPoseResult captureSkeletalObjectPose(std::string_view identity,
            const Animation& animation, const RenderNative::SkeletalObjectProgram& program,
            float simulationTime, std::vector<glm::mat4>& localTransforms);

        // Compatibility fallback seeds the exact current local pose once. Later
        // native frames then preserve the legacy controller's missing-channel
        // behavior without reading OSG bones again.
        [[nodiscard]] bool seedSkeletonPose(std::string_view actorIdentity,
            const RenderCore::SkeletonRecord& skeleton, std::span<const glm::mat4> localTransforms,
            std::string& diagnostic);

        void endFrame();
        void clear();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}

#endif
