#ifndef OPENMW_COMPONENTS_SCENEUTIL_PARTICLEPLAYBACK_H
#define OPENMW_COMPONENTS_SCENEUTIL_PARTICLEPLAYBACK_H

#include "skeleton.hpp"
#include <osgParticle/ParticleProcessor>
#include <osgParticle/ParticleSystemUpdater>
#include <osgUtil/CullVisitor>
#include <utility>
#include <vector>

namespace SceneUtil
{
    // OSG 3.6 runs CPU emitters/programs/updaters in their cull-phase traverse,
    // after all update callbacks. Visit only those leaf nodes with a real,
    // isolated CullVisitor. Never cull the scene or submit a drawable to it.
    // This retains canonical clocks, once-per-frame guards, emitter transforms,
    // operator ordering and virtual process/update implementations.
    class ParticlePlaybackVisitor final : public osg::NodeVisitor
    {
    public:
        ParticlePlaybackVisitor()
            : osg::NodeVisitor(TRAVERSE_ACTIVE_CHILDREN)
            , mSimulation(new osgUtil::CullVisitor)
            , mStamp(new osg::FrameStamp)
        {
            mSimulation->setFrameStamp(mStamp);
        }

        void apply(osg::Node& node) override
        {
            if (const auto* skeleton = dynamic_cast<const Skeleton*>(&node); skeleton && !skeleton->getActive())
                return;
            auto* processor = dynamic_cast<osgParticle::ParticleProcessor*>(&node);
            auto* updater = dynamic_cast<osgParticle::ParticleSystemUpdater*>(&node);
            if (!processor && !updater)
            {
                traverse(node);
                return;
            }
            struct CullGateScope
            {
                std::vector<std::pair<osgParticle::ParticleSystem*, bool>> systems;
                void add(osgParticle::ParticleSystem* system)
                {
                    if (!system) return;
                    systems.emplace_back(system, system->getFreezeOnCull());
                    system->setFreezeOnCull(false);
                }
                ~CullGateScope()
                {
                    for (auto it = systems.rbegin(); it != systems.rend(); ++it)
                        it->first->setFreezeOnCull(it->second);
                }
            } scope;
            if (processor) scope.add(processor->getParticleSystem());
            if (updater)
                for (unsigned i = 0; i < updater->getNumParticleSystems(); ++i)
                    scope.add(updater->getParticleSystem(i));
            if (!getFrameStamp())
                return;
            *mStamp = *getFrameStamp();
            mSimulation->setTraversalNumber(getTraversalNumber());
            mSimulation->getNodePath() = getNodePath();
            // Both concrete base types derive from Node, not Group: this call
            // cannot submit child geometry or execute update callbacks twice.
            node.traverse(*mSimulation);
        }

    private:
        osg::ref_ptr<osgUtil::CullVisitor> mSimulation;
        osg::ref_ptr<osg::FrameStamp> mStamp;
    };
}
#endif
