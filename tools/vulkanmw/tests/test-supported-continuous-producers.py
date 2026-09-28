#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]

def require(text: str, needle: str, why: str) -> None:
    if needle not in text:
        raise SystemExit(f"missing {why}: {needle}")

producer = (ROOT / 'apps/openmw/mwrender/v4producerclass.hpp').read_text(encoding='utf-8')
queue = (ROOT / 'apps/openmw/mwrender/v4objectqueue.hpp').read_text(encoding='utf-8')
bridge = (ROOT / 'apps/openmw/mwrender/v4enginerenderbridge.cpp').read_text(encoding='utf-8')
persistent = (ROOT / 'apps/openmw/mwrender/v4persistentobject.hpp').read_text(encoding='utf-8')
objects = (ROOT / 'apps/openmw/mwrender/objects.cpp').read_text(encoding='utf-8')

for needle, why in [
    ('DemandDrivenObject', 'demand-driven producer class'),
    ('SupportedContinuousActor', 'supported actor producer class'),
    ('SupportedContinuousParticle', 'supported particle producer class'),
    ('CompatibilityContinuous', 'compatibility producer class'),
]:
    require(producer, needle, why)

require(queue, 'previous = classFor(change.token)', 'per-registration previous class handoff')
require(queue, 'animation->beginV4ProducerVisit(previous, change.reasons)', 'dirty reason handoff')
require(queue, 'updateClass(change.token, animation->finishV4ProducerVisit())', 'classification commit')
require(queue, 'animation->setV4ProducerDemandDriven(false);', 'exception fail-closed classification')
require(queue, 'mSupportedActors', 'supported actor accounting')
require(queue, 'mSupportedParticles', 'supported particle accounting')
require(queue, 'mCompatibilityContinuous', 'compatibility accounting')
require(queue, 'v4ProducerClassIsSupportedContinuous(producerClass)', 'dedicated supported-continuous lane')
require(queue, 'genericContinuous()', 'generic fallback-continuous accounting')
require(queue, 'supportedContinuous()', 'direct supported-continuous accounting')
require(queue, '!handled.contains(token)', 'dirty supported producer duplicate suppression')
require(producer, 'v4ProducerClassUsesCompatibilityContinuousQueue', 'compatibility-only generic queue policy')

require(bridge, 'animation.setV4ProducerSupportedActor()', 'actor promotion after successful route')
require(bridge, 'animation.setV4ProducerSupportedParticle()', 'particle promotion after successful route')
require(bridge, 'particleBodyCanSleep', 'particle body clean-frame bypass')
require(bridge, 'objectProducer->bodyEventDriven() && !capturedEffects',
        'particle classification requires a supported event-driven ordinary body')
require(bridge, '!producer->eventDriven() && !supportedParticleBody',
        'supported particle bodies do not report as compatibility fallbacks')
require(bridge, 'OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS', 'same-executable supported producer control')
require(bridge, 'animation.previousV4ProducerClass() == V4ProducerClass::SupportedContinuousParticle',
        'particle bypass restricted to established supported particle lane')
require(bridge, '!animation.v4ProducerVisitIsDirty()', 'particle body bypass refuses dirty visits')
require(bridge, 'producer->canReuseBodyWithoutVisit()', 'particle body bypass checks producer-owned mutation state')

require(persistent, 'bool bodyEventDriven() const noexcept', 'body ownership distinct from simulation cadence')
require(persistent, 'OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS', 'particle body ownership switch')
require(persistent, 'return bodyEventDriven() && !mHasIntrinsicParticles;',
        'particles stay continuous while body can be event-driven')
require(persistent, '!mSubscription->changed.load', 'body clean reuse checks late mutation race')

for needle in ['"supported_actors"', '"supported_particles"', '"compatibility_continuous"',
               '"generic_continuous"', '"direct_continuous"']:
    require(objects, needle, 'runtime producer class diagnostics')

# Negative guard: supported continuous is not the same thing as demand driven.
if 'SupportedContinuousActor = DemandDrivenObject' in producer or 'SupportedContinuousParticle = DemandDrivenObject' in producer:
    raise SystemExit('supported continuous producer accidentally aliased to demand-driven scheduling')
if 'mV4ProducerTicket->continuous(v4ProducerClassIsContinuous' in (ROOT / 'apps/openmw/mwrender/animation.cpp').read_text(encoding='utf-8'):
    raise SystemExit('supported producers regained generic ProducerQueue continuous membership')

print('supported continuous producer source contract passed')
