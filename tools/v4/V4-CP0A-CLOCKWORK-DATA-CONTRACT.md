# V4.0 CP0A — Project Clockwork Data-Contract Audit

Status: **CP0A architecture donor handoff**  
Clockwork donor archive: `gpu-lowend-tuning`, SHA-256 `acc75476f5020c6facb5e8b573b2453b037892396a50fa44d5cc9716e82ed437`  
Disposition: **ALGORITHM/DATA-ARCHITECTURE DONOR — NOT AN OPENGL IMPLEMENTATION PORT**

The purpose of this document is to extract the data contracts V4 should reserve from day one so the renderer can evolve into a persistent GPU-driven scene without a second ownership rewrite. Clockwork's OSG capture callbacks, StateSet parsing and OpenGL submission are explicitly outside the common V4 contract.

## 1. What Clockwork proves architecturally

Clockwork's value is not `glMultiDraw*` by itself. Its important change is replacing repeated per-view scenegraph reconstruction with a persistent flat representation whose records can be reused by every view.

The relevant flow is:

`persistent logical mesh/material/instance data -> per-view visibility -> indirect command counts -> backend submission`

V4 should preserve that shape while replacing Clockwork's OSG/GL input/output mechanisms with:

`RenderWorld update batches -> persistent RenderWorld -> immutable FrameRenderState -> Vulkan/VSG backend GPU scene`

## 2. Generation-safe chunk identity is a CP1 requirement

Clockwork's static-world `Chunk` contains:

- generation;
- first instance;
- instance count;
- per-batch counts;
- mesh references;
- material references;
- bounds;
- light-base metadata.

Its `GpuInstance` stores both chunk slot and chunk generation. The cull shader rejects an instance when the chunk generation no longer matches the live chunk table before any view visibility work is accepted.

That is the key semantic lesson.

V4 must define a distinct `ChunkHandle { slot, generation }` in CP1. A streamed replacement may reuse a slot only with a new generation. Work produced for an older generation is stale and must be ignored without touching the replacement.

The following Clockwork fields are **backend derived**, not part of the common Chunk ABI:

- `firstInstance`;
- packed instance range;
- GPU batch counts;
- indirect-command offsets;
- backend mesh/material numeric IDs.

The common layer owns only logical membership, stable resource handles, bounds, worldspace/cell identity as needed, and semantic visibility/pass classifications.

## 3. Logical instance record vs GPU instance record

Clockwork's `GpuInstance` is a compact GPU ABI containing:

- 3x4 transform rows;
- bounding sphere;
- LOD parameters;
- metadata including chunk slot/generation;
- material/batch metadata.

V4 must **not** freeze this OpenGL-era GPU record as the common engine layout. Instead CP1 needs a logical `InstanceRecord` sufficient to generate an equivalent Vulkan record later:

- stable `InstanceHandle`;
- owning `ChunkHandle` when streamed/static, otherwise no chunk;
- `MeshHandle`;
- `MaterialHandle` or material-slot set where a mesh has several surfaces;
- optional `SkeletonHandle`;
- world transform;
- bounds;
- LOD center/range/policy;
- small-feature-culling eligibility;
- semantic pass/visibility flags;
- lighting participation;
- optional parent/attachment binding for equipment/effects.

GPU packing, batch bucket IDs and indirect-command slots remain backend-private.

## 4. Multi-view visibility contract

Clockwork's cull stage processes the same persistent instance population across several views. Each view has frustum planes, pixel-size/small-feature data, viewpoint + LOD scale, view-projection data and flags. Static chunks carry a per-view mask; visibility is written by view region, then the command stage creates per-view indirect draw counts.

This is the architecture V4 wants, but the common layer should not hard-code Clockwork's exact 32-bit view mask or its OpenGL buffer layout.

CP1 must reserve an explicit backend-neutral `FrameView` list. Each view descriptor needs at least:

- stable frame-local view index;
- semantic view kind (`Main`, `Shadow`, `Reflection`, `Refraction`, `Map`, `Preview`, `PrecipitationOcclusion`, `Debug`, extensible);
- current view/projection and derived view-projection;
- previous view/projection when temporal history exists;
- viewport/render extent;
- viewpoint/world origin;
- LOD scale;
- small-feature policy;
- inclusion/exclusion pass policy;
- history validity / camera-cut identity where applicable.

The backend is free to compile this list to a 32-bit mask, 64-bit mask, table, bitset or separate dispatch ranges.

## 5. Owner/FBFP visibility must be semantic, not camera-node tricks

Clockwork demonstrates per-view masks, and final V3.25 requires camera-specific owner visibility: the full body can be present for first-person gameplay while owner head/hair/helmet are hidden in the main first-person view but still participate correctly in shadow/secondary views.

Therefore `InstanceRecord`/attachment records need semantic visibility classes or pass flags that can be evaluated against each `FrameView`.

Do not encode this as:

- one backend camera pointer;
- one VSG node mask owned by the backend;
- one OpenGL cull callback;
- permanent deletion of the head from the logical actor.

The same logical part must be renderable in one view and hidden in another.

## 6. Material contract extracted from Clockwork + V3.25

Clockwork formalizes the supported material map order as:

1. Diffuse
2. Dark
3. Detail
4. Decal
5. Emissive
6. Normal
7. Environment
8. Specular
9. Bump
10. Gloss
11. Blend (terrain)

Its GPU material also carries diffuse, ambient, specular, emission, environment color, parameters/modes and texture transforms. Current V3.25 `SceneUtil::MaterialConfig` independently requires diffuse, ambient, specular, emission, shininess, emissive multiplier, specular strength and vertex-color mode.

CP1 should therefore define a logical material description that preserves at minimum:

- those V3.25 material color/scalar semantics;
- the full map roles above;
- texture transforms/UV-set semantics where used;
- alpha-test/blend mode and thresholds;
- blend function/ordering semantics where content requires them;
- cull/two-sided state;
- unlit/emissive behavior;
- vertex-color mode;
- normal/specular/PBR-related content semantics already supported by the final V3.25 renderer.

Clockwork's `MaterialMode { Legacy, Array, Bindless }` is **not** part of the logical material. That is a backend residency/submission tier selected at runtime.

## 7. Texture fallback tiers are backend policy

Clockwork uses three material/texture tiers:

1. bindless when supported;
2. texture arrays as fallback;
3. legacy per-material binding as a correctness floor.

V4 should preserve this as a backend policy principle, not as a semantic API.

The logical `TextureHandle` remains valid regardless of which tier currently represents it. A material must not change identity merely because the backend demotes from bindless to array or legacy.

Unsupported material content should fall back to a slower correct path rather than disappearing or rendering incorrectly.

## 8. Three independent lifetime domains

Clockwork's material/texture/mesh retirement code reinforces the lifetime split already identified in the V3.25 overlap audit.

V4 locks three independent domains:

1. **Content/source lifetime** — VFS/NIF/image/animation source data and CPU caches.
2. **Logical RenderWorld lifetime** — `MeshHandle`, `MaterialHandle`, `TextureHandle`, `SkeletonHandle`, `InstanceHandle`, `ChunkHandle`, `LightHandle` records and revisions.
3. **Backend residency lifetime** — Vulkan buffers/images, descriptors, pipelines, VSG objects, staging allocations and device-local memory.

Backend eviction or device-resource recreation must never invalidate a logically live handle. The backend must be able to request/reconstruct residency from the logical resource/source layer.

## 9. Explicit 8 GB VRAM budgeting is mandatory

Clockwork carries an explicit texture budget, derives/uses a mesh budget, tracks pending bytes, retires resources and can leave new meshes on the legacy path when a budget is exhausted.

V4 has an 8 GB hard VRAM constraint and therefore reserves accounting from CP2, not after the renderer is complete.

Backend residency accounting needs categories at minimum for:

- textures/images;
- mesh vertex/index storage;
- skinning buffers;
- instance/material/light buffers;
- render targets and temporal histories;
- shadow maps;
- water reflection/refraction targets;
- postprocess intermediates;
- staging/temporary allocations where materially resident.

Policies must distinguish:

- logical live bytes;
- resident bytes;
- pending upload bytes;
- pending retire bytes;
- pinned/non-evictable bytes;
- evictable bytes.

Promotion rule: no optimization that materially improves CPU/GPU time at the cost of unsafe adapter pressure near the 8 GB limit.

## 10. Retirement and stale-work contract

Clockwork keeps generation IDs for chunks and separates retirement from immediate destruction. V4 generalizes this:

- logical retire invalidates new lookup/update against the old generation;
- queued/asynchronous work contains the generation/revision it was prepared from;
- publish rejects stale work;
- backend GPU allocations retire only after the last frame/fence that can reference them;
- slot reuse increments generation before a new logical object is published;
- generation `0` is reserved as invalid/uninitialized;
- no raw pointer lifetime is used as stale-work protection.

This applies to chunks, instances and logical resources.

## 11. Skinning contract extracted from Clockwork

Clockwork contains explicit GPU skin data structures for:

- skin groups;
- bone influence ranges;
- influences (`bone`, `weight`);
- skin-mesh metadata;
- pose matrices;
- per-frame/per-snapshot pose identity;
- skin update records.

This validates reserving GPU skinning as a later backend option without making it mandatory in CP1.

Common V4 requirements:

- `SkeletonHandle` identifies immutable skeleton/bind data;
- mesh skinning data identifies bone influences independent of a live OSG/VSG skeleton node;
- `FrameRenderState` publishes current pose matrices for visible/dynamic actors;
- previous pose matrices/history identity are available when motion-vector generation requires them;
- pose/source/event selection remains in game/animation semantics, not the backend;
- CPU skinning remains a correctness fallback until GPU skinning is independently validated.

Morph weights need the same current/previous frame identity because facial/head morphs and other morph targets can contribute to temporal motion.

## 12. Lighting data

Clockwork stores per-view light ranges/lists and chunk light-base information. This is useful for GPU layout but does not supersede final V3.25 lighting semantics.

CP1 needs a stable `LightHandle` and logical light records. The backend may build clustered grids, per-view lists, light ranges or other acceleration structures from them.

Do not make a Clockwork light-range offset a persistent engine identity.

## 13. Occlusion contract

Clockwork's cull stage shows the correct placement for eventual GPU occlusion: after generation validation and as part of shared multi-view visibility. Its implementation is still an algorithm reference only.

V4 promotion rules remain:

- zero visible false occlusion;
- fail open on uncertain/stale/missing data;
- no same-frame CPU queue-then-wait dependency;
- view history and depth-pyramid identity must match the frame/view being tested;
- occlusion can be disabled without changing semantic scene membership.

## 14. What must remain backend-private

The following Clockwork concepts are deliberately **not** part of the CP1 common ABI:

- OSG `CullVisitor`/capture callbacks;
- OSG `StateSet` as material authority;
- OpenGL buffer bindings;
- GL fences/barriers;
- multi-draw-indirect command struct layout;
- `firstInstance` allocators;
- packed batch IDs;
- bindless handles;
- array layer IDs;
- shader-storage-buffer binding numbers;
- fixed maximum view bit width;
- OpenGL ray/skin readback objects.

They are implementation details to be re-derived for Vulkan.

## 15. CP1 requirements derived from Clockwork

Clockwork adds the following non-negotiable requirements to the CP1 handoff:

- distinct generation-safe `ChunkHandle`;
- distinct stable `LightHandle`;
- logical resources separate from backend residency;
- chunk replacement with stale-generation rejection;
- instance records rich enough to generate flat GPU records later;
- explicit frame view list rather than a single-camera contract;
- per-view semantic visibility/pass policy;
- material map roles broad enough for current content and future bindless/array packing;
- current/previous skin/morph data reservation for the temporal path;
- explicit residency accounting hooks from the first Vulkan bootstrap;
- correctness fallback for unsupported content.

With these reserved in CP1, CP8 can implement a Clockwork-derived Vulkan GPU scene without changing the game/render ownership boundary again.