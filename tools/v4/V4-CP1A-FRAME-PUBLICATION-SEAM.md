# V4.0 CP1A — Frame Publication / Epoch Seam

Status: **DESIGN LOCK FOR CP1A IMPLEMENTATION**  
Design lineage: `v4.0-cp1-architecture-design`  
Source authority audited: current accepted CP0A lineage at `8b5f989480903c4aeef028bd95177f44fddd00b8`

This note fixes the first exact main-thread publication boundary for `RenderWorldUpdateBatch` and `FrameRenderState`, and the destructive-reset ownership rule for `worldEpoch`.

## 1. Current frame ordering

`OMW::Engine::frame` currently executes the main game-side stages in this order:

1. finish prior Lua GC;
2. input;
3. sound/window visibility;
4. synchronized Lua update;
5. state-manager update;
6. local/global scripts and world time/recharge;
7. mechanics;
8. physics;
9. world update;
10. GUI update;
11. frame-tail pre-viewer work (`UnrefQueue::flush`, resource/stat reporting, stereo settings);
12. OSG `eventTraversal()`;
13. OSG `updateTraversal()`;
14. focus-object refresh / Lua worker scheduling;
15. OSG `renderingTraversals()`;
16. Lua wait/GC tail.

Object/cell semantic changes can originate before `mWorld->update()` (for example state loading, Lua/script actions, mechanics) as well as during world/scene update. Therefore publishing directly inside `World::update` is too early to define the general V4 frame boundary.

## 2. Locked CP1A publication point

The first neutral publication point is:

> **after the game/state/script/mechanics/physics/world/GUI update block has completed, and before `UnrefQueue::flush`, OSG `eventTraversal`, OSG `updateTraversal`, or any rendering traversal.**

At this point the single main-thread producer has observed all ordinary game-side semantic mutations for the frame, while backend traversal and backend-residency cleanup have not yet begun.

Initial order at that seam:

1. seal the current producer operation list;
2. publish exactly the next `RenderWorldUpdateBatch` into `RenderWorld`;
3. obtain the resulting immutable `RenderWorldReadView` / revision;
4. sample and seal `FrameRenderState` against that published revision;
5. allow the legacy OSG backend to continue its existing event/update/render traversals.

For CP1A's first narrow static-object slice, the OSG backend continues to receive the same semantic payload through the legacy adapter at the existing object hooks. The neutral publication step does **not** replace OSG traversal yet.

## 3. Why publication is before `UnrefQueue::flush`

`UnrefQueue` is backend/resource lifetime cleanup. RenderWorld logical lifetime must not be defined by when an OSG reference is released or cached. Publishing logical creates/destroys first makes the lifetime split explicit:

- game/resource content lifetime;
- RenderWorld logical lifetime;
- backend OSG/VSG/Vulkan residency/cache lifetime.

This also keeps exterior hibernation semantics correct: an active logical instance may be destroyed while a legacy OSG node remains cached for reuse.

## 4. OSG update traversal is not a common semantic boundary

Do not move the renderer-neutral boundary behind `mViewer->updateTraversal()` merely because legacy OSG animation/controller callbacks currently run there.

For CP1A statics this is irrelevant. When actors, NIF controllers, pose, morph, or other animated state are migrated later, their **semantic producer/evaluation step** must be made explicit before FrameRenderState sealing. A future V4 backend must not require an OSG update traversal to define neutral scene state.

Thus:

- OSG event/update traversal remains a legacy backend stage;
- neutral RenderWorld publication remains backend-independent;
- later animation migration moves the required semantic result to the producer/publish side instead of leaking OSG traversal into RenderCore.

## 5. FrameRenderState seal

The first `FrameRenderState` seal belongs at the same pre-viewer seam, immediately after RenderWorld batch publication.

The state is immutable for the render/record phase and includes at minimum:

- frame id;
- current/previous camera transforms;
- explicit view list and semantic view kinds;
- output/render extents;
- jitter/history state;
- current/previous instance/pose/morph data as those systems migrate;
- the published RenderWorld epoch/revision it references.

No fixed OSG camera/view-mask ABI is allowed in the common object.

## 6. Destructive reset / worldEpoch

`MWRender::RenderingManager::clear()` remains the first reset integration point to audit because it is explicitly savegame-specific/destructive. Ordinary exterior cell unload, interior/exterior transition, or `notifyWorldSpaceChanged()` **must not** advance `worldEpoch`.

When a destructive reset occurs:

1. invalidate/cancel any pending reservations and unsealed operations from the old epoch;
2. retire the old RenderWorld logical state;
3. advance `worldEpoch` once;
4. reset expected batch sequence to `1` for the new epoch;
5. perform/continue the existing legacy clear path;
6. allow any objects created later in the same engine frame to reserve identities in the new epoch and enter a new pending batch.

The frame boundary then publishes only the current epoch's batch. Old-epoch work fails closed.

## 7. Batch sequencing

For each epoch:

- first accepted batch sequence is `1`;
- each accepted batch must be exactly `last + 1`;
- duplicate/older batches reject;
- a future/holey sequence rejects;
- publisher is not a reorder buffer;
- sequence advances only after successful deterministic application.

If a batch is rejected before application, any still-reserved create handles owned by that batch are cancelled. Cancellation advances the handle generation so a failed reservation cannot later alias a valid logical object.

## 8. Exception behavior

`Engine::frame` currently catches ordinary frame exceptions and proceeds into the viewer tail. CP1 must not silently publish a second, independently-derived representation after an exception.

The one-semantic-write rule remains authoritative:

- neutral payload is resolved once;
- RenderWorld producer and legacy adapter consume that payload;
- an operation is appended only when that semantic operation has actually succeeded;
- already-successful operations may still be published after an unrelated later frame exception;
- a destructive reset supersedes all pending old-epoch operations.

## 9. Threading rule

CP1A publisher/builder is main-thread, single-owner. Worker production may be added later only under the validated V3.24/V3.25 rules:

- immutable/pre-resolved worker input;
- worker-owned unpublished results;
- bounded immediate workers/caller fallback for frame-critical work;
- deterministic main-thread publication;
- no background FIFO plus same-frame wait.

No worker may mutate RenderWorld or advance sequence/epoch directly.

## 10. Implementation consequence

The first source integration should expose a narrow neutral owner adjacent to `RenderingManager`, but the **frame publication call itself belongs at the engine pre-viewer seam**, not inside `MWRender::Objects` and not inside OSG traversal.

The initial CP1A path is therefore:

`game/scene semantic hook -> MWRender producer bridge -> owned RenderCore payload + reserved handle -> pending ordered batch`

then at the engine seam:

`seal batch -> RenderWorld publish -> seal FrameRenderState -> legacy OSG viewer tail`

This supersedes any earlier CP1 working note that left the exact frame boundary unspecified.
