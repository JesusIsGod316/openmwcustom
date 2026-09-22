#ifndef OPENMW_MWRENDER_V4UPDATEONLYVIEWER_H
#define OPENMW_MWRENDER_V4UPDATEONLYVIEWER_H

#include <components/sceneutil/updateonlyvisitor.hpp>
#include <components/sceneutil/particleplayback.hpp>
#include <osgGA/Device>
#include <osgGA/EventVisitor>
#include <osgViewer/Viewer>
#include <stdexcept>

namespace MWRender
{
    // SDL/Vulkan owns window lifetime and presentation. Keep canonical OSG CPU
    // updateTraversal/advance (including camera callbacks and frame stamps), but
    // do not use Viewer::eventTraversal's "no GraphicsWindow => done" policy.
    class V4UpdateOnlyViewer final : public osgViewer::Viewer
    {
    public:
        V4UpdateOnlyViewer()
        {
            setThreadingModel(osgViewer::ViewerBase::SingleThreaded);
            setUpdateVisitor(new SceneUtil::UpdateOnlyVisitor);
            setKeyEventSetsDone(0); // Escape belongs to OpenMW, not Viewer shutdown.
            setQuitEventSetsDone(false); // SDL input/state manager owns normal quit.
        }

        void eventTraversal() override
        {
            if (done())
                return;
            const double begin = elapsedTime();
            const double cutoff = getFrameStamp()->getReferenceTime();
            osgGA::EventQueue::Events events;
            for (const auto& source : _eventSources)
            {
                if (source->getCapabilities() & osgGA::Device::RECEIVE_EVENTS)
                    source->checkEvents();
                source->getEventQueue()->takeEvents(events, cutoff);
            }
            getEventQueue()->frame(cutoff);
            getEventQueue()->takeEvents(events, cutoff);
            for (const auto& event : events)
            {
                const auto* gui = event->asGUIEventAdapter();
                if (!gui || gui->getHandled())
                    continue;
                if ((gui->getEventType() == osgGA::GUIEventAdapter::KEYUP
                        && _keyEventSetsDone != 0 && gui->getKey() == _keyEventSetsDone)
                    || (gui->getEventType() == osgGA::GUIEventAdapter::QUIT_APPLICATION && _quitEventSetsDone))
                    setDone(true);
            }
            if (done())
                return;

            if (_eventVisitor.valid())
            {
                _eventVisitor->setActionAdapter(this);
                _eventVisitor->setFrameStamp(getFrameStamp());
                _eventVisitor->setTraversalNumber(getFrameStamp()->getFrameNumber());
                for (const auto& event : events)
                {
                    _eventVisitor->reset();
                    _eventVisitor->addEvent(event);
                    if (getSceneData())
                        getSceneData()->accept(*_eventVisitor);
                    for (unsigned int i = 0; i < getNumSlaves(); ++i)
                    {
                        auto& slave = getSlave(i);
                        if (slave._camera.valid() && !slave._useMastersSceneData)
                            slave._camera->accept(*_eventVisitor);
                    }
                    // Master/shared-scene camera callbacks must not visit the
                    // scene a second time. Mirror ordinary Viewer ordering.
                    const auto mode = _eventVisitor->getTraversalMode();
                    _eventVisitor->setTraversalMode(osg::NodeVisitor::TRAVERSE_NONE);
                    if (getCamera())
                        getCamera()->accept(*_eventVisitor);
                    for (unsigned int i = 0; i < getNumSlaves(); ++i)
                    {
                        auto& slave = getSlave(i);
                        if (slave._camera.valid() && slave._useMastersSceneData)
                            slave._camera->accept(*_eventVisitor);
                    }
                    _eventVisitor->setTraversalMode(mode);
                }
                for (const auto& event : events)
                    for (const auto& handler : _eventHandlers)
                        handler->handle(event.get(), nullptr, _eventVisitor.get());
                if (_cameraManipulator.valid())
                    for (const auto& event : events)
                        _cameraManipulator->handle(event.get(), nullptr, _eventVisitor.get());
            }
            if (getViewerStats() && getViewerStats()->collectStats("event"))
            {
                const auto frame = getFrameStamp()->getFrameNumber();
                const double end = elapsedTime();
                getViewerStats()->setAttribute(frame, "Event traversal begin time", begin);
                getViewerStats()->setAttribute(frame, "Event traversal end time", end);
                getViewerStats()->setAttribute(frame, "Event traversal time taken", end - begin);
            }
        }

        void updateTraversal() override
        {
            osgViewer::Viewer::updateTraversal();
            if (osg::Node* scene = getSceneData())
            {
                mParticles.setFrameStamp(getFrameStamp());
                mParticles.setTraversalNumber(getFrameStamp()->getFrameNumber());
                mParticles.setTraversalMask(getCamera()->getCullMask());
                scene->accept(mParticles);
            }
        }

        // An accidental legacy presentation path is a contract error, not a
        // reason to create an OpenGL context or silently lose a frame.
        void realize() override { throw std::logic_error("Vulkan CPU viewer must not realize an OpenGL window"); }
        void renderingTraversals() override
        {
            throw std::logic_error("Vulkan CPU viewer must not perform OpenGL rendering traversals");
        }
    private:
        SceneUtil::ParticlePlaybackVisitor mParticles;
    };
}

#endif
