# CP4F native preview: disabled-shadow texture repair

Base: `f29ef787cf07ae1e525f0967ff26947b1d50e048`.
Validated candidate: `353160c422bf8ed3656948429d17760615914990`.
Independent focused validation: run `35805401119`, all steps successful.

## Current capture and attribution

The matching f29 capture has now been supplied and inspected. Its manifest
identifies the f29 executable, Vulkan, Standard diagnostics, isolated user-data,
no copied regular saves, and unchanged original configuration hashes. The log
reserves shadow texture units `d..f`, records normal intro-skip decoder shutdown,
and then reports the fatal native character-preview texture-identity error.
The LuaWorker destruction warning follows the renderer failure, not vice versa.
There is no recorded exterior transition or completed preview publication.

This supports promoting the reproduced disabled-shadow repair. The f29 error
omits the offending texture unit and drawable, so exact per-texture attribution
is still not directly recorded. Do not claim a successful user-runtime repair
until the replacement executable passes a fresh isolated New Game test.
The previously supplied e26 capture remains separate historical evidence.

## Reproducible source defect

When shadows are enabled, CharacterPreview calls
`ShadowManager::disableShadowsForStateSet`. That routine binds an engine-generated
1x1 float depth image, with positive infinity and ALWAYS shadow comparison, to
reserved shadow units. It intentionally has no file name. Preview capture only
recognized the separate image-less unit-7 depth sentinel. Its authored-texture
check therefore rejected the image-backed shadow sentinel with the reported error.

The repair shares the existing disabled-shadow texture construction in a small
SceneUtil factory, adds an engine-owned name, and recognizes that exact name,
image dimensions, format, data type, value and comparison state in preview capture
only. The OpenGL texture bytes, wrap and comparison behavior remain unchanged.
World capture, arbitrary generated textures, modified sentinels and actual file-
backed images remain outside the exception. Authored diffuse/normal/glow textures
and UV sharing are retained; no failed VFS lookup is converted into success.

Unknown texture failures now report the unit, name, file name, dimensions, format,
comparison state, capture identity and drawable name/type. This helps distinguish
another generated dependency from genuinely missing authored image provenance.

## Validation and limits

The expanded real-OSG CPU fixture contains 20 cases. Against the original f29
capture header it passes 18/20: the disabled-shadow preview case produces the
exact reported error, and the detailed-error assertion also fails. The candidate
passes 20/20, including an ASan/UBSan run with leak detection and halt-on-error.
The retained UV fixture passes 20/20. Actual shadow.cpp compiles with the Vulkan
macro both off and on; the Vulkan preview bridge syntax check also passes.
The promoted implementation and fixture blobs are identical to those validated
in run 35805401119; this document records the later capture and promotion decision.

A new full Windows gate is justified because the matching current-build capture
now demonstrates a startup blocker preventing evaluation of the combined repairs.
It must retain the production link, expanded capture CTest, existing ownership,
UV, uniform and recorder tests, packaged-starter isolation, and package checks.
No separate old-build retest, global shadow disable, or blanket texture omission
is required. All prior rendering, memory-pressure, terrain, actor-plan, UV and
save-isolation paths are retained.

The short startup capture is not a memory/performance comparison: it never reaches
the exterior route that previously exhausted memory. Mod-script errors and the
BetterBars DDS decode/placeholder warning are separately recorded nonfatal issues;
this patch does not repair them or attribute them to the preview sentinel.
No game or regular save is accessed by source promotion and build validation.
