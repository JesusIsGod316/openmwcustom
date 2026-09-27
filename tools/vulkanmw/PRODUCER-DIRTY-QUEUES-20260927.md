# VulkanMW producer-owned dirty queues — 2026-09-27

Base: `348bfe6b087b7c8b83738e12a11a7e10f9e592ef`.
Scope: supported ordinary object producer dispatch and explicit persistent draw
lifetime. This is NOT completion of the entire OSG-to-native migration, actor GPU
deformation, general material semantic publication, or Vulkan visual parity.

## Old work removed

The 348bfe6 producer could skip bindings inside a clean `publish`, but the bridge
still walked all `Animation` objects and touched every owner every frame. The
persistent draw world still swept resident slots to discover dead owners.

With `OPENMW_VK_PRODUCER_DIRTY_QUEUES=1`, `Objects` owns a bounded registry and a
coalescing dirty queue. Initial load/registration is inspected once; after a
successful eligible publication, a clean supported object is not visited by the
Vulkan capture dispatcher. The remaining work list is changed owners plus the
continuous compatibility set. Draw residency ends through explicit retirement,
not through a missed frame touch. Clean frames can keep the same immutable frame
snapshot. Normal OSG gameplay/animation updates remain unchanged.

The queue works only together with persistent draw stream, native object
producers, change-driven objects, and load-bound texture identities. Otherwise
the exact broad-capture control remains selectable. A present renderer flag is
not sufficient to bypass the ownership checks.

## Producer ownership and invalidation

| Engine/source event | Action |
| --- | --- |
| Object insert / restored hibernated cell | New generation-safe registration, initially dirty and continuous until classified |
| Position, scale, attitude, root mask/reference frame | Tracked root setter enqueues its owner |
| Supported NIF transform, visibility, material, UV controller | Existing mutation source wakes the registered owner; repeated notifications coalesce |
| Source destruction or a callback becoming untracked | Invalidate and wake; retain compatibility rather than stale bindings |
| Root/model/controller rebind, harvest, glow, effects, alpha, light, reference replacement | Destroy old producer, retire its draws, invalidate cached admission and enqueue replacement |
| Global rendering settings | One explicit resident invalidation at the settings boundary, not polling |
| Cell/object removal or hibernation | Cancel generation-safe registration and retire publisher before deferred unref |
| World/stream change | One re-publication of registrations; old stream handles cannot mutate new residents |
| Queue capacity reached | Explicit continuous overflow set; no object silently omitted |

Registration and consumption remain engine-thread-owned. Notification tickets
contain no scene pointers. The bounded queue supports concurrent signalling and
cancellation; notifications raised after a drain are retained for the next one.
Queued stale generations are ignored. A publication failure stays continuous.

The existing bindings still evaluate the affected owner's hierarchy on a dirty
publication to propagate inherited transforms/materials. This change removes
global discovery and clean-owner inspection; it does not claim a direct
buffer-only implementation for every controller class.

## Eligibility / compatibility

Only an exact ordinary `ObjectAnimation` created from NIF with proven tracked
callbacks and engine invalidation coverage can sleep. Existing static native
publication is not duplicated. Actors, native skeletal props, intrinsic
particles, active effects/lights/transparency overrides, unknown callbacks,
raw mutable OSG transforms, skeleton activity and LOD ranges remain continuous
or use their existing specialized path. OSG-native model import is preserved.
Uncovered data does not become supported by setting a flag or inventing state.

## Persistent lifetime

Event-driven owners have explicit retirement tickets. The world tracks their
slots separately from ephemeral per-frame owners and scans only the latter for
missed captures. Removal queues own slot handles; submitted immutable frames
retain their mesh/material resources until normal GPU-safe retirement. Hidden
owners retain pending material/UV changes until revealed. Slot limits are still
bounded; retired owners can release capacity for a replacement in the same frame.

## Validation and limitations

Executable coverage includes queue coalescing, late notification, generation
reuse, cancellation after snapshot, bounded overflow, destruction/copy behavior,
concurrent signalling and atomically owned retirement tickets, and 10,000 registered owners with only 4 continuous + 1
dirty owner dispatched per frame. RenderCore tests cover untouched snapshots,
hide/reveal, full-capacity unload/reload, stale retirements and epochs. The actual
OSG capture fixture covers movement/scale, hidden UV updates, source invalidation,
unknown transform fallback and destruction without repeated clean publication.

The inherited architecture guard is **not globally green**: unchanged FX files
`fximagecapture.hpp` and `omwfx.cpp` still violate its OSG backend boundary. This
work leaves that guard intact, reports its failures and requires no new
violations relative to exact 348bfe6. The separate shadow-anchor non-finite
projection issue is not repaired or reclassified by this task.

No new game/frame-rate, GPU, full mod/save or visual parity result is claimed from
unit tests or successful builds. Runtime acceptance requires the queue to handle
real supported content and a matched whole-frame result, not just fewer counts.

## Packaged test entrypoint

All installed Windows Vulkan packages contain `START-VulkanMW-Test.bat`, now
forwarding to `START-VulkanMW-Producer-Test.bat` when the isolated harness is
present. OpenGL-only packaging is unchanged. The default is A/B/B/A, one exact
executable with the same cumulative publication/actor/admission/change-driven
controls in every arm; ONLY the producer queue flag changes. No parallel-shadow,
tile, postprocessing or visual-quality experiment is mixed into this comparison.

Each run preserves content order and uses private settings and user data with the
registered `vsync mode` setting. The harness verifies executable/source identity
and writes a benchmark ZIP, including available evidence on failure. `--single
candidate` is an explicitly labelled smoke test, not a performance verdict.

Inspect `producer_queue` (registered/continuous/visited), bounded
`producer_queue_fallback` reasons, native binding inspection, dynamic capture,
whole-frame mean/median/p95/p99 and memory. Do not infer total gains from
inclusive/overlapping stage sums. Movement, enable/disable, harvest, controller
changes, cell exit/return and representative mod/save behavior must remain right.
