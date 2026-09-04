# V4.0 CP1A — Architecture Audit and Source-Seam Plan

Status: **DESIGN EXECUTION STARTED — ENGINE SOURCE STILL GATED BY CP0 VISUAL CORPUS**  
Design branch: `v4.0-cp1-architecture-design`  
Design base: `8b5f989480903c4aeef028bd95177f44fddd00b8`  
Authoritative contract: `tools/v4/V4-CP1-NEUTRAL-RENDER-CONTRACT.md`

## 1. Audit verdict

The locked CP1 contract is directionally correct and remains the authority. No backend or roadmap redesign is required.

This audit adds implementation constraints that were implicit or underspecified in the handoff. They are intended to prevent parity drift, stale-handle aliasing, nondeterministic publication, accidental backend coupling, and an oversized first migration patch.

The CP0 gate is unchanged: **do not land CP1 engine-source behavior until the CP0 visual reference corpus is frozen and accepted**. Architecture/design, source-seam auditing, dependency planning, and test planning may proceed now.

## 2. Authority/state correction

The structured archive closed CP0A at `10ab6e75230c25efa2faeef43ad2487f55b6dcaf`, but the authoritative GitHub branch advanced five commits after that point for CP0 visual-corpus tooling only.

Current design base: `8b5f989480903c4aeef028bd95177f44fddd00b8`.

The post-closeout delta is limited to CP0 tooling/reference material (`.github/workflows/v4-cp0-tool-qc.yml`, native-first-person reference launcher, visual-corpus plan, and visual-capture PowerShell helper). It does not change the CP1 engine ownership contract.

## 3. Audit refinements that are now implementation rules

### 3.1 One semantic write, not dual independent writes

The migration must not implement a neutral RenderWorld write and a separately coded OSG write for the same migrated lifecycle operation.

For each migrated operation:

1. producer resolves gameplay/MWWorld state into one backend-neutral semantic operation payload;
2. RenderWorld consumes that payload;
3. the legacy OSG adapter consumes the same payload and performs the current OSG mutation;
4. unmigrated operations remain explicitly legacy-only until converted.

This is the core parity rule for CP1A. It keeps semantic divergence observable and prevents two implementations from silently drifting.

### 3.2 Read-view lifetime is deliberately simple in CP1A

CP1A uses a frame-boundary publication model rather than introducing RCU/COW complexity prematurely.

`RenderWorldReadView` is immutable and valid only across the render/record phase for the published revision it captures. No structural RenderWorld mutation may occur while that frame's read view is active. New batches publish at the next defined boundary before the next `FrameRenderState` is sealed.

A future backend may replace the storage mechanism with COW/RCU without changing the semantic contract.

### 3.3 Generation wrap must never resurrect stale identity

Handle generation `0` remains invalid. Reuse increments generation.

If a reusable slot's 32-bit generation would wrap to `0` or otherwise alias a previously issued generation, the slot is permanently retired/tombstoned instead of being reused. Stale handles therefore fail closed even at wraparound.

### 3.4 Batch sequencing must detect holes

`worldEpoch` changes on destructive world reset. Within one epoch:

- first accepted batch sequence is `1`;
- accepted sequence must be exactly `lastAccepted + 1`;
- duplicate/older sequence rejects;
- future sequence with a hole rejects rather than silently reordering;
- epoch change resets sequence expectation.

If later producers require out-of-order construction, they must merge into an ordered batch before publication. The RenderWorld publisher does not become a reorder buffer in CP1A.

### 3.5 Sealed batches own their payload storage

A sealed `RenderWorldUpdateBatch` may not contain raw pointers, `span`, `string_view`, or other references into producer-owned mutable/transient storage unless the referenced object has an explicitly shared immutable lifetime extending through publication.

Default rule: the batch owns its operation payloads and variable-length data.

### 3.6 Deterministic allocation is part of correctness

Logical slot assignment and reuse must depend only on ordered semantic operations and a deterministic free-slot policy. It must not depend on pointer values, hash-table iteration order, worker completion order, or OSG node identity.

The exact free-list container is implementation-private, but identical ordered input must produce identical handle allocation in tests.

### 3.7 CP1 descriptors are semantic, not frozen GPU ABI

Mesh/material/texture/skeleton/instance/chunk/light descriptors describe logical renderer semantics. CP1 must not freeze Vulkan descriptor layouts, indirect-command slots, bindless indices, GPU vertex packing, or VSG object layout into the common API.

Backend packing remains derived state.

### 3.8 No frame-critical FIFO-then-wait regression

Any later parallel producer work feeding batches inherits the validated V3.24/V3.25 rule: immutable/pre-resolved input, worker-owned unpublished result, bounded immediate execution/reserved capacity/caller fallback as appropriate, deterministic publish, and no submission to an unrelated background FIFO followed by same-frame waiting.

CP1A itself should remain single-publisher and correctness-first.

## 4. First source seam

### 4.1 What not to use as the common boundary

`MWRender::Objects` is OSG-owned and MWWorld-pointer-owned today:

- object identity keys use `MWWorld::LiveCellRefBase*`;
- cell identity keys use `MWWorld::CellStore*`;
- stored render objects are `osg::ref_ptr<Animation>` / OSG nodes;
- insertion builds OSG transforms and writes the base node directly into `RefData`.

Therefore `Objects` must remain a legacy backend consumer during CP1A rather than becoming RenderWorld itself.

### 4.2 Initial producer boundary

The first vertical slice should begin at the existing `RenderingManager` object lifecycle surface, where the engine already expresses semantic operations such as:

- insert/create object;
- remove object;
- move object;
- rotate object;
- scale object;
- move/update object between cells.

The current transform methods are thin OSG mutations, making them suitable low-risk parity probes before camera/environment/actors/paging are migrated.

### 4.3 Initial slice scope

First migrated semantic slice after the CP0 visual gate opens:

1. static/non-actor object logical identity;
2. create/destroy;
3. transform create/update;
4. containing-chunk/cell relationship needed by RenderWorld;
5. minimal mesh/material logical references sufficient to prove stable resource identity without implementing a new loader;
6. Legacy OSG adapter consumes the same operations and preserves current output.

Explicitly defer actor skeleton/pose, equipment, FBFP, water, sky, postprocessing, terrain/object-paging bulk ingestion, and backend residency until the foundation has passed unit and parity gates.

## 5. Foundation implementation layout after CP0 visual acceptance

Preferred common module:

`components/rendercore/`

Initial files:

- `handles.hpp`
- `math.hpp`
- `resources.hpp`
- `renderworld.hpp/.cpp`
- `updatebatch.hpp/.cpp`
- `framerenderstate.hpp`
- `renderer.hpp`

Build integration: add `rendercore` through the normal `components/CMakeLists.txt` component mechanism.

Unit tests belong in `apps/components_tests/rendercore/` and are added to `apps/components_tests/CMakeLists.txt`.

## 6. GLM dependency gate

The current audited root/extern build setup does not contain an in-tree GLM external or an explicit GLM acquisition step. CP1 must not rely on accidental transitive availability from another dependency or donor checkout.

Before `math.hpp` lands, choose and lock one reproducible GLM dependency path compatible with the existing Windows CI/dependency model. Dependency introduction must be independently buildable before rendercore math types depend on it.

This is a **build-system prerequisite**, not a reason to replace the targeted-GLM renderer-envelope decision.

## 7. Implementation increments

These are implementation increments inside the existing CP1 checkpoint; they do not change the locked V4 roadmap.

### Increment A — foundation types

- reproducible GLM dependency;
- typed slot32/generation32 handles;
- epoch/revision types;
- handle validity, equality, hashing only if deterministic semantics are preserved;
- generation-wrap retirement tests;
- header-purity test: no OSG/VSG/Vulkan/MWWorld dependencies in common rendercore headers.

### Increment B — persistent logical tables

- RenderWorld resource/object tables;
- deterministic allocation/release;
- logical lifetime vs resource revision;
- stale generation/revision rejection;
- world reset/epoch invalidation.

### Increment C — immutable update batches

- owned payload storage;
- exact-next batch sequencing;
- deterministic single-owner publication;
- stale/wrong-epoch rejection;
- read-view lifetime contract.

### Increment D — frame state

- immutable/versioned `FrameRenderState`;
- explicit variable `FrameView` list;
- current/previous camera state;
- render/output extents;
- jitter/history identity/reset semantics;
- current/previous transform slots reserved for temporal work;
- FBFP owner visibility expressed per view, not by deleting logical objects.

### Increment E — semantic renderer service

- narrow semantic service only;
- apply/publish/read/render lifecycle;
- no giant RenderDevice;
- no backend types in common boundary.

### Increment F — first legacy OSG vertical slice

- object lifecycle producer creates neutral operation payload;
- RenderWorld applies it;
- Legacy OSG adapter applies the same payload;
- existing OpenGL output remains authoritative;
- compare lifecycle/transform behavior against frozen CP0 visual/reference semantics.

Only after Increment F is clean should the seam expand to camera/environment, actors/FBFP, and paging.

## 8. Required unit gates before broad adaptation

At minimum:

1. default handle invalid;
2. generation 0 never issued;
3. stale handle rejected after release/reuse;
4. generation wrap retires slot;
5. identical ordered inputs allocate deterministically;
6. wrong world epoch rejected;
7. duplicate/older/holey batch sequence rejected;
8. stale resource revision rejected where revision-conditioned update is required;
9. batch payload remains valid after producer temporaries are destroyed;
10. read view is immutable for its lifetime;
11. destructive reset invalidates old handles/read state;
12. FrameRenderState current/previous state and history reset are deterministic;
13. arbitrary view counts and view kinds are supported without fixed bitmask ABI;
14. FBFP owner-hidden/main-view but shadow-visible policy is representable without logical object removal;
15. no common rendercore header includes OSG, VSG, Vulkan, or MWWorld types.

## 9. CP1A closure criteria

CP1A does not close merely because rendercore compiles.

Closure requires:

- CP0 visual corpus frozen before first engine-source behavior patch;
- foundation unit gates pass;
- first semantic object lifecycle vertical slice uses one semantic payload for RenderWorld + OSG adapter;
- no new gameplay/Lua/save/animation semantics;
- OpenGL visual/behavior parity passes the relevant frozen CP0 cases;
- no VSG/Vulkan dependency enters the common boundary;
- no performance promotion claim is made from CP1 correctness work;
- V3.25 frozen numerical cohort remains the performance control for later comparable A/B work.

## 10. Immediate next action

While CP0 visual capture is still pending, finish source-seam inventory and dependency/test scaffolding design only.

As soon as the CP0 visual corpus is accepted, create the actual CP1 implementation branch from the **then-current accepted CP0 head**, not from this design branch. Reapply/cherry-pick this design record as needed, then begin Increment A.
