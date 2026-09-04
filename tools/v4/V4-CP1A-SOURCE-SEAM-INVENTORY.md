# V4.0 CP1A — Source-Seam Inventory

Status: **DESIGN ONLY — SUPERSEDES THE SIMPLIFIED FIRST-SEAM DESCRIPTION IN `V4-CP1A-ARCHITECTURE-AUDIT.md` WHERE THIS FILE IS MORE SPECIFIC**  
Branch: `v4.0-cp1-architecture-design`

## 1. Source finding: there is no single object-lifecycle entry point today

The current source splits object render lifecycle responsibility across `MWWorld::Scene`, class-specific rendering dispatch, `MWRender::Objects`, and `MWRender::RenderingManager`.

### Creation path

`MWWorld::Scene::addObject(...)` currently:

1. resolves the normalized model path;
2. derives object rotation from gameplay position/rotation semantics;
3. decides whether the reference is represented by object paging;
4. calls `ptr.getClass().insertObjectRendering(ptr, model, rendering)` for non-paged references;
5. applies rotation through `RenderingManager`;
6. separately attaches mechanics, particles, physics, Lua, and navigator behavior.

Class-specific `insertObjectRendering` eventually routes into OSG-owned systems such as `MWRender::Objects::insertModel`, `insertNPC`, or `insertCreature`.

### Mutation/removal path

`RenderingManager` directly owns thin render mutations for:

- `rotateObject` -> OSG attitude;
- `moveObject` -> OSG position;
- `scaleObject` -> OSG scale;
- `updatePtr` -> object/cell ownership update;
- `removeObject` -> object/path/water cleanup.

Therefore CP1A must not pretend that `RenderingManager` is already a complete semantic creation boundary.

## 2. Correct first producer architecture

Introduce a **legacy producer/identity bridge** adjacent to `RenderingManager`, but keep it outside `components/rendercore`.

Working responsibility split:

`MWWorld/legacy renderer semantics -> MWRender producer bridge -> RenderCore semantic payload -> RenderWorld`

and, for fields already migrated:

`same RenderCore semantic payload -> legacy OSG adapter -> existing OSG structures`

The producer bridge may understand `MWWorld::Ptr`, VFS model paths, current OSG-facing source conventions, and cell identity. `components/rendercore` may understand none of those types.

Suggested implementation name is deliberately non-authoritative: `RenderWorldProducer` or `LegacyRenderWorldBridge`. Do not freeze the name until the first patch.

## 3. Producer-only identity maps

The legacy producer layer needs mappings from current game identity to neutral handles.

Allowed only in the producer/adapter layer:

- `MWWorld::LiveCellRefBase* -> InstanceHandle` lookup;
- `MWWorld::CellStore* -> ChunkHandle` lookup;
- normalized source asset key/path -> `MeshHandle` lookup.

Rules:

- raw pointers never cross into RenderCore descriptors, handles, batches, FrameRenderState, or renderer service APIs;
- pointer values never determine handle slot selection, publication order, or any serialized/debug identity;
- maps are lookup aids only;
- handle allocation is still driven by ordered semantic operations and RenderWorld's deterministic allocator;
- deletion removes the producer mapping before a stale game-side reference can target a reused handle.

This satisfies the locked rule that game identity belongs in the producer layer while neutral renderer identity is generation-safe.

## 4. First vertical slice eligibility

Start narrower than "all static objects".

Initial proof population should be active, individually rendered, non-player objects that are:

- not actor/NPC/creature;
- not object-paged;
- not using the animation path;
- not doors;
- not lights;
- not special effect/particle-only records.

This gives clutter/statics-like object lifecycle and transform coverage without pulling actor pose, inventory equipment, FBFP, light extraction, animation, or ObjectPaging ownership into the first patch.

Everything outside this predicate remains legacy-only and must have zero RenderWorld-side behavioral effect until its own migration slice.

## 5. Creation interception point

For the first slice, intercept creation in `MWWorld::Scene::addObject(...)` **after** all of these are known:

- reference is enabled/not deleted;
- normalized model path;
- current rotation semantics;
- current paging decision;
- current cell;

and **before or together with** the class-specific render insertion for eligible non-paged objects.

The producer creates one semantic object-create payload containing at least:

- producer-resolved logical source identity token only long enough to update the producer map (not stored in RenderCore);
- `ChunkHandle`;
- `MeshHandle`;
- `InstanceHandle` allocation request/result;
- translation;
- rotation;
- scale;
- semantic visibility/category flags sufficient for the slice;
- initial resource revision references.

The legacy OSG path must consume the same migrated transform/source values rather than independently re-deriving them after the field is declared migrated.

## 6. Mesh/material scope for the first slice

Do not derive neutral materials from live OSG `StateSet` objects. That would recreate the rejected OSG-capture architecture.

For the first slice:

- mesh logical source identity may be the normalized VFS model path in a producer-created semantic mesh-source descriptor;
- `MeshHandle` proves stable logical resource identity;
- actual NIF parsing/material extraction remains on the legacy path;
- MaterialHandle population may remain empty/deferred for the first lifecycle proof if the object has no neutral material representation yet.

Material records should be populated later from source-content semantics during the NIF/material adaptation slice, not by interrogating already-built OSG state.

## 7. Transform migration rule

Current source derives rotation semantics before `RenderingManager::rotateObject`, while position/scale mutations arrive through `RenderingManager`.

The first slice should create a neutral transform representation and make it authoritative for migrated instances.

Parity-safe transition rule:

1. preserve the existing OpenMW rotation order/sign semantics exactly;
2. convert once into the neutral transform representation at the producer seam;
3. update RenderWorld from that transform payload;
4. legacy OSG adapter converts that same payload to OSG position/attitude/scale;
5. no second gameplay-semantic rotation calculation is allowed in the adapter.

The first implementation must unit-test representative object rotations against the existing OSG result before broadening the population.

## 8. Cell/chunk migration rule

For first-slice individually rendered objects, active cell membership becomes the producer-side source for `ChunkHandle` association.

`RenderingManager::updatePtr(old, updated)` is the natural existing mutation hook for preserving `InstanceHandle` while changing the instance's `ChunkHandle` association.

Chunk identity is logical RenderWorld identity, not `CellStore*` identity. The pointer remains only the producer lookup key.

## 9. Object paging is a separate producer lane

The current `Scene::addObject` path explicitly substitutes a sentinel base node for references represented by ObjectPaging. Those references must **not** also be emitted as first-slice individual RenderWorld instances.

Later CP1 expansion should give `ObjectPaging` its own neutral producer path for chunk/static data. That is the natural bridge toward Clockwork-style persistent flat static GPU-scene records.

Until that lane is implemented:

- paged ref -> legacy paging only;
- non-paged eligible ref -> first-slice neutral individual instance + legacy OSG;
- never both.

## 10. Hibernation validates the three-lifetime model

V3.x exterior hibernation retains selected OSG nodes/animations across cell unload/reload. CP1 must not reinterpret that backend cache as logical RenderWorld lifetime.

For neutral state:

- active logical instance is destroyed when its active cell unloads;
- producer mapping is removed;
- legacy OSG may independently retain backend objects in its hibernation cache;
- on restore/reactivation, neutral logical identity is recreated through the active producer path;
- stale old `InstanceHandle` remains invalid even if the legacy backend happened to reuse an OSG node.

This cleanly separates:

1. content/source lifetime;
2. logical RenderWorld lifetime;
3. backend residency/cache lifetime.

## 11. World epoch/reset owner

Do **not** increment `worldEpoch` from `RenderingManager::notifyWorldSpaceChanged()`.

That method currently clears effects/ripples and can occur as active cells are transitioned; it is not proof of a destructive renderer-world reset.

The explicit `RenderingManager::clear()` / savegame-specific reset path is the appropriate initial integration point to audit for `RenderWorld::reset()` and epoch increment, after normal active-object destruction has been published.

Ordinary cell unload, exterior/interior transition, or worldspace change keeps the same `worldEpoch`; those operations use ordered instance/chunk create/destroy/update batches.

## 12. Removal semantics

For eligible first-slice objects, `RenderingManager::removeObject(ptr)` becomes a migrated lifecycle hook:

1. resolve `InstanceHandle` in the producer map;
2. emit neutral destroy operation;
3. remove producer mapping;
4. apply same lifecycle event to the legacy OSG adapter/existing removal path;
5. any later update carrying the retired generation fails closed in RenderWorld.

If an object is outside the migrated predicate or has no producer mapping, preserve the exact legacy path with no synthetic neutral mutation.

## 13. First-slice batch boundaries

Do not publish one batch per object mutation if the caller is already operating on a cell insertion/removal sequence.

Initial batching policy:

- collect semantic operations during the existing main-thread scene update/load/unload operation;
- seal one ordered batch at a defined render-world publication boundary;
- apply exact-next sequence validation;
- publish before the next `FrameRenderState` is sealed;
- preserve operation order inside the batch;
- no worker publication in CP1A foundation.

For standalone move/rotate/scale calls during normal simulation, append operations to the current producer batch and publish at the same frame boundary rather than mutating RenderWorld immediately mid-read.

## 14. Immediate code tasks once CP0 visual acceptance opens the gate

1. introduce reproducible GLM dependency;
2. add RenderCore handles/epoch/revision/math foundation + unit tests;
3. add deterministic RenderWorld tables and update-batch publisher + tests;
4. add a producer bridge owned near the legacy renderer, with game-identity lookup maps but no raw identity crossing the boundary;
5. integrate reset/epoch lifecycle;
6. integrate narrow eligible object creation in `Scene::addObject`;
7. integrate move/rotate/scale/updatePtr/remove hooks;
8. make the legacy OSG transform path consume the neutral migrated transform payload;
9. run unit gates;
10. run the relevant frozen CP0 visual/behavior parity cases before expanding the population.

No VSG, Vulkan, SDL3, actor pose, paging conversion, material extraction, or performance claim belongs in this first slice.
