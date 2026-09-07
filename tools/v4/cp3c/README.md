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
```

The second target requires GLM include paths in the compiler environment.
