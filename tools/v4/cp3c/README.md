# V4.0 CP3C production runtime integration

CP3C connects the accepted CP3B neutral static-NIF path to a production Vulkan
runtime. The implementation is deliberately incremental, but the compatibility
boundary is not: Vulkan must not become the automatic backend until content,
actors, terrain, environment, UI/composition, post-processing, shader mods,
multiview, auxiliary views, and gameplay/save/Lua integration are qualified.

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

## Cheap local checks

```sh
g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/frame-lifetime-smoke.cpp -o /tmp/v4-cp3c-frame-lifetime-smoke
/tmp/v4-cp3c-frame-lifetime-smoke

g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -I. \
  tools/v4/cp3c/static-world-plan-smoke.cpp -o /tmp/v4-cp3c-static-world-plan-smoke
/tmp/v4-cp3c-static-world-plan-smoke
```

The second target requires GLM include paths in the compiler environment.

