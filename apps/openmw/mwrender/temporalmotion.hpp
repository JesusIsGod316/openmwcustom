#ifndef OPENMW_MWRENDER_TEMPORALMOTION_H
#define OPENMW_MWRENDER_TEMPORALMOTION_H

#include <osg/Matrix>
#include <osg/Program>
#include <osg/Texture2D>
#include <osg/Vec2f>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>

namespace osg { class Geometry; class RenderInfo; class State; }
namespace MWRender
{
    struct TemporalDynamicFrame;
    // Stored in the existing ping/pong frame slot. The cull-owned projection
    // is retained, not copied early: OSG may finalize its near/far values when
    // it unwinds culling. Only the draw owner copies its final value.
    struct TemporalCamera
    {
        osg::ref_ptr<const osg::RefMatrix> projection;
        osg::Matrixd view;
        std::uint64_t frame = 0, cameraEpoch = 1, worldEpoch = 1, resourceEpoch = 1;
        unsigned renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
        bool zeroToOne = false;
        double clearDepth = 1.0;
        std::shared_ptr<const TemporalDynamicFrame> dynamic;
    };

    class TemporalMotion final
    {
    public:
        struct Status
        {
            std::uint64_t frame = 0, previousFrame = 0, targetRevision = 0;
            std::uint32_t resetReasons = 0;
            std::uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
            osg::Vec2f jitterPixels{};
            osg::Vec2f previousJitterPixels{};
            bool submitted = false;
            bool historyValid = false;
            // This first consumer is camera/static reconstruction only.
            // A DLSS adapter MUST also require a valid dynamic overlay.
            bool denseDynamicMotion = false;
            unsigned dynamicSurfaces = 0, unsupportedSurfaces = 0;
        };
        struct ConsumerFrame
        {
            Status status;
            osg::ref_ptr<osg::Texture2D> motion;
            std::array<float, 16> currentViewProjection{};
            std::array<float, 16> previousViewProjection{};
            std::array<float, 16> inverseViewProjection{};
            // Motion is current-to-previous, measured in render pixels with
            // +X right / +Y down. The future adapter must convert only if its
            // selected NVIDIA API contract requires another convention.
            bool motionInPixels = true;
        };
        explicit TemporalMotion(osg::Program* program, osg::Program* dynamicProgram = nullptr);
        ~TemporalMotion();
        TemporalMotion(const TemporalMotion&) = delete;
        TemporalMotion& operator=(const TemporalMotion&) = delete;

        // No readback, GPU wait, camera mutation or color substitution.
        // nullptr means invalid/unavailable input; normal presentation continues.
        osg::Texture2D* render(osg::RenderInfo&, const TemporalCamera&, osg::Texture2D* depth,
            const osg::Geometry& fullscreen);
        Status status(unsigned context) const;
        // Draw-owner access only, before the next render/release on this
        // context. The exact-frame gate protects metadata; the shared motion
        // target is not a cross-frame image lease for an async consumer.
        std::optional<ConsumerFrame> consumerFrame(unsigned context, std::uint64_t expectedFrame) const;
        void resizeGLObjectBuffers(unsigned size);
        void releaseGLObjects(osg::State* state);

        static bool enabled();
        static bool debugView();
        static bool ownershipEnabled();
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
#endif
