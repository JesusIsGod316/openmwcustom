# V4 CP4 Exterior Execution Audit

## Authority and intent

This audit applies the Shared Project Context Archive through the final CP3E Windows checkpoint and the CP4 guidance in
`evt.v4.0.renderer_execution_guideline.audit.076`. The staged CP4 plan is a routing guide, not a frozen implementation
law. Changes are allowed when evidence shows a safer or more direct route to the project's Vulkan performance and
compatibility goals.

CP4 remains architecture-complete but feature-scope bounded. Stable semantic identities, data flow, ownership, and safe
retirement are established before advanced algorithms. The 8 GB VRAM target remains a hard constraint, and the OpenGL
route remains the independent control path.

## Audited stage boundaries

- **CP4A - Exterior execution foundation:** stable view/target/pass identities, explicit offscreen and presentation
  dependencies, generation-safe terrain publication, shared backend residency, and one authoritative exterior LAND cell
  rendered through the VSG/Vulkan route.
- **CP4B - Terrain streaming:** widen the one-cell producer into an active and predicted chunk set with LOD, asynchronous
  preparation, bounded publication, persistent GPU residency, and completion-safe retirement.
- **CP4C - Static world and groundcover:** publish data-oriented populations. Do not scale the CP3 individually
  authoritative node topology to exterior density. Evaluate CPU-built multidraw or indirect submission only against
  measured complete-world bottlenecks.
- **CP4D - Environment, lighting, and shadows:** route sky, weather, fog, sun, exterior lights, and shadow views through
  the common view system and shared resident scene.
- **CP4E - Water and auxiliary views:** use the same target/pass architecture for reflection, refraction, underwater,
  maps, previews, and target reuse.
- **CP4F - Complete exterior closeout:** effects, traversal, cell churn, interior/exterior transitions, save/load,
  resize, and modded soak. Performance promotion remains evidence-led and belongs to the later profiling program.

These boundaries may move when implementation evidence warrants it, but semantic ownership may not move into VSG nodes
and unsupported behavior must fail closed instead of silently falling back inside an explicitly selected Vulkan session.

## CP4A implementation checkpoint

The first local CP4A checkpoint is intentionally larger than a contract-only slice:

- `FrameRenderState` carries stable view, render-target, and render-pass identities, validates offscreen-to-present
  dependencies, and rejects duplicate, unknown, cyclic/forward, or invalid presentation routes.
- The current single-view VSG host accepts only the route it actually executes. Auxiliary routes are represented and
  tested but remain fail-closed until their backend realization is implemented.
- `TerrainChunkProducer` publishes one generation-safe neutral terrain mesh/material/model/chunk/instance set atomically,
  replaces it on cell movement, retires it on interior transition, and recovers across world epochs.
- The OpenMW adapter obtains LOD0 LAND geometry from authoritative `TerrainStorage` using temporary OSG scratch arrays;
  only neutral mesh data crosses into RenderCore. The existing terrain diamond topology is preserved.
- The bridge avoids rebuilding unchanged LAND every frame and feeds the terrain instance through the existing persistent
  static residency and completion-safe retirement path.
- Exterior frame state now carries authoritative ambient, fog, sun direction, and sun colors. Sky/weather remain disabled
  until CP4D; water and underwater remain disabled/fail-closed until CP4E.
- Terrain layer textures are not part of this checkpoint. Vertex color provides the first visible LAND surface while the
  permanent texture and streaming design is built in CP4B.

## CP4B terrain-streaming foundation in progress

The first CP4B implementation widens CP4A without publishing a separate test artifact:

- `TerrainChunkProducer` owns a deterministic active set and atomically retains, adds, and retires LAND chunks while
  rejecting duplicate semantic addresses and mixed-worldspace batches.
- `TerrainPreparationService` owns one coarse background lane. New desired sets replace queued work, stale generations
  are abandoned between chunks, unchanged prepared chunks are reused, and exceptions or malformed output remain
  isolated from frame publication.
- The Vulkan application route keeps the current exterior cell correctness-critical, then prepares a 3-by-3 LOD0 LAND
  neighborhood without a same-frame queue-and-wait dependency. Completed immutable sets publish atomically.
- Movement within the resident neighborhood reuses prepared meshes. A teleport or first exterior entry synchronously
  establishes only the required current cell, after which neighborhood expansion resumes asynchronously.
- Cell-to-cell movement retains a three-cell look-ahead row in the dominant travel direction; the prediction persists
  while stationary and resets across worldspace or interior transitions, avoiding per-frame request churn.
- Interior transitions retire the resident terrain set. Shutdown joins the preparation worker before world-owned terrain
  storage is released.

This is a safe streaming foundation, not CP4B completion. Distance LOD rings, seam stitching, bounded GPU admission, and
measured residency budgets remain before the first consolidated CP4 artifact.

## Validation gate

Before publication, this checkpoint requires:

1. strict standalone MSVC terrain and frame-route contract tests;
2. pinned VSG 1.1.15/vsgXchange 1.1.13 conformance tests;
3. OpenMW engine-facing semantic source compilation;
4. complete V4 Vulkan runtime compilation;
5. complete `openmw.exe` link;
6. the full component test suite; and
7. source-route, formatting, and diff checks.

Passing this local gate proves implementation/build integrity only. It does not prove visual correctness, runtime
playability, VRAM behavior, or performance. Those require a deliberately batched Windows artifact and user testing.
