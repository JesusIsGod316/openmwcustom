#ifndef OPENMW_RESOURCE_TEXTURESTREAMING_H
#define OPENMW_RESOURCE_TEXTURESTREAMING_H

#include "nativetexture.hpp"
#include <osg/Texture2D>
#include <atomic>
#include <memory>

namespace osg { class State; }
namespace Resource::TextureStreaming
{
    enum class Skip : unsigned
    {
        Disabled, Purpose, Role, NotReady, OrdinaryAllocation, Context, Capability,
        Admission, AllocationLimit, RetirementDebt, Hold, GlFailure, Stale, TextureUnit, Count
    };
    struct Counters
    {
        std::atomic_uint64_t requested{0}, eligible{0}, boundaries{0}, allocations{0}, allocationBytes{0},
            uploads{0}, uploadedBytes{0}, coarsePublished{0}, finePublished{0}, demotions{0},
            coarseDraws{0}, fineDraws{0}, firstCoarseDraws{0}, firstFineDraws{0},
            retirements{0}, deletions{0}, deletedBytes{0}, liveBytes{0}, peakBytes{0}, contextDiscards{0};
        std::atomic_uint64_t compileDeferred{0},compileFallbacks{0},coldVisibleFallbacks{0};
        std::array<std::atomic_uint64_t,static_cast<unsigned>(Skip::Count)> skips{};
    };
    Counters& counters();
    const char* skipName(Skip);
    struct Descriptor;

    // OSG CompileTextureOp calls apply() directly. Only the production ICO
    // owner establishes this optional-compile intent; visible applies never do.
    class CompileScope
    {
    public:
        explicit CompileScope(bool allowDeferral);
        ~CompileScope();
        CompileScope(const CompileScope&)=delete;
        CompileScope& operator=(const CompileScope&)=delete;
    private:
        bool mChanged=false,mPreviousActive=false,mPreviousAllowed=false;
    };

    // Only the terrain producer creates this type. Semantic source images stay
    // full quality; the raw GPU objects and completion proofs are context local.
    class Texture final : public osg::Texture2D
    {
    public:
        Texture();
        Texture(osg::Image*,std::shared_ptr<NativeTextureTicket>);
        Texture(const Texture&,const osg::CopyOp& = osg::CopyOp::SHALLOW_COPY);
        ~Texture() override;
        META_StateAttribute(Resource,Texture,TEXTURE)
        int compare(const osg::StateAttribute&) const override;
        void apply(osg::State&) const override;
        void compileGLObjects(osg::State&) const override;
        void releaseGLObjects(osg::State* = nullptr) const override;
        const std::shared_ptr<Descriptor>& descriptor() const { return mDescriptor; }
    private:
        std::shared_ptr<Descriptor> mDescriptor;
    };

    // The engine's Renderer owner hook runs this before any of its scene views.
    // A queued GraphicsOperation is too late: OSG runs camera Renderers first.
    // Publication is frozen until the next owner boundary.
    void frameBoundary(osg::State&);
    // Called only after draw threads stop, while the original owner is current.
    void releaseContext(osg::State&);
    // TerrainDrawable calls after Geometry submitted its primitives; apply or
    // compilation alone cannot claim actual first draw use.
    void recordDrawUse(osg::State&);
    void reportDiagnostics();
#ifdef OPENMW_NATIVE_TEXTURE_TEST_HOOKS
    void setTestTime(double);
    void failNextUpload();
    void holdTestRetirements(bool);
#endif
}
#endif
