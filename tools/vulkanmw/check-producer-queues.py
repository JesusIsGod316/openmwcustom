#!/usr/bin/env python3
"""Source integration guard; executable queue/capture tests are the behavior gate."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def read(path):
    return (ROOT / path).read_text(encoding='utf-8')

def require(path, *snippets):
    text = read(path)
    for snippet in snippets:
        if snippet not in text:
            raise AssertionError(f'{path}: missing {snippet}')
    return text

def main():
    bridge = require('apps/openmw/mwrender/v4enginerenderbridge.cpp',
        'rendering.forEachV4Animation(', 'mPersistentDraws.stream()',
        'producer->eventDriven()', 'animation.setV4ProducerDemandDriven(true)',
        'animation.setV4ProducerSupportedActor()', 'animation.setV4ProducerSupportedParticle()',
        'const bool queuedPublication', 'particleBodyCanSleep', 'OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS')
    if 'rendering.forEachAnimation(' in bridge:
        raise AssertionError('Bridge regained broad per-frame object inventory traversal')
    require('apps/openmw/mwrender/renderingmanager.cpp',
        'OPENMW_VK_PRODUCER_DIRTY_QUEUES', 'OPENMW_V4_PERSISTENT_DRAW_STREAM',
        'OPENMW_V4_LOAD_BOUND_TEXTURES', 'forEachAnimation(visitor)',
        'mObjects->forEachV4Animation', 'mObjects->invalidateV4Producers()')
    require('apps/openmw/mwrender/objects.cpp',
        'registerV4Animation', 'unregisterV4Animation', 'mV4ObjectQueue->visit',
        'queue->add(*animation)', 'mV4ObjectQueue = std::move(queue)')
    animation = require('apps/openmw/mwrender/animation.cpp',
        'void Animation::invalidateV4PersistentObject()',
        'void Animation::attachV4ProducerTicket(', 'mV4ProducerTicket->notify()',
        'source->subscribeRenderMutations(mV4ProducerWake)',
        'mV4ProducerTicket->continuous(v4ProducerClassUsesCompatibilityContinuousQueue(mV4ProducerClass))')
    if animation.count('mV4PersistentObject.reset();') != 1:
        raise AssertionError('A producer reset bypasses engine dirty notification')
    require('apps/openmw/mwrender/v4objectqueue.hpp', 'mQueue.take()',
        'mQueue.valid(change.token)', 'mOverflow', 'beginV4ProducerVisit',
        'SupportedContinuousActor', 'SupportedContinuousParticle',
        'v4ProducerClassIsSupportedContinuous(producerClass)', '!handled.contains(token)',
        'genericContinuous()', 'supportedContinuous()',
        'mQueue.invalidateAll()', 'it->second->cancel()')
    producer = require('apps/openmw/mwrender/v4persistentobject.hpp',
        'mNotificationCovered', 'mOwner->eventDriven', 'mOwner->retire()',
        'bodyEventDriven()', 'canReuseBodyWithoutVisit()',
        'mSubscription->changed.exchange(false', 'mSubscription->wake', 'resetWorld')
    if 'mSubscription->changed = false' in producer:
        raise AssertionError('Publication can discard a notification raised during consumption')
    world = require('components/rendercore/persistentdraw.hpp', 'mRetirements->take()',
        'mEphemeral.begin()', 'owner->eventDriven', 'owner->registrationStream != mStream')
    if 'slot < mSeen.size()' in world:
        raise AssertionError('Clean explicitly owned draws regained the full liveness sweep')
    require('components/sceneutil/rendermutation.hpp', 'observer->notify(true)', 'std::atomic_bool', 'if (wake) wake()')
    require('components/sceneutil/positionattitudetransform.hpp', 'void setNodeMask(', 'void setReferenceFrame(')
    require('apps/openmw/CMakeLists.txt', 'START-VulkanMW-Producer-Test.bat',
        'START-VulkanMW-Supported-Producers-Test.bat', 'run-producer-queues.py')
    print('Producer dirty queue source integration PASS')

if __name__ == '__main__':
    main()
