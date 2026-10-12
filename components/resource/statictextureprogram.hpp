#ifndef OPENMW_COMPONENTS_RESOURCE_STATICTEXTUREPROGRAM_H
#define OPENMW_COMPONENTS_RESOURCE_STATICTEXTUREPROGRAM_H

#include "openmwcompileoperation.hpp"
#include "v321classifiedcompileset.hpp"
#include "objectcache.hpp"

#include <components/debug/hitchtrace.hpp>
#include <osg/GraphicsContext>
#include <osg/Program>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/observer_ptr>

namespace Resource
{
    struct StaticTextureProgramAdmission { std::atomic_bool active{false}; };
    // Only explicit static producer resources belong here. This descriptor
    // never retains a cull visitor, dynamic RTT texture or borrowed StateSet.
    // It seeds the existing context; ordinary demand still validates/applies
    // the canonical texture and actual program variant without a ready bypass.
    struct StaticTextureProgramPreparation
    {
        // Payload references retire before this exact publication charge.
        ObjectCacheLease imageLease;
        std::shared_ptr<StaticTextureProgramAdmission> admission;
        ~StaticTextureProgramPreparation()
        {
            pass = nullptr; program = nullptr; texture = nullptr; image = nullptr;
            imageLease = ObjectCacheLease{};
            if (admission) admission->active.store(false, std::memory_order_release);
        }
        std::shared_ptr<V321ResourceCompileLifetime> lifetime = std::make_shared<V321ResourceCompileLifetime>();
        osg::ref_ptr<osg::Texture2D> texture;
        osg::ref_ptr<osg::Image> image;
        osg::ref_ptr<osg::Program> program;
        osg::ref_ptr<osg::StateSet> pass = new osg::StateSet;
        osg::observer_ptr<osg::GraphicsContext> context;
        unsigned imageRevision = 0;
        std::size_t bytes = 0;
        std::uint64_t receipt = 0;
        void cancel() { lifetime->cancel(); }
    };

    class StaticTextureProgramCompileOp final : public OpenMWCompositeCompileOp
    {
    public:
        StaticTextureProgramCompileOp(std::shared_ptr<StaticTextureProgramPreparation> owner, Kind kind)
            : mOwner(std::move(owner)), mKind(kind) {}
        Kind kind() const override { return mKind; }
        std::size_t resourceBytes() const override { return mKind == Kind::Texture ? mOwner->bytes : 0; }
        bool required() const override { return false; }
        bool cancelled() const override { return mOwner->lifetime->cancelled(); }
        double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override
        { return mKind == Kind::Program ? .001 : .00025; }
        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            if (cancelled()) return true;
            auto& state = *info.getState();
            if (!mOwner->context.valid() || mOwner->context.get() != state.getGraphicsContext()
                || info.incrementalCompileOperation->getContextSet().size() != 1
                || mOwner->texture->getImage() != mOwner->image.get()
                || mOwner->image->getModifiedCount() != mOwner->imageRevision)
            {
                mOwner->cancel();
                return true;
            }
            Debug::HitchTrace::Scope work(mKind == Kind::Texture ? "water.static_texture_prepare" : "water.static_program_prepare",
                Debug::HitchTrace::CpuTiming::WallEnvelope);
            work.setLink(mOwner->receipt);
            work.setMetric(0, "source_bytes", mOwner->bytes);
            work.setMetric(1, "context", state.getContextID());
            if (mKind == Kind::Texture)
                mOwner->texture->compileGLObjects(state);
            else
            {
                state.pushStateSet(mOwner->pass);
                struct Pop { osg::State& state; ~Pop() { state.popStateSet(); } } pop{state};
                mOwner->program->compileGLObjects(state);
            }
            return true;
        }
    private:
        std::shared_ptr<StaticTextureProgramPreparation> mOwner;
        Kind mKind;
    };

    class StaticTextureProgramCompleted final : public osgUtil::IncrementalCompileOperation::CompileCompletedCallback
    {
    public:
        explicit StaticTextureProgramCompleted(std::shared_ptr<V321ResourceCompileLifetime> lifetime)
            : mLifetime(std::move(lifetime)) {}
        bool compileCompleted(osgUtil::IncrementalCompileOperation::CompileSet*) override
        { mLifetime->complete(); return true; }
    private:
        std::shared_ptr<V321ResourceCompileLifetime> mLifetime;
    };

    inline std::shared_ptr<StaticTextureProgramPreparation> prepareStaticTextureProgram(
        osgUtil::IncrementalCompileOperation* ico, osg::Texture2D* texture, osg::Program* program,
        const std::shared_ptr<StaticTextureProgramAdmission>& admission, ObjectCacheLease imageLease)
    {
        if (!ico || ico->getContextSet().size() != 1 || !texture || !program || !texture->getImage()
            || texture->getImage()->getDataVariance() == osg::Object::DYNAMIC || texture->getImage()->requiresUpdateCall())
            return {};
        const auto extent=imagePayloadExtent(*texture->getImage());
        const auto bytes=extent.bytes;
        if (!bytes || bytes > 32u * 1024u * 1024u || !imageLease || !imageLease.mCharge
            || !imageLease.mSourceRevision || imageLease.mValue.get() != texture->getImage()
            || imageLease.bytes() != bytes) return {};
        auto owner = std::make_shared<StaticTextureProgramPreparation>();
        bool inactive = false;
        if (!admission || !admission->active.compare_exchange_strong(inactive, true, std::memory_order_acq_rel)) return {};
        owner->admission = admission;
        owner->imageLease = std::move(imageLease);
        owner->texture = texture;
        owner->image = texture->getImage();
        owner->imageRevision = owner->image->getModifiedCount();
        owner->program = program;
        owner->bytes = bytes;
        owner->context = *ico->getContextSet().begin();
        owner->pass->setAttributeAndModes(program, osg::StateAttribute::ON);
        owner->pass->setTextureAttribute(0, texture);
        Debug::HitchTrace::Scope receipt("water.static_prepare_admit", Debug::HitchTrace::CpuTiming::WallEnvelope);
        receipt.retain();
        owner->receipt = receipt.id();
        receipt.setMetric(0, "source_bytes", bytes);
        receipt.setMetric(1, "operations", 2);
        osg::ref_ptr<V321ClassifiedCompileSet> set = new V321ClassifiedCompileSet(
            owner->lifetime, V321CompileClass::GenericModel, V321CompileUrgency::Background);
        auto& list = set->_compileMap[owner->context.get()];
        list.add(new StaticTextureProgramCompileOp(owner, OpenMWCompositeCompileOp::Kind::Texture));
        list.add(new StaticTextureProgramCompileOp(owner, OpenMWCompositeCompileOp::Kind::Program));
        ++set->_numberCompileListsToCompile;
        set->_compileCompletedCallback = new StaticTextureProgramCompleted(owner->lifetime);
        ico->add(set, false);
        return owner;
    }
}
#endif
