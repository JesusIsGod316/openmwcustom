# VulkanMW Phase 2 — Native Static World, Terrain and Groundcover

Status: P2A native static-world lifecycle implemented; focused QC pending
Branch: vulkanmw/phase2-native-static-world
Accepted parent: vulkanmw/phase1-nif-semantic-compiler@5b2a2128ccc37da84a16112be1965b8931352346

## Phase 2 objective

Move ordinary world geometry ownership onto:

winning VFS content
-> NifAssetService
-> RenderWorld canonical assets
-> native cell/static/population publication
-> retained VSG/Vulkan realization

without using live OSG scene traversal as the normal Vulkan source of truth.

Phase 2 also owns native terrain and groundcover. P2A isolates the ordinary static object/cell lifecycle first; terrain and groundcover follow on the same branch after the static boundary is green.

## P2A implemented

### Native StaticWorldService

`RenderNative::StaticWorldService` now owns neutral mutation policy for:
- active cell publication and retirement
- exterior population-cell creation/retirement
- interior static instance upsert
- exterior data-oriented population upsert
- interior <-> exterior representation transitions
- static reference retirement

The service consumes only RenderCore semantic sources and producer interfaces.

### OSG-free static source adapter

`apps/openmw/mwrender/vulkanmw/staticworldsource.*` publishes:
- canonical cell identity
- canonical reference identity
- exterior/grid metadata
- static placement
- scale
- model handle/bounds

Reference rotation is reproduced directly with GLM angle-axis composition. The static path no longer depends on `Misc::Convert::makeOsgQuat`.

### Production routing

`V4SceneRenderLifecycle` now delegates normal static mutation to `StaticWorldService`.

It no longer directly owns:
- exterior population cell creation
- exterior population placement upsert
- interior static instance upsert
- static representation retirement

The old `makeV4StaticInstanceSource` adapter has been removed.

This does not rename/remove the broader V4 lifecycle yet because lights, animated non-actors and actors remain transitional work belonging to later phases.

## P2A acceptance gate

Before moving to terrain:
- Phase 0 architecture boundary remains green.
- Phase 1 NIF compiler boundary remains green.
- Phase 2 no-OSG static source contract is green.
- StaticWorldService smoke verifies interior publication, migration to exterior population, flush, removal and cell retirement.
- No Windows build is required for P2A alone unless the focused gate exposes a source ambiguity.

## P2B next — terrain

Audit and move `v4terrainsource` behind an explicit VulkanMW native terrain source boundary:
- authoritative land data -> TerrainChunkSource
- no OSG drawable/scene ownership
- bounded preparation service retained
- terrain residency/delta publication retained
- no legacy rendering preload required for Vulkan

## P2C next — groundcover

Groundcover should share the data-oriented static population form:
- canonical model handles through NifAssetService
- cell/chunk grouped placements
- Groundcover semantic flag
- retained VSG population residency
- distance/visibility decisions backend-private
- no per-placement OSG node ownership

## Phase 2 closeout target

Representative interior/exterior routes after warmup:
- normal static OSG capture = 0
- normal terrain OSG rendering ownership = 0
- normal groundcover OSG rendering ownership = 0
- steady static model realization = 0 when assets are unchanged
- cell/static add/move/remove semantics remain stable
- OpenGL control remains unchanged
- no performance promotion until matched runtime evidence exists
