#ifndef OPENMW_MWRENDER_V4OBJECTQUEUE_H
#define OPENMW_MWRENDER_V4OBJECTQUEUE_H

#include "animation.hpp"
#include <components/misc/producerqueue.hpp>
#include <osg/observer_ptr>
#include <cstdint>
#include <functional>
#include <vector>
#include <map>
#include <set>

namespace MWRender
{
    // Objects owns membership. This registry is consulted by Vulkan rendering
    // only, never by gameplay/update traversal. Clean supported objects are NOT
    // visited; actors, live particles and unsupported content stay continuous.
    class V4ObjectQueue
    {
    public:
        void add(Animation& animation)
        {
            remove(animation);
            auto ticket = mQueue.add();
            if (!ticket)
            {
                mOverflow.insert(&animation);
                return;
            }
            ticket->continuous(true); // fail closed until a successful publication
            mAnimations.emplace(ticket->token(), &animation);
            mTickets.emplace(&animation, ticket);
            animation.attachV4ProducerTicket(std::move(ticket));
        }
        void remove(Animation& animation)
        {
            const auto it = mTickets.find(&animation);
            if (it != mTickets.end())
            {
                it->second->cancel();
                mAnimations.erase(it->second->token());
                animation.attachV4ProducerTicket({});
                mTickets.erase(it);
            }
            mOverflow.erase(&animation);
        }
        // Called only at explicit settings/world boundaries, not every frame.
        void invalidate()
        {
            for (auto& [_, observer] : mAnimations)
            {
                osg::ref_ptr<Animation> animation;
                if (observer.lock(animation)) animation->invalidateV4PersistentObject();
            }
            mQueue.invalidateAll();
        }
        void visit(const std::function<void(Animation&)>& visitor, std::uint64_t stream, bool preLight)
        {
            if (mStream != stream || mPreLight != preLight)
            {
                mStream = stream; mPreLight = preLight;
                // First/changed streams must rebind handles even if the engine
                // produced no object delta. No transform/material polling.
                mQueue.invalidateAll();
            }
            const auto work = mQueue.take();
            mLastVisited = 0;
            for (const auto& change : work)
            {
                const auto found = mAnimations.find(change.token);
                if (found == mAnimations.end() || !mQueue.valid(change.token)) continue;
                osg::ref_ptr<Animation> animation;
                if (!found->second.lock(animation)) continue;
                animation->setV4ProducerDemandDriven(false);
                ++mLastVisited;
                try { visitor(*animation); }
                catch (...)
                {
                    // Retry an interrupted publication on the compatibility
                    // work set; never strand a now-unsupported dirty object.
                    animation->setV4ProducerDemandDriven(false);
                    animation->finishV4ProducerVisit();
                    throw;
                }
                animation->finishV4ProducerVisit();
            }
            // Capacity is bounded. Overflow is explicit compatibility work,
            // never an omitted object. Snapshot protects re-entrant removal.
            const std::vector<Animation*> overflow(mOverflow.begin(), mOverflow.end());
            for (auto* animation : overflow)
                if (mOverflow.contains(animation))
                {
                    osg::ref_ptr<Animation> keepAlive(animation);
                    ++mLastVisited;
                    visitor(*animation);
                }
        }
        std::size_t registered() const { return mQueue.size() + mOverflow.size(); }
        std::size_t visited() const noexcept { return mLastVisited; }
        std::size_t continuous() const { return mQueue.continuousSize() + mOverflow.size(); }
    private:
        Misc::ProducerQueue mQueue;
        std::map<Misc::ProducerQueue::Token, osg::observer_ptr<Animation>> mAnimations;
        std::map<Animation*, std::shared_ptr<Misc::ProducerQueue::Ticket>> mTickets;
        std::set<Animation*> mOverflow;
        std::uint64_t mStream = 0;
        bool mPreLight = false;
        std::size_t mLastVisited = 0;
    };
}
#endif
