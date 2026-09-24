# Persistent object producers: implementation and evidence

2026-09-24. Experimental, default-off implementation; not a runtime promotion.

Worktree: `C:\Users\LSCha\.codex\worktrees\cp4f-material-frame-repair\OpenMW custom Build`.
Branch: `codex/cp4f-material-frame-repair`.
Base HEAD: `efa4f3694fad904b3546e7c846c756403950f0d2`.
Changes are uncommitted and layered on the existing dirty worktree. Existing
repairs were preserved; no reset, commit, or push was performed.

## Implemented boundary

* ObjectAnimation builds a bounded, flat set of node, transform, controller,
  drawable, and texture bindings after NIF loading/controller installation.
  Mesh/material realization occurs on the first evaluated publication, after
  OpenMW has evaluated its controllers. Thereafter unchanged supported draws
  bypass captureGeometry, captureMaterial, and inherited-state discovery.
* Engine movement setters publish revisions. The supported controller contract
  covers keyframe/roll/path/look-at transforms, visibility, material alpha/color,
  UV animation, and texture flips. Composite controller masks include every
  child; an untracked child invalidates native coverage. Ordinary content mods
  do not implement a new notification API.
* Changed material/UV bindings update the affected draw only. Hidden draws retain
  pending changes. New UV snapshots do not mutate retained frames. Engine PAT
  transforms use revisions; supported generic OSG matrix adapters compare the
  local matrix to catch inherited setter calls. Parent changes and absolute
  reference frames are preserved.
* Root/effect replacement resets ownership. Flat weak-pointer, edge, callback
  chain, and state-callback guards detect structural changes. This is not a deep
  per-frame material/vertex scan. Unsupported objects use the existing evaluated
  capture path, scoped to the affected object, rather than being silently omitted.
* Per-asset texture identity slots avoid repeatedly resolving/normalizing the
  same path. An independent load-bound option retains identity until engine
  rebind/reload, explicit cache invalidation, cache lifetime change, or VFS index
  generation change. This matches retained ImageManager loading behavior; it is
  explicitly NOT a live filesystem watcher. The per-frame identity-check control
  remains available.
* The producer has an aggregate 64 MiB estimated ownership budget, 512-node and
  64-level limits, and cycle rejection. Accounting includes the retained mesh
  streams (including both tangent arrays), and acquires/releases the difference
  when a UV layout grows/shrinks. Failed coverage or unload releases its lease/bindings;
  already-published immutable meshes remain alive through existing frame/GPU
  retirement ownership. This budget is not a total-process RSS or total-VRAM cap.
* Only neutral values and privately immutable meshes cross the publication
  boundary. Existing bounded neutral-data/actor workers, per-view VSG culling,
  independent-view recording, and fence-completed resident reuse are retained.
  No worker traverses these live OSG bindings.

## Coverage limits: do not describe this as the completed renderer rewrite

The direct producer currently covers ordinary supported NIF object graphs.
Custom/untracked callbacks, LOD nodes, rigs/morph geometry, particles, procedural
textures, and OSG-native model imports retain existing paths. Keeping an importer
or fallback does not require universal per-frame capture for supported objects.
No model-format, plugin, script, or save serialization format was changed.

Character model/pose publication, equipment assembly, and their existing
invalidation paths were preserved, not replaced by a new all-actor event queue.
The new object producer still emits neutral draw descriptors into the existing
frame stream. Thus the backend still reconciles those descriptors, audits active
pipeline graphs, and records scene graphs each frame. Direct persistent
RenderWorld instance/resource deltas are a remaining stage.

Early preparation rejection covering main, reflection, refraction, map, and
shadow views is NOT newly completed here. Existing conservative per-view draw
culling remains; the main-view-only occlusion path is not used to stop gameplay
or suppress other views. Do not imply that existing late culling eliminates all
capture/preparation work. No F2/Rafael/PBR parity claim is made.

Unsupported by this new producer does not mean newly supported by Vulkan: the
fallback retains the existing backend's limits. Full mod/save/visual compatibility
needs broader runtime coverage, including animated materials, cell transitions,
equipment changes, maps, reflections, shadows, and OSG-native assets.

## Independent controls

Both new paths are OFF for an ordinary launch:

* `OPENMW_V4_NATIVE_OBJECT_PRODUCERS=1`:
  launcher key `architecture-producers`.
* `OPENMW_V4_LOAD_BOUND_TEXTURES=1`:
  launcher key `architecture-texture-bindings`; used by the native producer.

The diagnostic launcher's `all` profile explicitly selects them. The existing
OpenGL route and evaluated-capture controls remain intact. The automated measured
arms disable `architecture-metadata`: the separate metadata batching experiment
has not demonstrated a gain and must not be credited for these results.

## Measured intermediate candidate

Package: `C:\OpenMW-Producer-Test-3`.
Source/executable/shader hashes: `persistent-build-manifest.json` in that package,
plus the per-run manifests. This package precedes the final actual-stream
budget accounting and extra test configurations. Do not label
its numbers as measurements of a later executable.

Same executable, same ordered mod/config chain, private writable config/data,
Seyda Neen start, fixed seed 123456, 1920x1080, vsync off, uncapped, 15-second
warm-up, 30-second sample. All arms ran sequentially with no concurrent builds.
Monotonic wall-clock frame intervals, not clamped simulation delta or GPU timers.
This is not the user's exact route/camera or a visual-parity benchmark.

| Arm | Mean ms | Median ms | p95 ms | Approx. FPS from mean |
| --- | ---: | ---: | ---: | ---: |
| New producers + loaded identities, run 1 | 72.027 | 71.778 | 75.691 | 13.88 |
| New producers + loaded identities, run 2 | 72.671 | 72.200 | 78.391 | 13.76 |
| Same Vulkan exe, those two options disabled | 84.892 | 83.727 | 93.231 | 11.78 |
| Same exe, OpenGL control | 15.768 | 15.389 | 17.535 | 63.42 |

Run directories under that package's Local-Benchmarks:

* `native-01/20260924-024622-gameplay-138504`
* `native-02/20260924-025428-gameplay-135500`
* `control-01/20260924-024953-gameplay-145896`
* `opengl-01/20260924-025200-gameplay-74364`

Every run exited 0 normally and verified the original configuration chain hashes
unchanged. No regular save was opened, copied, or modified. All four logs reported
19 error-level mod/script/config lines; their presence in the OpenGL control means
they are not evidence that this producer newly caused 19 engine failures.
Sparse diagnostic stage pairing can report incomplete sampled stages without a
crash. Those warnings are retained in reports, not erased.

This is about 14-15% lower frame time versus the disabled new paths, but Vulkan is
still about 4.6 times OpenGL's frame time. It does NOT satisfy the performance goal
and does NOT warrant another requested manual playtest or promotion.

Sparse steady-state median evidence from native-01 (inclusive scopes, not additive):

* ~1,653 native draw reuses, zero native mesh builds/material updates per sampled
  frame; ~41 object fallbacks. This confirms actual producer bypass.
* Native binding work ~0.99 ms, identity work ~0.63 ms, draw copy ~1.29 ms.
* Dynamic capture ~13.65 ms, total Vulkan capture ~16.77 ms.
* Backend dynamic realization ~12.23 ms, pipeline audit ~5.14 ms,
  submit/record scope ~10.89 ms. These are CPU envelopes, not GPU timestamps.
* Existing visibility reported ~334 main-view frustum rejections and zero
  occlusion rejections in these sampled frames. No occlusion speedup is claimed.

The identity-only change was motivated by an earlier producer run still spending
~8-9 ms checking texture identities each frame. Loaded bindings removed most of
that measured work; adding workers to unchanged-file polling was not the solution.

## Exact files changed in this pass

New:

* `apps/openmw/mwrender/v4persistentobject.hpp`
* `components/sceneutil/rendermutation.hpp`
* `tools/v4/cp4/NATIVE-OBJECT-PRODUCERS.md`

Modified (these files may also contain preserved changes from earlier passes):

* `apps/openmw/mwrender/animation.cpp`
* `apps/openmw/mwrender/animation.hpp`
* `apps/openmw/mwrender/v4enginerenderbridge.cpp`
* `components/debug/gameplaydiagnostics.hpp`
* `components/nifosg/controller.hpp`
* `components/nifrender/textureidentitycache.hpp`
* `components/sceneutil/nodecallback.hpp`
* `components/sceneutil/positionattitudetransform.hpp`
* `components/sceneutil/statesetupdater.hpp`
* `tools/v4/cp4/gameplay-diagnostics.py`
* `tools/v4/cp4/architecture-benchmark.py`
* `tools/v4/cp4/rendering-capture-tests.cpp`
* `tools/v4/cp4/rendering-tests/CMakeLists.txt`

## Validation

* Final production MSVC Release Vulkan build: PASS, exit 0;
  `build/native-producers-budget-vulkan-build.log`.
* Final production MSVC Release OpenGL build: PASS, exit 0;
  `build/native-producers-budget-opengl-build.log`.
* All fixture targets rebuilt: PASS, exit 0;
  `build/native-producers-final-fixtures-build.log`.
* CTest: 24/24 configurations PASS, including both new producer/texture control
  combinations; `build/native-producers-budget-ctest.log`.
* Capture fixture: 44/44 PASS; `build/native-producers-budget-tests.log`. Includes
  immutable old-frame retention, hidden dirtiness, supported/unknown callbacks,
  movement and absolute transforms, topology replacement, budget/unload,
  same-name texture replacement, cache lifetime and VFS invalidation, and UV
  layout lease growth/shrinkage/budget rejection with old frames still valid.
* Headless Vulkan pixel fixture with validation enabled: PASS, exit 0, no VUID or
  validation-error lines; `build/native-producers-validation-pixels.log`. Covers
  production material, terrain, GUI, preview, native sky, water and three-cascade
  shadow view routing. The final producer-budget correction does not change these
  backend shaders or pixel fixtures.
* Python diagnostic harness: 29/29 PASS; configuration isolation: 17/17 PASS;
  runtime diagnostic parser: 17 PASS, one skipped.
* Runtime blocker contract, generated-output/materialization verifier, and
  `git diff --check`: PASS.

Existing C4305 float-conversion/compiler warnings remain; successful compilation
is not a warning-free claim. Source/build success and controlled runtime
performance are separate gates. Headless pixel fixtures validate backend features,
not every live mod's producer behavior.

Final package: `C:\OpenMW-Producer-Test-5`, 320,124,290 bytes before captures.
Executable SHA256:
`3758b797470e8ead08fc79a690ed7a5d28334ac1e9b2dca4baa50b4b1f830323`.
Its `persistent-build-manifest.json` pins the uncommitted source, executable,
build log and cache hashes, and explicitly records
`runtime_performance_accepted: false`. No replacement of normal game installation
or saved-game configuration was performed. Approximately 110 GiB remained free
on C: before the final ~305 MiB package was staged. No new downloads were needed.

The intermediate Test-4 experiment conservatively reserved four full UV/tangent
streams per mesh. That reduced native coverage to ~1,526 draw reuses with ~84
object fallbacks and averaged 77.447 ms versus 84.219 ms for its control. The final
accounting charges retained streams instead and admits any later UV growth before
publication; it does not simply raise the memory cap to hide this regression.

## Final executable comparison

All three following runs used the final Test-5 executable hash above. No builds
or other benchmark arms ran concurrently. Native versus Vulkan control differs
only in OPENMW_V4_NATIVE_OBJECT_PRODUCERS and OPENMW_V4_LOAD_BOUND_TEXTURES.
The separate metadata-batching option was disabled in both arms. OpenGL uses its
normal renderer path with these Vulkan experiments disabled.

| Final arm | Mean ms | Median ms | p95 ms | Approx. FPS from mean |
| --- | ---: | ---: | ---: | ---: |
| Native producers + loaded identities | 72.709 | 72.046 | 80.539 | 13.75 |
| Vulkan, both new mechanisms disabled | 85.334 | 83.912 | 93.540 | 11.72 |
| OpenGL | 15.866 | 15.436 | 17.213 | 63.03 |

Final evidence under `C:\OpenMW-Producer-Test-5\Local-Benchmarks`:

* `native-01/20260924-030717-gameplay-147720`
* `control-01/20260924-030843-gameplay-143688`
* `opengl-01/20260924-031008-gameplay-147368`

Every arm exited 0 and verified original configuration hashes unchanged.
The native run's sampled median was 1,682 native draw reuses, zero native mesh
builds/material updates, and 31 object fallbacks. Native bindings took ~1.01 ms,
identity access ~0.61 ms, and copy-out ~1.44 ms. Total capture was ~15.96 ms;
backend dynamic realization ~12.03 ms, pipeline audit ~5.25 ms, and command
recording ~10.86 ms (sparse inclusive CPU envelopes, not additive/GPU timings).
The source hashes were checked against the staged manifest after staging.

Conclusion: 14.8% lower mean frame time (17.4% more frames per second) for these
new mechanisms in this automated scene. Vulkan remains 4.58 times OpenGL's frame
time. The architecture work is partly implemented and experimentally useful;
the full migration, compatibility qualification, and performance objective are
NOT complete. This is not a request for another user playtest.
