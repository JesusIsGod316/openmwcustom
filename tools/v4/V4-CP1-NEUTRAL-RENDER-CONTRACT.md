# V4.0 CP1 — Neutral Render Contract

Status: **LOCKED IMPLEMENTATION HANDOFF — IMPLEMENTATION NOT STARTED**  
Produced by: V4.0 CP0A donor/source convergence audit  
Implementation gate: **CP0 visual reference corpus must be frozen before CP1 source implementation begins**

This is the contract CP1A must implement while the final materialized V3.25 OSG/OpenGL renderer remains the only rendering backend. It exists to prevent the Vulkan port from inheriting OSG ownership and to prevent the later Clockwork-derived GPU scene / temporal / DLSS work from requiring another game-to-render ownership rewrite.

The common boundary is:

`game/mod semantics -> ordered immutable RenderWorld update batches -> persistent RenderWorld -> immutable FrameRenderState -> Renderer -> backend`

The common boundary must contain **no OSG or VSG types**.

## 1. Scope and non-goals

CP1A does:

- add backend-neutral GLM-facing core types;
- add generation-safe logical handles;
- add persistent logical `RenderWorld` records/lifetimes;
- add ordered immutable update batches;
- add immutable/versioned frame state and multi-view descriptors;
- define the high-level renderer service contract;
- add a legacy OSG adapter path that can consume the new semantic state without changing rendered output;
- establish deterministic publish and stale-work rules.

CP1A does **not**:

- add Vulkan/VSG;
- change the active renderer;
- port NIF geometry to VSG;
- replace the current OSG scenegraph;
- implement GPU-driven culling/indirect draws;
- change postprocess visuals;
- implement DLSS;
- perform a whole-repository GLM migration;
- change gameplay, Lua, animation event or save semantics.

CP1B then ports SDL3 while OpenGL remains active. CP2 is the first Vulkan bring-up checkpoint.

## 2. Module boundary

Recommended neutral module root: `components/rendercore/`.

Expected first files:

```text
components/rendercore/handles.hpp
components/rendercore/math.hpp
components/rendercore/resources.hpp
components/rendercore/renderworld.hpp
components/rendercore/updatebatch.hpp
components/rendercore/framerenderstate.hpp
components/rendercore/renderer.hpp
```

Exact filenames may be adjusted during implementation, but the dependency rule is locked:

- `rendercore` may depend on standard C++, GLM and engine-semantic value types that are not backend objects;
- `rendercore` may not include `<osg/...>`, `<osgViewer/...>`, `<osgUtil/...>`, `<vsg/...>` or Vulkan headers;
- legacy OSG/Vulkan adapters depend **on** `rendercore`, never the reverse.

## 3. Logical handles

All persistent renderer identities use typed handles with slot + generation.

Conceptual representation:

```cpp
template <class Tag>
struct Handle
{
    std::uint32_t slot = UINT32_MAX;
    std::uint32_t generation = 0;
};
```

Required distinct handle types:

- `MeshHandle`
- `MaterialHandle`
- `TextureHandle`
- `SkeletonHandle`
- `InstanceHandle`
- `ChunkHandle`
- `LightHandle`

Rules:

- `slot == UINT32_MAX` is invalid;
- generation `0` is invalid/reserved;
- first live generation is `1`;
- slot reuse increments generation before the new object becomes visible;
- stale handle access fails closed and never aliases a replacement;
- different handle families are not implicitly convertible;
- handles are value types and may be copied into immutable job inputs;
- raw pointers, `MWWorld::Ptr`, `LiveCellRefBase*`, OSG object addresses, VSG object IDs and pointer hashes are not common-renderer identities.

A backend may maintain a private mapping from a logical handle to backend objects/descriptors/buffer offsets.

## 4. Generation vs resource revision

Handle generation and content revision have different meanings and must not be conflated.

**Generation** changes when a slot is retired and later reused for a different logical object.

**Revision** changes when the same live logical resource keeps its handle but its payload changes.

Conceptual fields:

```text
Handle { slot, generation }
ResourceRevision = uint32/uint64 monotonic per live resource
```

Examples:

- texture contents are reloaded: same `TextureHandle`, higher revision;
- material animated parameters change: same `MaterialHandle`, higher revision or frame-local dynamic data depending on the source;
- streamed cell chunk is replaced: old `ChunkHandle` retires; replacement has a new generation;
- slot reused after retirement: generation must differ even if source path happens to be identical.

Asynchronous work carries both the relevant handle generation and payload revision and is rejected if either is stale for the operation being published.

## 5. World epoch and ordered publication

`RenderWorld` has a 64-bit `worldEpoch`.

The epoch increments on destructive semantic resets such as full `clear()` / worldspace teardown where pending work from the prior world must be invalidated as a class.

Every immutable `RenderWorldUpdateBatch` contains:

- `worldEpoch`;
- monotonic 64-bit `sequence`;
- zero or more typed operations;
- optional diagnostic source/frame identity.

Rules:

- a batch is immutable after it is sealed;
- the initial CP1 implementation has one deterministic publish owner;
- batches for the active epoch are applied in sequence order;
- wrong-epoch batches are dropped as stale;
- duplicate or older sequence publication is rejected;
- worker threads may prepare operation payloads but do not mutate `RenderWorld` directly;
- publish ordering must preserve existing V3.25 gameplay/render semantics.

This is the generalization of the V3.24/V3.25 worker-owned-unpublished-result rule.

## 6. Neutral math contract

Renderer-facing new code uses GLM, but broad engine conversion is deferred.

Core conventions:

- world/camera translation: `glm::dvec3` at the neutral boundary;
- local/object scale: `glm::vec3`;
- rotation: explicit `glm::quat`/`glm::dquat` as appropriate;
- colors: `glm::vec4`;
- GPU-facing matrices may be float after camera-relative/backend conversion;
- no implicit default quaternion construction: identity is explicitly initialized;
- conversion from current OSG transforms must preserve the existing OpenMW axis/sign/order conventions exactly; CP1 is not a coordinate-system rewrite.

Conceptual transform:

```cpp
struct WorldTransform
{
    glm::dvec3 translation{0.0};
    glm::quat rotation{1.f, 0.f, 0.f, 0.f};
    glm::vec3 scale{1.f};
};
```

The backend may camera-rebase and pack to a 3x4 float transform. That packing is not part of the common ABI.

## 7. Persistent RenderWorld ownership

`RenderWorld` owns logical renderer state, not backend allocations.

Minimum logical tables:

- meshes;
- materials;
- textures;
- skeletons;
- instances;
- chunks;
- lights.

Each slot tracks at minimum:

- generation;
- live/retired state;
- revision where applicable;
- backend-neutral semantic descriptor/payload reference;
- enough source identity/debug metadata to diagnose mismatches without making source pointers part of identity.

`RenderWorld` does not own:

- `osg::Node`/`StateSet`/Viewer/Camera;
- `vsg::Node`/Viewer;
- Vulkan image/buffer/descriptor/pipeline objects;
- current gameplay `MWWorld::Ptr` objects.

Mappings from game object identity to logical handles live in the game/legacy producer layer.

## 8. Three lifetime domains

V4 permanently separates:

1. **content/source lifetime** — VFS/NIF/image/animation CPU source data;
2. **logical RenderWorld lifetime** — stable handles + logical semantic records;
3. **backend residency lifetime** — OSG/VSG/Vulkan allocations and GPU memory.

Consequences:

- backend eviction does not retire a logical handle;
- a logically live resource may be temporarily non-resident;
- backend residency can be reconstructed from logical/source data;
- backend device/swapchain recreation does not require game-object recreation;
- GPU deletion is fence/frame-lifetime delayed independently of logical retirement.

## 9. Resource records

CP1 reserves logical resource identities. CP3 may refine the exact neutral vertex/skin payload format, so CP1 must not freeze a Vulkan or OSG vertex ABI prematurely.

### Mesh

Minimum semantic descriptor:

- bounds;
- surface/material-slot count;
- topology;
- static vs skinned/morphed capability;
- optional `SkeletonHandle` compatibility metadata;
- logical geometry payload reference with no OSG/VSG type.

### Texture

Minimum semantic descriptor:

- image/source identity;
- dimensions/format class where known;
- color-space/semantic role information;
- sampler semantics separate from backend descriptor allocation.

### Skeleton

Minimum semantic descriptor:

- ordered bone identities/names as required by OpenMW animation semantics;
- parent hierarchy;
- bind/inverse-bind transforms;
- stable bone indexing for pose publication.

Backend-specific VSG skeleton nodes are not stored here.

## 10. Material semantic record

Current V3.25 semantics and the Clockwork material map inventory define the minimum logical surface.

Material scalar/color state must preserve:

- diffuse;
- ambient;
- specular;
- emission;
- shininess;
- emissive multiplier;
- specular strength;
- vertex-color mode;
- environment-map color/strength where applicable;
- alpha value/test threshold;
- blend/ordering semantics;
- cull/two-sided state;
- unlit/emissive behavior;
- relevant texture transforms/UV selection.

Required texture map roles:

- Diffuse
- Dark
- Detail
- Decal
- Emissive
- Normal
- Environment
- Specular
- Bump
- Gloss
- Blend/terrain

Current final V3.25 material/PBR interpretation wins if donor behavior differs.

The logical material does **not** contain:

- bindless GPU handles;
- texture-array IDs/layers;
- descriptor-set IDs;
- VSG pipeline objects;
- Clockwork `Legacy/Array/Bindless` material mode.

Those are backend residency/submission choices.

## 11. Instance record

An independently addressable rendered object uses `InstanceHandle`.

Minimum logical instance state:

- handle;
- optional owning `ChunkHandle`;
- `MeshHandle`;
- one or more `MaterialHandle` surface bindings as required by the mesh;
- optional `SkeletonHandle`;
- current world transform;
- bounds;
- LOD center/range/policy;
- small-feature-culling eligibility;
- semantic visibility/pass flags;
- lighting participation;
- optional parent `InstanceHandle` + attachment/bone binding;
- semantic flags needed for owner/FBFP, effect, projectile, groundcover, terrain-extra and similar categories.

Backend batch/bucket/indirect-command IDs are not stored in the common record.

## 12. Chunk record

A `ChunkHandle` groups streamed/static population whose lifecycle is replaced as a unit.

Minimum common state:

- handle;
- bounds;
- worldspace/cell/producer identity as backend-neutral values;
- ordered or stable membership list of logical instances / compact chunk items;
- semantic visibility/LOD policy for the group;
- revision/generation state.

The exact representation may optimize immutable chunk-local instances, but stale generation safety is mandatory.

Clockwork-derived GPU fields such as first-instance range, batch counts and indirect offsets are backend-derived caches.

## 13. Attachment model

Actor equipment, held weapons, shields, lights, VFX and similar parts must not require backend child nodes at the common boundary.

Logical attachment state contains:

- child `InstanceHandle`;
- parent `InstanceHandle`;
- optional `SkeletonHandle`/bone index or stable bone key;
- local attachment transform;
- visibility/pass semantics;
- material/effect state as normal logical resources.

This supports David-style in-place equipment updates while keeping final V3.25 source/equipment semantics authoritative.

## 14. Light record

Persistent/dynamic lights use `LightHandle`.

Minimum logical point-light state:

- position;
- diffuse;
- specular/ambient where current semantics use them;
- constant/linear/quadratic attenuation;
- effective radius;
- enabled/empty state;
- actor fade where applicable;
- semantic view/pass participation.

Directional sun state belongs in environment/frame state rather than pretending to be a local point-light instance.

The backend may derive clustered grids, light lists, ranges and SSBO/descriptor layouts.

## 15. Update operation vocabulary

CP1's update batch must be expressive enough to represent current renderer-facing mutations without exposing backend objects.

Required operation families:

### Resources

- create mesh / update mesh revision / retire mesh;
- create texture / update texture revision / retire texture;
- create material / update material / retire material;
- create skeleton / update skeleton revision if ever valid / retire skeleton.

### World population

- create instance;
- update instance transform;
- update instance resource/material bindings;
- update instance visibility/pass state;
- update attachment state;
- retire instance;
- create/replace chunk;
- retire chunk.

### Actors/dynamic deformation

- publish actor pose;
- publish morph weights;
- publish equipment/attachment delta;
- publish actor fade/visibility state.

### Lighting/environment

- create/update/retire light;
- update environment/fog/sun/sky state;
- update water state;
- update weather/precipitation state.

Operation payloads are value/immutable data or stable logical handles, never live backend nodes.

## 16. RenderWorld mutation and read model

Initial CP1 rule:

- one publish lane owns `RenderWorld` mutation;
- structural updates are committed at a defined frame boundary before the `FrameRenderState` for that render frame is sealed;
- the renderer/backend receives a read-only world view/version whose lifetime extends through command recording/submission use;
- recording jobs may read that immutable/version-stable view in parallel;
- backend code never reaches mutable `MWWorld`, Lua state, inventory, mechanics or live OSG scene objects through the common path.

Implementation may use generation-stable tables, copy-on-write pages, RCU-style snapshots or another mechanism later. The semantic guarantee is immutable read visibility for the renderer's frame lifetime.

## 17. FrameRenderState

`FrameRenderState` is immutable after seal and has a monotonic 64-bit `frameId`.

Required frame identity:

- `frameId`;
- `worldEpoch`;
- `renderWorldRevision` or equivalent read-view version;
- simulation/reference time;
- frame delta;
- temporal `historyEpoch`/reset identity;
- history-valid flag.

Required render/output state:

- render extent;
- output/present extent;
- render scale represented by the extent relationship, not assumed equal;
- current jitter;
- unjittered projection/camera constants;
- exposure/eye-adaptation semantic values when available;
- current projection offset.

The render/output split is reserved now for NIS/DLSS and must not require a later ownership redesign.

## 18. Camera state and temporal history

For the primary camera and each temporal-capable view, frame state reserves:

- current world position/orientation;
- previous world position/orientation;
- current unjittered view matrix;
- previous unjittered view matrix;
- current projection;
- previous projection;
- current jittered projection or jitter value;
- near/far/FOV/aspect semantics;
- camera-cut/history reset identity.

A teleport, load, backend reset, incompatible resolution change or explicit camera cut can invalidate history by incrementing/resetting the history epoch.

Previous state must be previous **rendered** state, not merely whatever simulation value happened to exist one update earlier.

## 19. Explicit multi-view model

`FrameRenderState` contains a variable-length list of `FrameView` descriptors.

Required `ViewKind` vocabulary begins with:

- Main
- Shadow
- Reflection
- Refraction
- Map
- Preview
- PrecipitationOcclusion
- Debug

It is extensible without changing handle identity.

Each view contains at minimum:

- frame-local `viewIndex`;
- view kind;
- current and previous camera/matrices where applicable;
- viewport/render extent;
- LOD scale;
- small-feature policy;
- visibility/pass include/exclude policy;
- target/history semantic identity;
- flags for shadow/occlusion/temporal participation.

There is **no fixed common maximum number of views** and no common 32-bit view mask ABI. A Vulkan backend may compile the list to efficient masks/tables internally.

## 20. Visibility/pass semantics and FBFP

Common visibility state must express current V3.25 owner-view behavior without deleting logical content or relying on OSG camera callbacks.

The logical scene needs semantic layer/visibility flags sufficient to distinguish at least:

- ordinary world geometry;
- player/owner body;
- owner head/hair/helmet or other scene-camera-hidden owner parts;
- first-person-only/owner-view content where used;
- shadow-caster eligibility;
- reflection/refraction eligibility;
- map/preview eligibility;
- debug/editor/marker classes where relevant.

A `FrameView` applies semantic inclusion/exclusion rules.

Required test case: owner head can be hidden in the main FBFP scene view while remaining available to the appropriate shadow view, matching final V3.25 behavior.

## 21. Dynamic transforms

Frame-local dynamic transform publication is separate from structural resource creation.

`FrameRenderState` carries or references immutable arrays of:

- `InstanceHandle`;
- current transform;
- previous rendered transform;
- history-valid bit where required.

Static population does not need to be redundantly copied every frame; its persistent transform lives in `RenderWorld` until changed.

This distinction is required for efficient motion vectors later.

## 22. Skeleton pose and morph frame state

For each rendered skinned actor that needs dynamic deformation, immutable frame state exposes:

- `InstanceHandle`;
- `SkeletonHandle`;
- current bone palette;
- previous rendered bone palette or valid history reference;
- pose revision/frame identity;
- history-valid bit;
- current morph weights;
- previous morph weights/history where applicable.

Gameplay/animation remains responsible for:

- animation source order;
- active groups;
- priorities/blend masks;
- text-key/event semantics;
- root-motion semantics.

The backend is responsible only for rendering the published deformation state. CPU skinning remains a correctness fallback; GPU skinning is a later optimization.

## 23. Environment state

Frame state must carry renderer-consumed semantic environment values without requiring backend calls into `RenderingManager`/`MWWorld` during recording.

Reserve explicit state for:

- exterior/interior classification;
- ambient color/night-eye factor;
- fog color and current fog parameters;
- sun direction, diffuse/specular color and visibility;
- night/day state;
- sky enabled/moon state as needed;
- water enabled/height/underwater state;
- weather/precipitation state;
- other current V3.25 environment inputs required by parity features.

CP4–CP7 may expand exact fields, but they must extend this value-state model rather than adding mutable world callbacks to the backend.

## 24. Renderer service contract

The high-level common renderer service is semantic, not a low-level GPU device abstraction.

Conceptual operations:

```text
initialize/start backend
apply/publish RenderWorld changes
render(const RenderWorldReadView&, const FrameRenderState&)
resize/present-surface state
screenshot/readback request through neutral result types
shutdown
```

Backend selection is startup/restart scoped. Hot-switching a live renderer is not a CP1/V4.0 requirement.

Low-level API operations such as createBuffer/createPipeline/bindDescriptor are not exposed to game code through this interface.

## 25. Legacy OSG adapter rule

Final materialized V3.25 `RenderingManager` remains the OpenGL correctness control.

CP1 introduces an adapter incrementally:

- existing game calls retain behavior;
- semantic producer code additionally publishes neutral state or is routed through neutral operations;
- OSG adapter applies equivalent mutations to the existing OSG scene;
- no attempt is made to reconstruct the entire current OSG scene from RenderWorld in the first CP1 patch;
- conversion proceeds by controlled surface areas with exact visual/runtime parity.

The legacy adapter may contain OSG types. `rendercore` may not.

## 26. Worker/QoS rules

Any CP1 preparation jobs must follow the proven V3.24/V3.25 architecture rules:

- input is fully resolved before worker dispatch;
- inputs are immutable for worker lifetime;
- output is worker-owned and unpublished;
- no worker mutates live scene/world/backend state;
- publish is deterministic;
- stale generation/revision/epoch is checked before publish;
- frame-critical work is bounded and must not enter a background FIFO that the same frame later waits behind;
- fallback reconstructs the complete result before any partial publish when required for correctness.

## 27. Backend residency contract

Although Vulkan residency starts in CP2, CP1 handle semantics must allow it.

The backend may track for each logical resource:

- resident/non-resident state;
- byte cost;
- last use;
- pinned/evictable state;
- pending upload;
- pending retire fence/frame;
- fallback tier.

Residency transitions do not change logical handle generation.

CP2 must expose category-level VRAM telemetry because 8 GB is a hard project limit.

## 28. Unsupported-content fallback

V4 does not require every asset to use the fastest path immediately.

If a resource/material/actor cannot yet use the optimized Vulkan GPU-scene representation:

- keep semantic identity valid;
- route it through a slower correct backend path;
- record why it fell back;
- never silently omit it;
- never lower visual/material semantics merely to increase fast-path coverage.

This adopts the strongest Clockwork compatibility lesson while keeping Vulkan as the backend.

## 29. Validation gates for CP1 implementation

Before CP1A is allowed to close:

1. **Header purity:** automated scan fails if common `rendercore` headers contain `osg::`, `osgViewer::`, `osgUtil::`, `vsg::` or Vulkan API types/includes.
2. **Typed-handle safety:** compile/unit test proves handle families cannot be mixed accidentally.
3. **Stale handle:** retire + reuse test proves old generation cannot mutate/read the replacement.
4. **Chunk stale result:** replace a chunk, then publish work for the old generation; old work is rejected.
5. **World epoch:** after clear/world reset, prior-epoch updates are rejected.
6. **Ordered publish:** same input operation sequence produces identical logical RenderWorld state/order.
7. **No raw backend identity:** common records contain no backend/node pointer identity.
8. **Residency independence model:** simulated backend eviction/recreate leaves logical handles/live records valid.
9. **Frame immutability:** once sealed, frame state is read-only and safe for parallel backend recording.
10. **Temporal history:** current/previous transform and camera history advances only from rendered frames and resets on a cut/history-epoch change.
11. **FBFP multi-view:** owner-head/main-scene hide + shadow-view presence can be represented without mutating logical membership.
12. **OpenGL parity:** with Vulkan absent, existing CP0 reference scenes and behavior remain unchanged within the frozen visual corpus.
13. **No V3 patch-harness dependency:** CP1 builds from the materialized V3.25 source now committed to the V4 lineage.

## 30. CP1A implementation order

Once CP0 visual freeze removes the implementation block:

1. add `rendercore` handle/math/epoch/revision types + unit tests;
2. add logical resource/instance/chunk/light tables + stale-generation tests;
3. add immutable `RenderWorldUpdateBatch` + deterministic apply path;
4. add `FrameRenderState`/`FrameView` + temporal-history tests;
5. introduce the semantic renderer service without changing the active OSG renderer;
6. adapt the smallest safe `RenderingManager`/`Objects` lifecycle surface to publish neutral operations while retaining current OSG mutations;
7. expand producer coverage incrementally across camera/environment/actors/paging with visual parity checks;
8. only then begin CP1B SDL3 migration.

No step is justified by a performance claim. CP1A is an ownership/correctness checkpoint whose value is enabling the later Vulkan/parallel/GPU-driven work without backend leakage.

## 31. Final architecture invariant

At the end of CP1, it must be possible to implement a renderer that knows nothing about `MWWorld::Ptr` and a game/world producer that knows nothing about OSG/VSG/Vulkan objects.

That is the gate that makes David's Vulkan implementation portable into our engine and Clockwork's persistent GPU-scene architecture adoptable later without another semantic rewrite.