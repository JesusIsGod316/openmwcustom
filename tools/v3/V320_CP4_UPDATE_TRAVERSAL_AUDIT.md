# V3.20 CP4 object-update and traversal audit

## Disposition

CP4 does not add broad object-update suppression or a new scene traversal bypass.
Both candidate lanes are rejected for the initial V3.20 build because the effective
P0 source does not expose one authoritative generation that covers every semantic
producer required to prove a skip safe.

## Audited ownership

- `MWWorld::World::update()` coordinates weather, navigator, player, world scene,
  renderer, sound listener, spell preload, and post-load navigation work. It is not
  an object-only loop and cannot be cadence-thinned as a unit.
- Object transforms, scale, rotation, pointer replacement, lifecycle, physics,
  navigation, Lua, animation, AI, and cell activation enter through separate paths
  in `worldimp.cpp`, `scene.cpp`, `renderingmanager.cpp`, and `objects.cpp`.
- OSG update/cull traversal also runs node callbacks, controllers, morph/rig
  updates, light management, particles, water, shadows, and third-party/mod-added
  callbacks. Node masks and the existing cull/occlusion systems are the available
  authoritative visibility contracts; there is no engine-owned global “unchanged
  subtree” generation.
- Existing specialized implementations already own safe local checks, including
  morph dirty state, rig/skeleton traversal numbers and active state, node masks,
  LOD/cull masks, and hierarchical occlusion. A second broad skip would sit above
  those contracts and could prevent them from observing their own invalidation.

## Rejected approximations

- Distance-only update cadence for arbitrary objects.
- A single player/camera dirty bit as a proxy for world, Lua, animation, particle,
  audio, or streaming state.
- Skipping `World::update()`, viewer update traversal, or scene-root traversal on a
  fixed cadence.
- Persistent “unchanged object” or “invisible subtree” conclusions without every
  producer participating in invalidation.
- Reusing P1/P1b material, shader, shadow, or StateSet equivalence as a traversal
  grouping signal.

## Revisit gate

Revisit only after a narrower owner supplies an explicit generation contract. A
candidate must enumerate all writers, invalidate on lifecycle and cell changes,
retain a bounded forced-update interval, and demonstrate that input, player,
listener, playback, streaming, animation, Lua, physics, navigation, particles,
water, lighting, and shadow semantics remain normal. The first suitable target
should be a self-contained subsystem, not the whole world or scene root.

## Checkpoint result

The safe V3.20 tree remains CP1 focus refinement + CP2 engine/Lua and conversion
fast paths + CP3 same-frame sound-query coalescing. CP4 changes no generated
runtime source and preserves exact P0 fallback. This rejection is intentional
engineering progress, not an unimplemented enabled feature.
