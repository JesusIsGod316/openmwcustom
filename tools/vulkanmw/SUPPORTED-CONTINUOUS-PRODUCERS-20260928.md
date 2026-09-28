# VulkanMW supported-continuous producers — 2026-09-28

Parent checkpoint: `vulkanmw/phase3c-producer-dirty-queues@3a83d7d3f96f01f63532449601fb331280c1befe`.

This slice separates **known continuous work** from **compatibility fallback**. It does not claim that actors or
particles are fully native yet. Actors still need a per-frame transform/pose/morph/effect/light update; live NIF
particles still use OpenMW's authoritative update-only simulation and an evaluated particle snapshot. The change is
that these known paths are now explicitly scheduled and measured as supported producers rather than being mixed with
unknown callbacks/raw mutable OSG compatibility.

## Scheduling classes

- `DemandDrivenObject`: proven ordinary NIF producer; sleeps until producer-owned dirty notification.
- `SupportedContinuousActor`: actor route succeeded and is deliberately visited each frame.
- `SupportedContinuousParticle`: particle actor or intrinsic-particle object route succeeded and is deliberately
  visited each frame.
- `CompatibilityContinuous`: fail-closed lane for uncovered/unknown mutation or unsupported content.

Classification is committed only after a successful visit. Each visit starts fail-closed, so a route that stops being
supported cannot remain mislabeled. Queue registration and generation safety are unchanged. The complete slice is
same-executable switchable with `OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS=1`; without it, the green producer-queue
checkpoint keeps the prior compatibility-continuous behavior.

## Particle body/simulation split

An intrinsic-particle object's ordinary NIF body can now own persistent draw slots event-driven even while its particle
simulation remains continuous. A particle object is classified as supported-continuous only when its ordinary body has
a valid producer-owned event-driven mutation contract; if that body falls back to generic binding/subtree capture, the
whole object remains compatibility-continuous rather than hiding partial fallback behind the particle label. Once a
particle producer has been proven and its body is clean, a continuous-only queue visit can skip the body publication
entirely and capture only current particles. Stream changes, dirty notifications, late mutation-source `changed` state,
invalidation, or lack of persistent draws force a normal body publication.

This is intentionally narrower than native particle simulation. Free EffectManager VFX, projectile VFX, weather/ripple
particles, and unsupported/custom particle graphs are not reclassified by this slice.

## Diagnostics / acceptance

`producer_queue` now reports `demand_driven`, `supported_actors`, `supported_particles`, and
`compatibility_continuous` in addition to registered/continuous/visited. `object_producer_work` reports
`particle_body_sleeps`.

The packaged `START-VulkanMW-Supported-Producers-Test.bat` runs A/B/B/A with dirty queues enabled in every arm and
changes only `OPENMW_VK_SUPPORTED_CONTINUOUS_PRODUCERS`. The first runtime goal is classification stability: steady
supported actors/particles should stop inflating the true compatibility count. For intrinsic-particle objects, clean body sleeps should be nonzero where present. Whole-frame
performance remains secondary until the same-executable benchmark shows a repeatable effect. Do not infer a win from
counter changes alone.

Next actor work should split structural/equipment/model dirties from unavoidable per-frame pose/placement updates, but
only after all relevant actor mutation sources are audited. GPU skin/morph remains the later high-value actor step.
