# CP4F combined rendering repair

Base: `570289804810174724e7e534600f0831d0e9ae67`. This checkpoint retains the
memory-pressure policy, legacy-terrain bypass, corrected packaged starter,
evaluated-UV sharing, and actor-plan reuse. Source and controlled-pixel results
are not acceptance of the user's modded gameplay or a performance measurement.

## Character preview: capture, isolated view, and publication

CharacterPreview exposes a main-thread snapshot after its normal update-only
traversal: authored evaluated geometry, its view/viewport, lighting, and redraw
revision. Capture does not advance animation callbacks. Preview targets use a
separate view namespace and an isolated scene, never the main-world scene root.
A native image alias is published to MyGUI after successful frame submission;
closed/replaced views retain old resources through their last GPU use.

The bridge preserves separate color/alpha blend factors, the preview's force-
opaque rules, premultiplied target colors, widget alpha, and the existing RTT
facade's vertical-coordinate convention. Ordinary UI images are converted from
straight to premultiplied output in the UI shader; their resulting composition
is unchanged. Additive color with zero alpha remains visible. Only the engine's
named, always-pass preview depth sentinel is omitted from texture capture.

## Native sky layers, not a different constant background

The gameplay SkyManager remains authoritative for weather, time, textures,
geometry, switches, transforms, moon phase/masks, cloud movement and opacity.
Its atmosphere/night/cloud/moon/sun passes are captured using the winning VFS
texture identities and published to each main/reflection/refraction view in a
camera-relative frame. Immutable geometry is shared across uniform changes;
GPU-completed resource versions are reused with a bounded three-frame pool.
The placeholder backdrop is suppressed only when native sky data is supplied.

Sun occlusion-query/glare passes 5 and 6 are explicitly counted/deferred and
warned about, not represented as completed native occlusion. Precipitation,
full weather parity, and every auxiliary-view combination still require more
work and runtime tests. Unsupported ordinary sky pass semantics fail explicitly.

## Water: real refraction depth and composition inputs

The refraction target can retain a sampled D32 depth attachment. Store/layout,
format support and render-pass dependencies are explicit. The shared clear
helper now classifies depth by attachment format, not final image layout:
VSG's original layout-based inference mistook read-only sampled depth for color.
Color and depth clear values are initialized after depthFormat is assigned.

Water samples the real refraction depth, reconstructs reversed-Z distances, and
uses depth-dependent absorption and shoreline fade. Native fog/water colors,
light, camera side, projection and Fresnel inputs remain distinct. Reflection
and refraction now also receive native sky layers. This is not a global tint,
gamma adjustment or replacement constant clear color. Native rain/collision
ripples, complete shadow response and all shoreline cases are not finished.

## Material normal mapping and host uniform alignment

Two-channel normal maps reconstruct Z for object normal-map roles, not only
terrain. The derivative tangent frame includes the signed UV Jacobian, including
mirrored charts and the Vulkan Y convention; degenerate derivatives fall back
to the geometric normal. No global color swizzle, saturation or texture-quality
change is used. This fixes proven normal-mapping defects but does not identify
or prove repair of every cyan/green surface in the user's mod stack. Authored
per-vertex tangent input remains outside this standard vertex contract.

An optimized pixel fixture additionally reproduced a host allocation crash:
VSG 1.1.15 IntrusiveAllocator's default object alignment is 8 bytes, while three
custom uniform Value payloads had requested 16-byte alignment. Their redundant
host over-alignment is removed. Standard-layout, exact per-field offsets and
160/32/32-byte upload sizes are checked independently, preserving GPU std140.
Mixed-allocation tests verify the current contract; the old headers fail it.

## Same-executable controls

Presence semantics match existing CP4 controls: unset selects the repair; even
`0` as a value selects the legacy comparison. Do not change controls mid-run.

- `OPENMW_V4_LEGACY_PREVIEW_CONTROL`: do not publish native preview targets.
- `OPENMW_V4_LEGACY_SKY_CONTROL`: retain the previous placeholder sky route.
- `OPENMW_V4_LEGACY_WATER_OPTICS_CONTROL`: retain pre-repair water optics.
- `OPENMW_V4_LEGACY_NORMAL_MAPPING_CONTROL`: retain pre-repair normal decoding/frame.

All earlier actor, effect, static, water composition/procedural, memory-retention
and terrain controls remain. Lua workers, save semantics, content lists, normal
cache profile inputs, simulation and required LAND/physics are unchanged.

## Validation surface

`rendering-tests/CMakeLists.txt` builds actual production renderer sources with
pinned VSG and a real Vulkan implementation. `rendering-pixels all` runs both
retained water/terrain/GUI fixtures and native sky, preview, optics, and normal-
mapping tests. Missing devices or a requested missing validation layer are
errors, not skipped passes. Execution logs must be checked for Vulkan validation
errors in addition to the process result. Pixel tests are explicitly run, not
silently attempted on a Windows runner with no suitable Vulkan driver.

CPU fixtures cover 14 evaluated-capture/frame-contract cases and 256 mixed
uniform allocations. ASan/UBSan covers the capture fixture; only an unrelated
unchanged sceneutil/util.cpp vptr check is excluded to avoid unused full-engine
vtable linkage. New capture translation units retain vptr instrumentation.
An original-header control rejects the unsafe uniform alignment. Legacy normal
and water controls deliberately fail the new pixel assertions.

The prior memory/terrain Windows gate reached the production EXE, CTests, install
and successful packaged-starter preparation. Its final two config-fixture
assertions compared Windows `RUNNER~1` and canonical `runneradmin` spellings.
This batch canonicalizes the fixture's temporary root and adds an explicit alias
case. It does not remove the package check or weaken source/config isolation.

The full MSVC RelWithDebInfo production target and all packaged CPU tests remain
required. User acceptance requires the isolated New Game ship/Seyda Neen/Excise
route, preview changes and water/sky/material screenshots. No normal saves,
claimed FPS improvement, complete visual parity or resource-soak acceptance is
part of the controlled test result.
