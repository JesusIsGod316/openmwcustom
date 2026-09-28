#ifndef OPENMW_MWRENDER_V4OBJECTQUEUE_H
#define OPENMW_MWRENDER_V4OBJECTQUEUE_H

#include "animation.hpp"
#include "v4producerclass.hpp"
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
    // only, never by gameplay/update traversal. Producer scheduling distinguishes
    // understood continuous actors/particles from true compatibility fallback.
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
            const auto token = ticket->token();
            mAnimations.emplace(token, &animation);
            mClasses.emplace(token, V4ProducerClass::CompatibilityContinuous);
            ++mCompatibilityContinuous;
            mTickets.emplace(&animation, ticket);
            animation.attachV4ProducerTicket(std::move(ticket));
        }
        void remove(Animation& animation)
        {
            const auto it = mTickets.find(&animation);
            if (it != mTickets.end())
            {
                const auto token = it->second->token();
                it->second->cancel();
                mAnimations.erase(token);
                eraseClass(token);
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
                const auto previous = classFor(change.token);
                animation->beginV4ProducerVisit(previous, change.reasons);
                ++mLastVisited;
                try { visitor(*animation); }
                catch (...)
                {
                    // Retry an interrupted publication on the compatibility
                    // work set; never strand a now-unsupported dirty object.
                    animation->setV4ProducerDemandDriven(false);
                    updateClass(change.token, animation->finishV4ProducerVisit());
                    throw;
                }
                updateClass(change.token, animation->finishV4ProducerVisit());
            }
            // Capacity is bounded. Overflow is explicit compatibility work,
            // never an omitted object. Snapshot protects re-entrant removal.
            const std::vector<Animation*> overflow(mOverflow.begin(), mOverflow.end());
            for (auto* animation : overflow)
                if (mOverflow.contains(animation))
                {
                    osg::ref_ptr<Animation> keepAlive(animation);
                    animation->beginV4ProducerVisit(V4ProducerClass::CompatibilityContinuous, 1);
                    ++mLastVisited;
                    visitor(*animation);
                    (void)animation->finishV4ProducerVisit();
                }
        }
        std::size_t registered() const { return mQueue.size() + mOverflow.size(); }
        std::size_t visited() const noexcept { return mLastVisited; }
        std::size_t continuous() const { return mQueue.continuousSize() + mOverflow.size(); }
        std::size_t demandDriven() const noexcept { return mDemandDriven; }
        std::size_t supportedActors() const noexcept { return mSupportedActors; }
        std::size_t supportedParticles() const noexcept { return mSupportedParticles; }
        std::size_t compatibilityContinuous() const noexcept
        { return mCompatibilityContinuous + mOverflow.size(); }
    private:
        V4ProducerClass classFor(Misc::ProducerQueue::Token token) const noexcept
        {
            const auto it = mClasses.find(token);
            return it == mClasses.end() ? V4ProducerClass::CompatibilityContinuous : it->second;
        }
        void decrement(V4ProducerClass value) noexcept
        {
            switch (value)
            {
                case V4ProducerClass::DemandDrivenObject: --mDemandDriven; break;
                case V4ProducerClass::SupportedContinuousActor: --mSupportedActors; break;
                case V4ProducerClass::SupportedContinuousParticle: --mSupportedParticles; break;
                case V4ProducerClass::CompatibilityContinuous: --mCompatibilityContinuous; break;
            }
        }
        void increment(V4ProducerClass value) noexcept
        {
            switch (value)
            {
                case V4ProducerClass::DemandDrivenObject: ++mDemandDriven; break;
                case V4ProducerClass::SupportedContinuousActor: ++mSupportedActors; break;
                case V4ProducerClass::SupportedContinuousParticle: ++mSupportedParticles; break;
                case V4ProducerClass::CompatibilityContinuous: ++mCompatibilityContinuous; break;
            }
        }
        void updateClass(Misc::ProducerQueue::Token token, V4ProducerClass value)
        {
            const auto it = mClasses.find(token);
            if (it == mClasses.end() || it->second == value) return;
            decrement(it->second);
            it->second = value;
            increment(value);
        }
        void eraseClass(Misc::ProducerQueue::Token token)
        {
            const auto it = mClasses.find(token);
            if (it == mClasses.end()) return;
            decrement(it->second);
            mClasses.erase(it);
        }

        Misc::ProducerQueue mQueue;
        std::map<Misc::ProducerQueue::Token, osg::observer_ptr<Animation>> mAnimations;
        std::map<Misc::ProducerQueue::Token, V4ProducerClass> mClasses;
        std::map<Animation*, std::shared_ptr<Misc::ProducerQueue::Ticket>> mTickets;
        std::set<Animation*> mOverflow;
        std::uint64_t mStream = 0;
        bool mPreLight = false;
        std::size_t mLastVisited = 0;
        std::size_t mDemandDriven = 0;
        std::size_t mSupportedActors = 0;
        std::size_t mSupportedParticles = 0;
        std::size_t mCompatibilityContinuous = 0;
    };
}
#endif
