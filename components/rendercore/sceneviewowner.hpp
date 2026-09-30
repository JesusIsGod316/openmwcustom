#ifndef OPENMW_RENDERCORE_SCENEVIEWOWNER_H
#define OPENMW_RENDERCORE_SCENEVIEWOWNER_H

#include <array>
#include <osg/observer_ptr>
#include <osg/ref_ptr>
#include <osgUtil/CullVisitor>

namespace RenderCore
{
    // Pinned OSG 3.6.5 Renderer has exactly two SceneViews. A caller MUST
    // establish that visitor belongs to an acquired main-renderer SceneView,
    // with mono SingleThreaded/DrawThreadPerContext semantics. Its cull owner
    // may then recycle only that SceneView's payload after the previous draw
    // returned it to availableQueue. No frame-number/parity ownership exists.
    template <class Payload> class SceneViewOwner final
    {
    public:
        template <class Factory, class Capture>
        Payload* acquire(osgUtil::CullVisitor* visitor, Factory&& factory, Capture&& capture)
        {
            for (auto& slot : mSlots)
            {
                if (slot.visitor.valid() && slot.visitor.get() != visitor) continue;
                if (!slot.visitor.valid())
                {
                    slot.visitor = visitor;
                    slot.payload = factory();
                }
                else capture(*slot.payload);
                return slot.payload.get();
            }
            return nullptr; // An unknown third visitor must use the old path.
        }
    private:
        struct Slot
        {
            osg::observer_ptr<osgUtil::CullVisitor> visitor;
            osg::ref_ptr<Payload> payload;
        };
        std::array<Slot, 2> mSlots;
    };
}
#endif
