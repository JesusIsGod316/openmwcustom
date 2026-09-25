# VulkanMW Native Renderer Architecture Plan

Status: Phase 0 foundation
Project: VulkanMW
Repository: JesusIsGod316/openmwcustom
Foundation branch: vulkanmw/phase0-foundation
Accepted OpenGL base: optimizedmw/gl-p1p2-repair-ffpb@41ff409f8ba8fce55f57eecb8ece45b196aa000b
Parked Vulkan reference: codex/cp4f-material-frame-repair@be2869dd00a49774ff2642ec238d98f1456b2b8a
Donor/reference: vsgopenmw-dev/vsgopenmw
Date: 2026-09-25

## 1. Executive decision

VulkanMW will not continue optimizing the old steady-state OSG -> capture/translation -> neutral frame -> VSG path as the primary architecture.

The retained V4 renderer remains a valuable compatibility and implementation reference, but the normal VulkanMW path must progressively remove live OSG scene-graph ownership from Vulkan gameplay.

The target architecture is:

OpenMW game/mod semantics
-> persistent backend-neutral RenderWorld
-> immutable/versioned FrameRenderState
-> direct VSG/Vulkan realization and delta updates
-> Vulkan

The RenderWorld/FrameRenderState layer is retained. It is not the source of the observed fourfold frame-cost gap. The expensive debt is primarily the producer side: deriving renderer semantics from an already-evaluated OSG scene, repeatedly capturing dynamic geometry/material state, CPU-deforming actors, and continuing an OSG update-only scene while VSG owns presentation.

Core rule:

**Convert authored content once. Publish semantic changes as deltas. Never rediscover normal Vulkan rendering semantics by traversing an OSG scene graph.**

## 2. Evidence and audit basis

The parked retained renderer at be2869dd00 reduced the prior Vulkan mean frame time from approximately 72.95 ms to 60.28 ms in the matched Seyda Neen cohort, a roughly 17.4% reduction. The OpenGL control was approximately 15.45 ms, leaving retained Vulkan at roughly 3.90x OpenGL frame cost.

The retained implementation proved that persistence helps:
- 1682 resident persistent draws were observed.
- steady-state static resource realizations were zero.
- static compilation disappeared from the sampled steady interval.
- pipeline audit median fell from roughly 5.02 ms to 0.78 ms.
- persistent draw synchronization was roughly 0.09 ms.

Remaining sampled hot work still included:
- capture: ~13.17 ms
- dynamic realization: ~11.45 ms
- command recording: ~11.84 ms
- OSG update: ~4.49 ms

These intervals are inclusive/overlapping and must not be added as independent frame costs. They identify architectural targets.

The existing source already contains the right neutral foundation:
- RenderCore::RenderWorld owns stable mesh/model/material/texture/skeleton/instance/chunk/light tables.
- RenderWorld uses stable handles, revisions, ownership validation, retirement and referential-integrity checks.
- FrameProducer/FrameRenderState already model transforms, current/previous skeleton poses, morph weights, cameras, derived shadow/water views, auxiliary views, environment, render targets and render passes.
- records.hpp already models NIF-like geometry, models, materials, texture bindings/transforms, skins, morphs, skeletons, switches, LOD, billboards, sorting and dynamic requirements.
- the VSG backend already contains static realization, retained membership, persistent resources, GPU-safe retirement, pipeline inventories, native UI/video, native terrain, auxiliary views, water/sky and submission infrastructure.

The main architectural debt is therefore not the neutral model itself. It is how normal runtime content reaches it.

## 3. Non-negotiable compatibility boundary

Backend internals are negotiable. User-facing OpenMW behavior is not.

VulkanMW must preserve:
- ESM/ESP/content interpretation.
- winning VFS behavior.
- Lua/script APIs and observable ordering.
- savegame format and save semantics.
- quests, AI, physics and mechanics results.
- animation source priority and animation event/text-key semantics.
- body-part/equipment/attachment semantics.
- Full Body First Person behavior where supported by the OpenGL build.
- ordinary content mods.
- supported material, lighting, shadow and postprocessing semantics.
- current OpenGL backend as a selectable compatibility/reference backend during migration.

Renderer-specific OpenGL shader/postprocessing hacks may require a Vulkan implementation or OpenGL fallback. They are a separate compatibility class from normal content mods and saves.

## 4. Target architecture

### 4.1 Semantic core

OpenMW gameplay systems remain authoritative:
- world and references
- VFS
- animation rules
- mechanics/AI
- physics
- Lua/scripts
- saves

Those systems publish renderer semantics into RenderWorld rather than constructing a scene graph that Vulkan later reverse-engineers.

### 4.2 Persistent RenderWorld

RenderWorld remains the stable semantic ownership layer.

It stores canonical resources and stable identities:
- MeshRecord
- ModelRecord
- MaterialRecord
- TextureRecord
- SkeletonRecord
- InstanceRecord
- ChunkRecord
- LightRecord

It must remain free of OSG, VSG and Vulkan dependencies.

### 4.3 Immutable/versioned frame state

FrameRenderState represents the dynamic frame boundary:
- object transforms
- skeleton poses
- morph weights
- dynamic material/texture state
- camera and previous-camera state
- environment
- lights
- views
- render targets/passes
- frame history validity

Frames contain values and handles, not live scene-graph pointers.

### 4.4 VSG/Vulkan backend

VSG consumes RenderWorld plus FrameRenderState.

It owns:
- GPU buffers/images
- pipeline state
- descriptors
- resident VSG resources
- compilation
- command recording
- synchronization
- frame retirement
- presentation

It must not reach back into gameplay state or OSG to discover missing semantics.

## 5. Phase 0 — Foundation, branch and architectural guardrails

### Goals

1. Start VulkanMW from the newest runtime-accepted OpenGL lineage.
2. Preserve all newer accepted paging/memory/compatibility work.
3. Preserve the parked Vulkan source as a reference.
4. Define and automatically enforce dependency boundaries.
5. Quarantine transitional OSG-capture code rather than expanding it.
6. Establish acceptance metrics before native migration begins.

### Starting base

Initial Phase 0 base:
optimizedmw/gl-p1p2-repair-ffpb@41ff409f8ba8fce55f57eecb8ece45b196aa000b

Reason:
- explicitly runtime accepted.
- retains P1A/P1B/P2.
- descendant of the parked Vulkan checkpoint.
- GitHub comparison showed the accepted base is 62 commits ahead and zero behind be2869dd00.
- the V4 RenderCore/VSG foundation remains present.
- P3B is runtime-positive but still provisional/not default accepted, so it is not part of the Phase 0 foundation unless later promoted and deliberately rebased/cherry-picked.

### Phase 0 dependency rules

RenderCore:
- no osg headers or osg:: types.
- no vsg headers or vsg:: types.
- no Vulkan headers or Vk* API ownership.

Native Vulkan semantic producers:
- no OSG scene-graph dependency.
- no osg::Node, StateSet, Drawable, Geometry or NodeVisitor.
- may depend on OpenMW gameplay/content semantics and RenderCore.
- temporary OSG math conversions are allowed only at explicitly documented legacy seams and should not require a live scene graph.

VSG backend:
- consumes RenderCore/semantic data.
- no direct MWWorld/MWMechanics/MWLua gameplay ownership.
- no live OSG traversal.
- no gameplay mutation.

Transitional V4 capture:
- remains buildable for compatibility/reference while migration is incomplete.
- must not gain new normal-path responsibilities when a native producer can be implemented instead.
- every runtime fallback must be measurable and attributable.

### Phase 0 exit gate

- branch exists from exact accepted base.
- architecture plan committed.
- boundary checker committed.
- OpenGL behavior unchanged.
- no Vulkan performance or runtime acceptance claimed.
- native migration paths and transitional quarantine are explicit.
- later VulkanMW commits must respect boundary checks.

## 6. Phase 1 — Native NIF semantic compiler

### Objective

Replace normal:
NIF -> NifOsg -> OSG scene/state -> capture -> RenderCore

with:
NIF -> OpenMW NIF parser -> canonical RenderCore records

VSG then realizes those records directly.

### Components

Introduce a native semantic compiler, logically:

NifSemanticCompiler
- compileModel
- compileMesh
- compileMaterial
- compileTextureBindings
- compileSkeleton
- compileSkin
- compileMorphs
- compileControllers
- compileSwitchLodBillboard
- compileEffects

### Source authority

Use current OpenMW components/nif structures as the data source.

Use vsgopenmw selectively as a donor/reference for:
- geometry conversion
- triangles/strips/lines
- material and blend semantics
- texture-slot handling
- skin weights/indices
- skeleton/bone handling
- keyframe controllers
- morphs
- switches
- billboards
- depth sorting
- particle semantics

Do not wholesale merge or rebase onto vsgopenmw. Its ownership model and upstream base differ from this project.

### Cache identity

Canonical asset identity should include at minimum:
- normalized winning VFS path
- content identity/hash
- model interpretation options that change semantics
- compatibility/material schema version

Multiple world instances must share one canonical asset.

### Phase 1 exit gate

- deterministic semantic output for a representative NIF corpus.
- static Vulkan corpus renders without constructing OSG rendering nodes.
- invalid/unsupported semantics fail closed with source identity diagnostics.
- OpenGL backend remains unchanged.

## 7. Phase 2 — Native static world

### Objective

Make static interiors/exteriors use direct semantic publication and retained GPU resources.

Object lifecycle becomes:
- add -> resolve canonical model -> create InstanceRecord
- move -> update transform
- enable/disable -> update visibility/flags
- material semantic change -> update affected record/revision
- remove -> retire instance

No OSG traversal is permitted for normal static content.

### Reuse

Retain/refine:
- ActiveCellProducer
- StaticPopulationProducer
- RenderWorld
- StaticAssetRealizer
- persistent resource ownership
- GPU-safe retirement
- native terrain chunk/residency pipeline
- current memory/paging policies where backend-independent

Groundcover should migrate to equivalent native semantic ownership.

### Phase 2 exit gate

On representative interior/exterior routes after warmup:
- normal static OSG capture count = 0
- steady static resource realization = 0
- static asset rebuilds occur only on real asset/revision changes
- visual parity passes representative corpus

## 8. Phase 3 — Native controller and animation runtime

### Objective

Stop using OSG mutation as the canonical output that Vulkan later inspects.

Animation evaluation should publish semantic values directly:
- local transforms
- switch selections
- visibility
- texture transforms
- material colors
- morph weights
- animation/controller timing
- event/text-key results

### Compatibility rules

OpenGL behavior is the oracle for:
- source priority
- .kf additions
- loop behavior
- controller timing
- animation events/text keys
- duplicate-name behavior
- attachments
- first/third person behavior
- Lua-observable state

vsgopenmw animation/mwanimation code is a donor/reference, not a drop-in replacement.

### Phase 3 exit gate

- controller/pose corpus matches the OpenGL reference semantically.
- Vulkan results do not require OSG traversal.
- no normal dynamic controller state is reconstructed from osg::StateSet or osg::Node.

## 9. Phase 4 — Native actors and GPU deformation

### Objective

Eliminate actor graph capture and normal CPU vertex deformation.

### GPU skinning

Static per asset:
- base positions/normals/tangents
- indices
- bone indices
- bone weights
- bind data

Per frame:
- bone matrix buffer
- previous bone matrix buffer when motion vectors require it

Vertex shader performs normal skinning.

### GPU morphing

Static:
- morph target deltas resident on GPU.

Per frame:
- current/previous morph weights.

Prefer shader/compute realization chosen from measured workload and simplicity.

### Actor semantic publication

Publish:
- skeleton identity
- composed body parts
- equipment
- weapon/ammunition attachments
- pose
- morph weights
- visibility
- enchant state
- first-person flags
- placement

The renderer must not need an OSG skeleton.

### Transitional code to retire

When native equivalents are proven:
- V4RigidActorPose
- MorphCollector
- OSG actor-placement capture
- normal actor geometry capture
- normal CPU-deformed MeshPayload generation

### Phase 4 exit gate

Representative actor/mod corpus:
- zero OSG pose capture
- zero normal CPU vertex deformation
- correct vanilla creatures/NPCs
- Better Bodies/custom body parts
- armor/clothing/weapons
- external .kf
- first/third person
- Full Body First Person
- morphs
- attachment edge cases

## 10. Phase 5 — Native effects, particles and projectiles

### Objective

Replace generic subtree capture with persistent semantic effect instances.

Effect state should express:
- canonical effect asset
- transform
- controller/start time
- visibility
- color
- scale
- particle state
- attached light
- render semantic flags

Assets compile once; instances update by state.

ImmediateEffectDraw should be restricted to genuinely immediate/generated rendering primitives, diagnostics or unavoidable temporary compatibility—not arbitrary model representation.

Initial particle simulation may remain CPU-side if it publishes native buffers efficiently. GPU simulation is a later measured optimization, not a prerequisite.

### Phase 5 exit gate

Ordinary:
- spell VFX
- enchantment effects
- physical projectiles
- magic bolts
- particles
- attached lights

run without OSG subtree capture.

## 11. Phase 6 — Secondary views, UI and postprocessing

Reuse current native work where sound:
- VSG MyGUI
- video texture path
- character preview
- local/global map
- save thumbnails/screenshots
- shadow views
- water reflection/refraction
- auxiliary render targets

All secondary views must reuse the same canonical RenderWorld assets. A shadow/map/preview view must not create a second model interpretation path.

Postprocessing compatibility should reproduce supported OpenMW public semantics where practical. OpenGL-specific hacks may require Vulkan-specific implementations or the OpenGL backend.

### Phase 6 exit gate

Main view and secondary views consume shared native assets, with visual parity and no normal OSG rendering dependency.

## 12. Phase 7 — Remove the Vulkan OSG update-only world

### Objective

Delete the need for V4UpdateOnlyViewer from normal Vulkan gameplay.

Final Vulkan runtime ownership:
- Engine
- World
- Mechanics
- Physics
- Lua
- native animation/controller evaluation
- RenderWorld publisher
- VsgRuntimeHost

There is no live gameplay osgViewer scene traversal.

OSG helper/math libraries may remain temporarily where harmless. The architectural requirement is zero live OSG scene-graph/update traversal for Vulkan rendering semantics.

### Transitional code to remove/quarantine

- V4UpdateOnlyViewer
- V4PersistentObject
- general V4EffectCapture
- AnimatedObjectCaptureVisitor
- V4RigidActorPose
- whole-object OSG capture plans
- generic OSG-to-neutral runtime walkers

### Phase 7 exit gate

VulkanMW can:
- boot
- reach menu
- start new game
- load existing save
- transition cells
- animate actors
- run effects/projectiles
- use maps/previews/UI
- exit normally

with zero live-game OSG scene traversal and zero normal OSG capture fallback.

## 13. Phase 8 — Vulkan-specific performance optimization

Do this only after the clean architecture is functional.

Targets may include, when profiling proves headroom:
- parallel command recording
- persistent command structures
- draw packet reuse
- descriptor indexing/bindless resource tables
- static instancing
- multidraw
- indirect drawing
- GPU culling
- visibility/occlusion refinement
- pipeline-cache refinement
- resource upload scheduling
- async compilation

Do not assume these mechanisms help. Every candidate needs mechanical activity proof plus matched runtime performance evidence.

## 14. Compatibility and fallback policy

Development may keep an explicit compatibility capture mode, but it is not the shipping design.

Required behavior:
- fallback use is counted
- offending asset/semantic is named
- fallback is visible in diagnostics
- native acceptance routes target zero fallback
- unsupported semantics fail closed rather than silently disappearing

A future debug switch may retain the old capture route for forensic comparison. Normal VulkanMW must not depend on it.

## 15. CI and validation plan

Every substantive VulkanMW checkpoint should preserve an OpenGL control.

Required gates:
- GCC source/contract checks
- Clang ASAN/UBSAN
- MSVC OpenGL build
- MSVC VulkanMW build
- Vulkan validation
- deterministic NIF semantic corpus
- animation/controller parity corpus
- rendering pixel/visual corpus
- mod/save smoke route
- matched performance benchmark after correctness

Architectural contracts should enforce:
- RenderCore has no OSG/VSG/Vulkan dependency.
- native Vulkan producers have no OSG scene-graph dependency.
- VSG backend does not reach into gameplay ownership.
- native acceptance route reports zero OSG capture fallback.

## 16. Benchmark and promotion rules

### Architecture acceptance

Architecture acceptance is not final renderer promotion.

Before advanced Vulkan tuning, require:
- OSG update = 0 in native Vulkan gameplay
- OSG capture = 0 in native validation route
- steady static realization = 0 after warmup
- normal actor CPU deformation = 0
- representative save/mod route functional

### Feasibility gate

A strong feasibility signal is roughly <=1.5x matched OpenGL frame cost on CPU-heavy parity workloads. This is a planning gate, not a promised result.

Using the historical ~15.5 ms OpenGL cohort as scale, <=~23 ms VulkanMW would demonstrate that the architectural gap has mostly been closed and justify deeper Vulkan optimization.

### Final promotion

Do not promote VulkanMW solely because it is functional.

Require:
- median frame time near OpenGL
- p95/p99 near or better than OpenGL
- no major transition regression
- safe 8 GB VRAM behavior
- safe 32 GB system-RAM behavior
- visual parity
- representative mod/save compatibility
- no normal OSG fallback dependence

## 17. Ordered implementation sequence

1. Phase 0 branch/foundation/guardrails.
2. Direct NIF -> RenderCore compiler.
3. Native static world and groundcover.
4. Native controller/animation evaluation.
5. Native actor composition and pose publication.
6. GPU skinning and morphing.
7. Native effects/projectiles/particles.
8. Complete secondary views against shared native assets.
9. Remove OSG update-only Vulkan world and normal capture bridges.
10. Benchmark clean architecture.
11. Profile and implement only proven Vulkan-specific optimization targets.
12. Temporal inputs/DLSS/Reflex/frame generation only after renderer/frame-pacing stability.

## 18. Explicit reuse versus replacement

### Keep and extend
- RenderWorld
- FrameRenderState / FrameProducer
- stable handles/revisions
- semantic records
- VSG runtime/bootstrap/submission
- static realizer
- terrain pipeline
- UI/video work
- resource lifetime and GPU-safe retirement
- retained membership/resource patterns
- pipeline inventories
- material compatibility knowledge
- texture resolver
- current validation/diagnostic corpus
- newer accepted OpenGL paging/memory work that is backend-independent

### Replace or quarantine
- OSG-derived normal Vulkan producers
- generic runtime scene traversal/capture
- V4UpdateOnlyViewer as a permanent Vulkan dependency
- CPU-deformed actor meshes as the normal path
- ImmediateEffectDraw as a generic arbitrary-object compatibility format
- repeated OSG material/state reconstruction

## 19. Phase 0 current action

This file begins Phase 0.

Current branch:
vulkanmw/phase0-foundation

Current accepted foundation:
41ff409f8ba8fce55f57eecb8ece45b196aa000b

No Vulkan runtime or performance acceptance is granted by Phase 0 source scaffolding. The next implementation work is to enforce the dependency boundaries, inventory native-versus-transitional producers, and begin the direct NIF semantic compiler without expanding the old capture route.
