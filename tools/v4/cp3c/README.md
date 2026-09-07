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
```

The RenderCore targets require GLM include paths in the compiler environment.
