# CP4F native preview: disabled-shadow texture candidate repair

Base: `f29ef787cf07ae1e525f0967ff26947b1d50e048`.

A new user screenshot reports a fatal error from native character preview after
skipping the New Game intro: the evaluated texture has no recoverable winning-VFS
image identity. The accompanying capture is from the earlier `e26a825f67` build,
not the new run. It must not be used to attribute this failure or report new
memory/performance results. The current capture remains required for attribution.

## Reproducible source defect

When shadows are enabled, CharacterPreview calls
`ShadowManager::disableShadowsForStateSet`. That routine binds an engine-generated
1x1 float depth image, with positive infinity and ALWAYS shadow comparison, to
reserved shadow units. It intentionally has no file name. Preview capture only
recognized the separate image-less unit-7 depth sentinel. Its authored-texture
check therefore rejected the image-backed shadow sentinel with the reported error.

The candidate shares the existing disabled-shadow texture construction in a small
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

These are focused source/capture results, not a Windows production link, pixel
comparison or reproduction with the user's current modded capture. The candidate
is staged separately; no new full Windows build is justified solely by the old
capture. All prior rendering, memory-pressure, terrain, actor-plan, UV and save
isolation paths are retained. Await the current capture before claiming this
specific user failure resolved or choosing the next combined build.
