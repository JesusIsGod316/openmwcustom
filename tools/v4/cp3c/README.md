# V4.0 CP3C production runtime integration

CP3C connects the accepted CP3B neutral static-NIF path to a production Vulkan
runtime. The implementation is deliberately incremental, but the compatibility
boundary is not: Vulkan must not become the automatic backend until content,
actors, terrain, environment, UI/composition, post-processing, shader mods,
multiview, auxiliary views, gameplay/save/Lua integration, and configuration/
content discovery are qualified.

## First ownership batch

The first source audit found two blockers to a correct production consumer:

1. `RenderWorld` did not expose deterministic read-only enumeration of live
   instances/chunks/lights/resources.
2. `InstanceRecord` had no local monotonic revision, so a persistent backend
   could not distinguish current placement from stale asynchronous or resident
   work without invalidating the entire world on every unrelated update.

This batch adds the missing read surface and instance revision contract,
`StaticWorldPlan`/`StaticInstancePlan`, double-precision placement at the backend
boundary, explicit CP3D/CP4 deferral accounting, and backend-local submitted vs.
completed frame lifetime primitives. These are inputs to the production VSG host;
they do not claim that a standalone static render is full OpenMW compatibility.

The persistent residency layer stages revision-keyed upserts separately from
commit, so shader/pipeline/texture compilation can finish before the live root is
changed and stale asynchronous work is rejected. The VSG 1.1.15 submission bridge
uses the viewer's per-task ring fences for completion, avoiding device-wide idle
during normal scene mutation and streaming.

The bridge is source-checked against VSG 1.1.15 commit
`599a8c5c61cbb993079261b2ada99bd843badf5b`, the exact dereferenced upstream
tag identity used by the CP3C gate.

The startup-selection slice adds canonical `auto`, `opengl`, and `vulkan`
configuration values plus a strict/fallback control. The production executable
continues to advertise only OpenGL until its distinct Vulkan bootstrap and real
semantic producer are connected. `Auto` also fails closed rather than selecting
an unqualified Vulkan-only build, and fallback from OpenGL to Vulkan is permitted
only after the complete automatic-compatibility mask passes.
Startup policy is isolated in the GLM-free `backendselection.hpp`, so wiring the
selector into the existing executable does not add semantic-world math or modern
backend dependencies to the OpenGL build.

The next bootstrap slice adds a backend-private `VsgRuntimeBootstrap` that owns
a distinct SDL Vulkan window without entering the established OSG/OpenGL window
path. Its teardown order keeps the SDL native window alive until the runtime has
waited for its submissions and the VSG surface/swapchain objects are gone. The
backend-neutral `SingleViewFrameProducer` binds each immutable frame to the
current world revision and invalidates temporal history on world-epoch, extent,
or explicit continuity changes. These sources remain build-gated and do not make
Vulkan eligible for `Auto`.

The active-cell slice adds a shared-sequence semantic publisher for loaded cells
and static references. It preserves stable instance handles across transform
updates and cell moves, retires a cell and all of its owned references atomically,
and discards all source bindings when the world epoch changes. NIF asset
publication now allocates from the same publisher sequence instead of assuming it
is always the first producer. `MWRender::v4semanticsource` supplies the production
game-side conversions from stable cell/`RefNum` identity, authoritative placement,
and the live gameplay camera into RenderCore's reversed-Z/down-Y contract. The
Vulkan integration target compiles this GLM-backed adapter; the legacy OpenGL
target does not acquire a new renderer-math dependency before the explicit
production Vulkan engine path is enabled.

`StaticModelCache` retains the complete published binding for each normalized,
winning VFS model path. Repeated cell references reuse the same neutral resources;
a different content identity for the same path during one world epoch is rejected
instead of mixing mod versions. A new world epoch clears the cache and permits a
generation-safe republish. This is the session-level ownership baseline for later
CP4 paging and CP7 residency budgets.

`VsgSemanticSession` is the build-gated engine ownership aggregate: it keeps the
world, common publisher, model cache, active-cell producer, frame-history
producer, distinct SDL/Vulkan bootstrap, and runtime renderer under one lifetime.
Reset waits for GPU completion before advancing the world epoch, and destruction
releases the Vulkan runtime before any logical source bindings. It is compiled in
the real-NIF Vulkan target but is not linked into or selected by the normal
OpenGL engine yet.

The production world now exposes an optional, owned `SceneRenderLifecycle`
boundary at the exact `MWWorld::Scene` load/unload and object mutation points.
It receives authoritative `CellStore`/`Ptr` state, is injectable through
`MWWorld::World::init`, and is null on the unchanged OpenGL route. Activation,
addition, and mutation may fail an explicitly selected renderer; retirement and
reset are `noexcept` so unload and shutdown cannot be blocked. This is the
engine-facing seam for `VsgSemanticSession`, not an OSG-node mirroring path.
The build-gated `V4SceneRenderLifecycle` implementation resolves each eligible
static `Ptr` through its corrected winning VFS model path, reuses or publishes
that NIF through `StaticModelCache`, and upserts the reference into the active
cell producer. Missing content, translation failures, stale handles, and
publication failures stop the explicit route instead of silently dropping an
object. Cell and object retirement remain cleanup-safe and retain a health
diagnostic for the engine loop.

`V4EngineRenderBridge` now provides the build-gated application factory around
that session. It creates the texture resolver from OpenMW's already-registered
winning VFS, hands exactly one lifecycle observer to the world before cell
activation, and converts the authoritative game camera plus the current SDL
pixel extent into a `SingleViewFrameInput`. The lifecycle shares ownership of
the session, preventing a future application-member reorder from destroying GPU
state while the world can still issue retirement callbacks. Resize sampling is
kept at the SDL pixel boundary, so render and output extents are explicit and do
not depend on OSG camera or graphics-window state. The normal executable still
does not link or select this bridge; the remaining engine branch must instantiate
it before `World::init` and drive it from a Vulkan-specific loop after the
non-rendering subsystems have been separated from the OSG host.

Scene-source failures also cross that ownership boundary through a shared,
sticky `V4RenderRouteStatus`. If cell, model, or reference publication fails,
the first diagnostic is retained and the application bridge rejects every later
frame even when an outer engine catch continues simulation. Cleanup failures do
not overwrite the original cause. This prevents an explicitly selected Vulkan
route from presenting a believable but incomplete mod-resolved scene.

Engine startup now establishes the authoritative VFS immediately after the
content encoder and before constructing the OSG viewer/window. The OpenGL route
then continues through its existing preparation path unchanged. This ordering
is a required seam for the distinct Vulkan branch: its session can consume the
same archive precedence, loose-file overrides, and normalized winning paths
without creating an OpenGL graphics context first.

The immutable frame environment now carries explicit fog enablement, planar vs.
radial distance, and linear vs. exponential falloff. Validation rejects unknown
modes and enabled fog with an invalid range. Backends no longer guess whether
fog is active from its numeric endpoints; this preserves the established OpenMW
shader choices and gives later post-processing/depth reconstruction one stable
semantic contract. The CP3C host still fails closed when fog is enabled until
the compatibility shader consumes this state.

`ActiveCellProducer` now owns stable, source-identified local-light bindings in
addition to static instances. Light changes retain their handles and advance
resource revisions; cross-cell moves update semantic ownership; cell unload
retires owned lights and references in the same atomic batch; world-epoch reset
clears every source binding. This is the neutral ownership needed for genuine
static interiors, while flicker/pulse/controller realization remains explicitly
deferred rather than being flattened into an incorrect constant light.

The build-gated OpenMW adapter now recognizes ESM3 and ESM4 lights before the
generic animated-object exclusion. It publishes their authoritative reference
identity and placement, negative-light colors, off-default state, modulation
class, and dynamic/carry/spot flags. Attenuation is resolved by the same shared
fallback calculation used by the OpenGL light manager, avoiding backend-specific
brightness drift from duplicated settings logic. Animated light-model attachment
and GPU light/modulation realization are still fail-closed: the semantics are
retained, but the VSG host will not present an incomplete lit scene.

`LocalLightWorldPlan` is the deterministic backend discovery boundary for the
next realization slice. It snapshots generation-safe handles, revisions, and
the complete neutral records, rejects stale plans after an update, and counts
modulated, spot, and disabled categories explicitly. A constant-point-only
implementation therefore cannot accidentally claim compatibility with a scene
whose authored behavior it would lose.

The first local-light realization extends VSG's own per-view descriptor owner
with an OpenMW storage-buffer binding. It does not translate point lights into
VSG's inverse-square representation: the compatibility shader consumes the
authored diffuse/ambient/specular channels, negative colors, enabled/actor fade,
and exact constant/linear/quadratic attenuation directly. Classic versus
non-classic radius behavior comes from immutable frame state. The buffer remains
bounded at 4096 lights to prevent an accidental unbounded fragment loop, while
its std430 layout can feed later clustered selection without a record or shader
data-layout rewrite. Modulated, spot, overflow, and clustered cases still fail
closed rather than degrading silently.

Interior distance fog now uses a second frame-safe OpenMW view descriptor. The
legacy shader matches the established planar/radial distance choice and
linear/exponential equations, and material packing preserves `NiFogProperty`
disablement plus color/depth overrides. Source-alpha/one materials use the
legacy additive rule (fade toward black); other materials blend toward fog
color. Sky blending remains out of scope while the host rejects sky rendering.

## Cheap local checks

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/frame-lifetime-smoke.cpp -o /tmp/v4-cp3c-frame-lifetime-smoke
/tmp/v4-cp3c-frame-lifetime-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/static-world-plan-smoke.cpp -o /tmp/v4-cp3c-static-world-plan-smoke
/tmp/v4-cp3c-static-world-plan-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/backend-selection-smoke.cpp -o /tmp/v4-cp3c-backend-selection-smoke
/tmp/v4-cp3c-backend-selection-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/frame-producer-smoke.cpp -o /tmp/v4-cp3c-frame-producer-smoke
/tmp/v4-cp3c-frame-producer-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/active-cell-producer-smoke.cpp -o /tmp/v4-cp3c-active-cell-producer-smoke
/tmp/v4-cp3c-active-cell-producer-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/static-model-cache-smoke.cpp -o /tmp/v4-cp3c-static-model-cache-smoke
/tmp/v4-cp3c-static-model-cache-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/scene-render-lifecycle-smoke.cpp -o /tmp/v4-cp3c-scene-render-lifecycle-smoke
/tmp/v4-cp3c-scene-render-lifecycle-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/render-route-status-smoke.cpp -o /tmp/v4-cp3c-render-route-status-smoke
/tmp/v4-cp3c-render-route-status-smoke
```

The RenderCore targets require GLM include paths in the compiler environment.
