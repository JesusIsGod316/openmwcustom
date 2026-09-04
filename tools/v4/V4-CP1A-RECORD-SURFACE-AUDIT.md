# V4.0 CP1A — Neutral record-surface audit

Status: **DESIGN AUDIT — FOUNDATION TYPES EXIST, MINIMUM CP1 RECORD SURFACE NOT YET COMPLETE**

Authority: `V4-CP1-NEUTRAL-RENDER-CONTRACT.md` remains the contract. This audit compares that locked minimum against the first header/test-only `RenderWorld` scaffold so Increment C does not accidentally freeze an under-specified common record ABI.

## 1. Verdict

The first foundation correctly establishes typed handles, monotonic epoch/revision identities, deterministic slot allocation, neutral GLM transforms, and the seven persistent table families. It is **not yet a complete implementation of CP1 sections 9–14**.

That is acceptable at the current foundation stage, but `RenderWorldUpdateBatch` must not become the authoritative producer vocabulary until the persistent record surface can represent the locked semantics below. Otherwise later material/animation/FBFP/paging work would force another ownership/API rewrite.

No field added here may encode Vulkan/VSG/OSG residency or submission ABI.

## 2. MeshRecord gap

Current scaffold has revision, source identity, surface count, and skinned/morphed capability. Locked minimum still needs backend-neutral bounds, primitive topology class, optional skeleton compatibility metadata, and a logical geometry/source payload reference sufficient to reconstruct backend residency without OSG/VSG identity.

CP1 must **not** freeze vertex-buffer packing, index type, VSG arrays, Vulkan vertex input state, meshlet layout, indirect offsets, or device addresses.

## 3. TextureRecord gap

Current scaffold has only revision + source identity. Locked minimum still needs semantic width/height where known, format class where known without Vulkan format ABI, color-space interpretation, semantic role information, and sampler semantics independent of descriptor allocation.

## 4. SkeletonRecord gap

Current scaffold has only revision + source identity. Locked minimum still needs stable ordered bone identities/names, parent hierarchy, bind/inverse-bind transforms, and stable bone indexing for pose publication. Do not store VSG skeleton nodes, OSG bones, GPU palette addresses, descriptor offsets, or animation-controller objects.

## 5. MaterialRecord gap — largest missing surface

Current scaffold has only revision + source identity. Before material update operations are frozen, the logical material must be able to preserve final V3.25 semantics for diffuse/ambient/specular/emission, shininess, emissive multiplier, specular strength, vertex-color mode, environment-map color/strength, alpha value/test threshold, blend/ordering semantics, cull/two-sided state, unlit/emissive behavior, texture transform/UV selection, and logical texture bindings for Diffuse, Dark, Detail, Decal, Emissive, Normal, Environment, Specular, Bump, Gloss, and Blend/terrain roles.

This record must not contain bindless handles, texture-array layers, descriptor-set IDs, pipeline IDs, or Clockwork backend material mode.

The first object-lifecycle slice is still allowed to leave `MaterialHandle` unbound/deferred. The requirement is that the **common type surface is extensible without changing ownership identity**, not that CP1A immediately parse every NIF material through the neutral path.

## 6. InstanceRecord gap

Current scaffold already has optional chunk, mesh, material bindings, optional skeleton, current world transform, semantic visibility flags, and lighting participation. Locked minimum still needs bounds, LOD center/range/policy, small-feature-culling eligibility, optional logical parent `InstanceHandle`, attachment/bone binding semantics, and explicit category semantics where existing flags are insufficient for effect/projectile/groundcover/terrain-extra expansion.

Do not add backend batch IDs, draw buckets, indirect command slots, descriptor indices, or fixed per-view masks.

## 7. ChunkRecord gap

Current scaffold has revision, producer identity string, and derived ordered individual-instance membership. Locked minimum still needs backend-neutral bounds, worldspace/cell/producer semantic identity values that do not rely on `CellStore*`, group visibility/LOD policy, and later compact chunk-item representation without duplicating first-slice individual logical instances.

The relationship invariants in `V4-CP1A-RENDERWORLD-INVARIANTS.md` apply.

## 8. LightRecord gap

Current scaffold already has revision, position, diffuse, constant/linear/quadratic attenuation, effective radius, and enabled state. Locked minimum still needs specular/ambient where current semantics use them, actor fade where applicable, and semantic view/pass participation.

Cluster-grid indices, SSBO offsets, descriptor bindings, culled-light lists and backend radius caches remain backend-derived.

## 9. Neutral helper types to define before Increment C freezes operations

The next header/test-only record increment should introduce only semantic helper types needed by the locked contract, for example: world/local bounds type; primitive topology enum; texture format/color-space/role vocabulary; sampler semantic struct; material texture-role binding; vertex-color/blend/cull semantic enums; LOD policy/value struct; attachment/bone binding value type; and extensible semantic pass/category flags that are **not** a fixed view-mask ABI.

Names are implementation details; semantic separation is mandatory.

## 10. Sequencing rule

Before authoritative immutable update batches are added:

1. close RenderWorld relationship/referential-integrity gaps;
2. complete enough of the persistent record semantic surface that every required CP1 operation has a stable neutral destination;
3. keep fields defaultable so the first narrow legacy slice can populate only semantics it actually owns;
4. then define typed update operations against those records;
5. only after batch validation/read publication exists should the game/OSG producer seam consume the API.

This remains architecture/correctness work. It does not authorize VSG/Vulkan, renderer behavior changes, or a performance claim.
