# Phase 9 root-cause / culling audit

Base: `bb4876a6ea287745c8f535f5525c5381a631a24c` (tree
`78ac535aa941cb703707a42fc946910fc0b03c61`). This is a source audit and
implementation description, not a new gameplay benchmark or promotion.

## Existing coverage and disposition

`apps/openmw/mwrender/occlusionculling.cpp` already has the main scene callback,
whole-cell terrain rejection, a large-occluder then small-object cell pass,
paged-object coarse tests/rasterization, and coarse groundcover rejection.
`objects.cpp` installs cell callbacks. `objectpaging.cpp` and `groundcover.cpp`
install the existing chunk callbacks. Groundcover additionally has its P8G3
hierarchy and the P8G4 traversal-local input reuse in `groundcoverbatch.hpp`.
Wind-expanded bounds, shader-state compatibility and fail-open fallbacks matter.

**Do not add a second broad opaque-world hierarchy in this checkpoint.** It would
largely duplicate existing rejection. A future new population must show remaining
avoidable leaf work and replace that work, not add a parallel scene representation.
Do not re-enable dormant V3.22/23/24 MSOC experiments or loosen occluder acceptance.
Do not use main-camera visibility to discard a potentially visible shadow caster
or a reflection contributor. Door `skipOcclusion`, exact callback order/masks,
receiver/caster separation and existing shadow policy are unchanged.

### Implemented narrow reuse

The paged and coarse callbacks still invert the same camera view repeatedly on
the ordinary path. The new optional `CullViewCache::Scope` starts inside the
existing main-scene traversal and caches this calculation only. Each lookup
checks both camera identity and the exact current matrix. A mismatch, singular
matrix or absent scope falls back to the prior inverse calculation. Nested scopes
restore their parent and storage is thread-local. No visibility answer, object
bounds, occluder mesh, lifecycle identity or GPU resource is cached by this change.

This reduces redundant CPU preparation when it engages; it does not itself
reduce triangles or establish an explanation for 40-60 ms draw stalls. The fixed
HIER-CULL/MSOC foundation already handles those visibility reductions.

## Exact trace-installation defect

The published tracer was installed by `PostProcessor` after scene setup. Engine
`createWindow()` calls `Viewer::realize()` earlier, which can start draw threads.
The tracer correctly refused attachment to a running viewer, yielding zero live
visitors in both user traces. Tests previously stopped workers before installation
and therefore did not exercise the real startup ordering.

Install the two mono scene-view visitors before realization. Attach selected GL
dispatch wrappers through a graphics realization operation. Do not install in the
later PostProcessor. Unknown visitor types, late installation or unsupported
stereo fail explicitly for TRACE rather than replacing custom culling silently.
The new startup fixture realizes a DrawThreadPerContext viewer, replaces scene
data afterward, renders frames and verifies nonzero actual leaf/API calls. It
also rejects the old late-install pattern. Settings restoration/raw ZIP happen
regardless of final trace validity; status/aggregate loss remains visible.

## What the trace can and cannot attribute

The leaf split preserves OSG 3.6.5 state transitions and dynamic completion:
matrices, StateGraph/StateSet application, drawable dispatch, completion. All
instrumented calls feed frame aggregates; slow leaves retain owned metadata.
Selected OSG extension-dispatch API calls retain CPU duration, frame/context
and leaf phase. Wrappers preserve calling convention, arguments and return value,
are idempotent, and are restored after graphics workers stop. They add no new
GL waits, frame readbacks, flushes or synchronous GPU-timer requests.

This is **not complete OpenGL interception**: core calls inside a prebuilt OSG
library, render-stage setup and driver internals are not individually split.
Large enclosing time with little instrumented API time is an explicit uncovered
bucket, not proof that the driver or any particular unmeasured API caused it.
CPU and GPU scopes overlap; `dynamic_draw_wait` is a wait location, not a GPU fence.
Compare current and preceding frame identities without relabeling asynchronously
read GPU results. Preserve severe frames and report diagnostic observer overhead.

## Safe shared actor preparation

The empty rig/morph `compileGLObjects()` does NOT mean material programs and
textures are never precompiled: OSG `StateToCompile` collects those independently.
Reference: OpenSceneGraph-3.6.5 `src/osgUtil/IncrementalCompileOperation.cpp`,
`StateToCompile::apply(Drawable&)`, `apply(StateSet&)`, and `CompileDrawableOp`.

The new optional drawable hook only compiles shared STATIC vertex/index storage
that live rig/morph geometries reuse. It skips live/private position streams and
rig normals/tangents, rejects DYNAMIC backing data, respects OSG revision state,
bounds work to 2 MiB per geometry call, and leaves unsupported resources to the
normal draw path. Existing ICO scheduling and independent program/texture
preparation remain intact. No source-array mutation, forced orphaning, skinning
on another worker, extra graphics context or GPU fence is introduced.

The old HITCH1 experiment remains advanced/off in primary modes. Its prior
resubmitted-byte counter is not a measurement of incremental bytes versus control.

## Validation and acceptance

Native fixtures check exact inverse parity (including nested cameras, changed
matrices, singular fallback and independent threads); real shared vertex/index
buffer data and private-buffer noninterference; GL dispatch ABI/returns/arguments,
a deliberately delayed forwarded call with correct frame/phase; and actual
pre-realization startup. Post-exit CSV verification checks aggregates and live
coverage. Existing single/threaded pixel parity and temporal tests remain.

Compile real culling plus the existing six integration translation units, run
native and Address/UndefinedBehavior sanitizer fixtures, retained source/shader
checks and real Windows PowerShell 5.1 archive/trace-health tests. Full Windows
engine compilation and installed package gates remain mandatory. No test substitutes
for the user's hardware: actual hitch attribution needs a new live trace.

Temporal camera/static RG16F integration is retained byte-for-byte. Scene jitter
is off. Dense dynamic motion and GL/Vulkan/NVIDIA runtime integration remain future
work; no working DLSS, DLAA, frame generation or guaranteed hitch cure is claimed.
