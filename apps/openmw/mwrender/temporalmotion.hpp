#ifndef OPENMW_MWRENDER_TEMPORALMOTION_H
#define OPENMW_MWRENDER_TEMPORALMOTION_H

#include <osg/Matrix>
#include <osg/Program>
#include <osg/Texture2D>
#include <cstdint>
#include <memory>

namespace osg { class Geometry; class RenderInfo; class State; }
namespace MWRender
{
    // Stored in the existing ping/pong frame slot. The cull-owned projection
    // is retained, not copied early: OSG may finalize its near/far values when
    // it unwinds culling. Only the draw owner copies its final value.
    struct TemporalCamera
    {
        osg::ref_ptr<const osg::RefMatrix> projection;
        osg::Matrixd view;
        std::uint64_t frame = 0, cameraEpoch = 1, worldEpoch = 1;
        unsigned renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
        bool zeroToOne = false;
        double clearDepth = 1.0;
    };

    class TemporalMotion final
    {
    public:
        struct Status
        {
            std::uint64_t frame = 0, previousFrame = 0, targetRevision = 0;
            std::uint32_t resetReasons = 0;
            bool submitted = false;
            // This first consumer is camera/static reconstruction only.
            // A future DLSS adapter MUST also require a valid dynamic overlay.
            bool denseDynamicMotion = false;
        };
        explicit TemporalMotion(osg::Program* program);
        ~TemporalMotion();
        TemporalMotion(const TemporalMotion&) = delete;
        TemporalMotion& operator=(const TemporalMotion&) = delete;

        // No readback, GPU wait, camera mutation or color substitution.
        // nullptr means invalid/unavailable input; normal presentation continues.
        osg::Texture2D* render(osg::RenderInfo&, const TemporalCamera&, osg::Texture2D* depth,
            const osg::Geometry& fullscreen);
        Status status(unsigned context) const;
        void resizeGLObjectBuffers(unsigned size);
        void releaseGLObjects(osg::State* state);

        static bool enabled();
        static bool debugView();
    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };
}
#endif
