# CP4F depth-only shadow view lighting repair

Base: `3b581b011e411b8633065b76ebd288fadc04cec7`.
The matching Standard Vulkan startup capture reports `unshadowed view light data
exceeds its compiled buffer capacity`. It does not record the failing view ID
or counts. The source reproducer below establishes a specific defect with the
same fatal; successful gameplay on the replacement remains unverified.

## Cause and repair

The previous lit-preview repair dispatched light packing whenever a view lacked
`RECORD_SHADOW_MAPS`. That condition includes VSG's generated depth-only shadow
cameras, which carry only `INHERIT_VIEWPOINT`, not `RECORD_LIGHTS`.
VSG initializes these cameras with one vec4 for zero light counts. Its record
traversal still encounters the parent's ambient/directional light nodes. Our
packer then asks for five vec4s and correctly rejects that one-vec4 allocation.

Require `RECORD_LIGHTS` as well as the absence of `RECORD_SHADOW_MAPS` before
calling the custom unshadowed packer. Other views retain VSG's original route.
Depth passes do not need shading lights. Do not enlarge their buffers, skip
shadow recording, truncate lights, weaken the capacity guard, or switch lit
previews back to the broken black-output path. Environment/local-light bindings
and existing alpha-tested shadow descriptor contracts remain intact.
A genuine capacity mismatch now reports view ID, feature flags, required and
available vec4 counts, and the four light-type counts before any write occurs.

## Regression coverage

`rendering-view-features-tests.cpp` initializes actual production VSG states and
uses real RecordTraversal light collection. It covers no-feature/inherited depth
views, all three generated cascade cameras, ordinary/inherited lighting-only
views, mixed light types, exact capacity, actual overflow without partial writes,
error details, changed/removed lights, stable buffer identity, and repeated init.
The original implementation passes 7/11 and fails the three depth-route cases and
error-detail assertion. The repaired implementation passes 11/11 locally,
including ASan/UBSan with leak detection and halt-on-error. Pinned VSG itself is
not sanitizer-instrumented; the changed production state and fixture are.

The expanded pixel suite adds a shadow-enabled scene with three real cascades,
multiple submitted frames, inherited parent light collection, original one-vec4
shadow buffers, and preserved main-world lighting. It also runs the existing lit
preview checks, rather than treating absence of an exception as visual success.
The first expanded pixel attempt exposed a fixture setup error: its view-only
compiler prepared the new drawable for the main camera but not shadow cameras.
A debugger backtrace located the failure in BindGraphicsPipeline::record during
the generated shadow traversal, before the capacity check. The fixture now uses
the same all-live-view compilation helper as production world publication and
checks pipeline realization for the main camera and each shadow before recording.
No production compilation contract is removed or bypassed by this fixture repair.
Pixel results require execution on the provisioned independent Vulkan runner;
local syntax and CPU tests alone do not establish that result.

Register the CPU regression in both focused CMake and the production OpenMW
CTest surface. The next Windows gate must build and execute it. Retain the
existing shader pixel checks, old normal/water/lighting/specular controls,
evaluated-bump, preview sentinel, uniform layout, memory/cache, UV, recorder,
launcher, and production-compilation tests.

No game, normal save, mod, shadow setting, or cache setting is changed by the
source tests. All previous combined repairs remain. The guard, individual GUI
input events, complete preview orientation, water/material parity, and the major
exterior frame cost are not marked fixed by this startup repair.
