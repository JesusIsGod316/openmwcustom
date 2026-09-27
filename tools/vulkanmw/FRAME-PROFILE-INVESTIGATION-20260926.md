# Vulkan performance investigation — 2026-09-26

## Scope

Requested diagnosis, not another speculative renderer repair. Added independently
opt-in observation and private benchmarks; no shader, rendering-policy, save
format, normal configuration, or mod content changes in this investigation.
Existing uncommitted animation/resource experiments are preserved and disabled
in the benchmark arms unless already part of the established retained preset.

Source worktree: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
Branch: `vulkanmw/phase3c-native-runtime-substitution`.
Base: `336a4cf8668afa1a4f03069785421e9a5e2b74ef`; local changes, no new commit or push.
Each private package has a source-file SHA256 manifest and executable SHA256.
Do not identify these dirty builds using their base version string alone.

## Measurement design

- `OPENMW_VK_FRAME_PROFILE=1` plus gameplay diagnostics enables sampled profiles.
  Disabled execution adds no timing reads in the new inner scopes.
- Fixed-capacity frame-thread nesting: inclusive time, exclusive time, call count,
  maximum single-call time. Repeated calls are totaled **within each frame**.
  Worker graph wall times are kept separate; they overlap and are not additive.
- Root time is checked against the sum of exclusive scopes. Incomplete accounting
  samples are rejected. The pre-existing report's median actor preparation time
  represented one batch, not the sum of all batches in a frame.
- GPU timestamp pairs cover main, refraction/reflection (when configured), and
  shadow command graphs. Eight slots per graph, period/valid-bit checks, 64-bit
  modular timestamp subtraction, availability flags, and completion-gated reuse.
  **No query WAIT flag, new fence wait, or new queue/device idle.**
- GPU query intervals include stalls within their command-buffer spans. They are
  not fragment-shader-only execution times and must not be added to CPU times.
- Revision 1 exposed dropped individual totals in the existing try-lock logger.
  Revision 2 enqueues a whole frame-profile summary atomically or drops the whole
  sample, keeping the render thread nonblocking and storage bounded. Legacy stage
  logs can still lose individual rows; use the validated profile totals instead.
- Test driver: private Seyda Neen start, seed 123456, 15 seconds warmup, 30 seconds
  measurement, normal scripted quit. Uses wall-clock intervals, not the engine's
  clamped simulation delta. Same executable within each cohort, no builds running
  during measurements, no normal saves copied. Original configuration-chain
  hashes are verified after every run.
- Full-resolution arms are 1920x1080 with vsync/frame cap removed in the private
  profile only. The separate 960x540 run diagnoses pixel sensitivity; it is not an
  equal-quality optimization or a proposed user configuration.
- OpenGL and Vulkan use the same content/configuration sources. This does not
  establish complete visual equivalence or full shader/mod compatibility.

Vulkan query rules consulted: [Khronos query results](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)
and [timestamp query guidance](https://docs.vulkan.org/samples/latest/samples/api/timestamp_queries/README.html).

## Cohorts and caveats

`C:/VulkanMW-FrameProfile-Test` is exploratory revision 1. The user reported camera
movement during the early tests. Do not use that cohort for controlled backend
speedup claims. Preserve it as diagnostic evidence of the logger loss and broad
CPU/GPU costs.

`C:/VulkanMW-FrameProfile-V2-Test` is the stationary-camera repeat after requesting
no movement. The earlier reply (very little walking, mainly looking around)
applies to the exploratory runs; the repeat followed the explicit request to
leave the camera and character untouched. This is one short scene/cohort, not a
confidence interval or proof of identical image quality across backends.

Executable SHA256:
`dc1aa17bb4c74ca3cc83311c3212928dfbf8363aa2c15aa61acbfe36a2300275`.
Packaged source manifest SHA256:
`1d2ee78b330109ca6be222e611243d3cac4fbc01965d7bad8fe23c3d3795a31a`.
All four successful runs used that executable. Later Python launcher/analyzer
repairs did not rebuild it; each run records its driver hash, and regenerated
analysis records the analyzer hash separately.

## Results: the gap is not one 13 ms bottleneck

| Arm | Mean frame ms | Median ms | p95 ms | Frames | Approx. FPS from mean |
| --- | ---: | ---: | ---: | ---: | ---: |
| Vulkan 1920x1080, profiler off | 68.0000 | 66.5807 | 82.3517 | 441 | 14.7 |
| Vulkan 1920x1080, profiler on | 66.8640 | 66.2470 | 74.4816 | 448 | 15.0 |
| OpenGL 1920x1080, profiler on | 18.1449 | 17.9303 | 20.0022 | 1653 | 55.1 |
| Vulkan 960x540, diagnostic only | 69.8784 | 67.2296 | 92.4508 | 430 | 14.3 |

The profiler-off Vulkan arm takes approximately **3.75 times** the OpenGL frame
time. The slightly faster profiler-on arm is run variation, not an optimization.
Even the optimistic arithmetic of subtracting all 13 ms of command recording
from 68 ms leaves 55 ms (18 FPS), nowhere near OpenGL. The actual gain can be
smaller if a different critical path becomes limiting.

Validated CPU samples: Vulkan full resolution **15/15**, OpenGL **56/56**, Vulkan
half resolution **15/15**, with no profile accounting or query health issues.
These samples are sparse; the frame-time table uses all frames in each wall-clock
measurement interval, so its means need not equal the sampled CPU root mean.

### CPU: several broad costs, not just OSG

Mean **exclusive frame-thread wall time** at full resolution (nested regions
removed; these rows are disjoint). Worker completion/driver costs inside a scope
are included; a scope's wall time is not necessarily all parallelizable computation.

| Work | ms/frame | Interpretation |
| --- | ---: | --- |
| Command recording | 13.083 | Largest single CPU scope; still needs traversal/driver/wait subdivision before choosing a recording redesign |
| Non-actor object capture | 8.590 | Expensive producer remains upstream of retained output |
| Actor capture + CPU preparation + stream update | 8.969 | 2.859 + 3.073 + 3.037; preparation includes all six batches, not just one |
| Static population publication + root pipeline inventory rebuild | 7.328 | 4.641 + 2.686; coarse change propagation despite retained resources |
| OSG update traversal | 4.871 | Material cost, but deleting this alone cannot close the gap |
| Synchronized Lua update | 5.845 | OpenGL 1.795; not yet attributed to a renderer defect, timestep effects, or script workload |
| Focus/interaction update | 3.010 | OpenGL 0.048; needs separate investigation without regressing accurate NPC targeting |
| Remaining capture envelope work | 2.598 | Exclusive residue inside Vulkan capture, excluding the child capture scopes above |

The first five rows total about **42.84 ms** of CPU frame-thread wall time. This
is a map of affected work, **not** a claim that all of it can be eliminated.
There are further smaller realization, compilation, audit, retirement, gameplay,
and synchronization costs in the complete machine-readable report.

Source findings that explain why older downstream caches have limited effect:

- `components/rendercore/staticpopulationproducer.hpp::flush` builds a complete
  `ChunkRecord` for each dirty cell via `makeRecord(cell)` before publication.
  Reusing placement nodes later does not remove this upstream reconstruction.
- `components/render/backend/vsg/vsgruntimehost.cpp::synchronizeStaticWorld`
  seals a new root pipeline inventory on the changed-graph route. This rescans
  retained content instead of maintaining an incrementally updated inventory.
- Object/actor capture still performs substantial inspection despite persistent
  resources. Persistent identity alone does not make the producer incremental.
- Native visibility in this control scene has 612 candidates and substantial
  frustum rejection, but **zero admitted occluder triangles and zero occlusion
  rejections**. Existing default-off LAND-occluder experiments were not enabled;
  this is not a claim that the entire project lacks an occlusion implementation.

### GPU: a second performance ceiling

These elapsed GPU intervals are separate from CPU timing. Do **not** add them to
CPU scopes or treat nested CPU submit envelopes as additional work.

| GPU interval | 1920x1080 mean ms | 960x540 mean ms |
| --- | ---: | ---: |
| Main rendering | 33.049 | 11.483 |
| Shadows | 15.557 | 15.391 |
| Refraction | 4.497 | 4.447 |

Quartering the pixel count reduced the main GPU interval by about 65%, while
whole-frame time did not improve. Command recording remained about 13.1 ms and
object capture about 8.6 ms. This strongly supports CPU-side work as the current
throughput limit in this scene, while exposing a substantial full-resolution GPU
cost that will matter after CPU repairs. Half-resolution is diagnostic only, not
an acceptable substitute for performance at the requested image quality.

The probe is not perfectly deterministic: Lua scope means changed from 5.85 to
8.27 ms and focus from 3.01 to 0.003 ms. Do not attribute the small total-frame
regression to resolution itself. The large GPU interval change alongside similar
frame medians is the useful evidence, not the sign of a small FPS difference.

Steady workload: 90 local lights, three 2048-pixel shadow cascades, shadow distance
4096, refraction enabled, reflection disabled. The shader in
`components/render/backend/vsg/legacymaterialshader.cpp` loops through the supplied
local-light list for each lit fragment. `locallightplan.hpp` gathers world lights,
not spatially selected per-tile lists. This is a source-grounded GPU optimization
candidate; the precise share of the 33 ms attributable to that loop has **not**
been isolated. Authored attenuation and unbounded-light semantics must survive
any spatial light-list implementation; do not just discard distant lights.

### Recommended next implementation scope

This diagnosis supports a coordinated pass, not another silt-strider-only or
13-ms-only patch. These are proposed mechanisms, not changes implemented here:

1. **Incremental static publication end to end:** instance/group deltas from the
   producer through residency and pipeline inventory. Unchanged populations
   must not be republished or globally resealed for a small animated change.
2. **Remove capture for covered object families:** load-time controller/material
   bindings and targeted engine-owned updates. Keep narrow compatibility capture
   for unknown mutation paths. Preserve mod behavior, equipment, saves, and picking.
3. **Reduce view-specific recording work:** useful conservative occluders and
   visibility before expensive preparation, plus retained/batched draw data.
   Parallelize stable independent work only after measuring the recording scope's
   actual CPU/wait split. Main-camera visibility must not remove shadow casters,
   reflection content, or gameplay updates.
4. **Address GPU lighting and shadow work in parallel:** instrument/split these
   passes further and implement conservative light selection and caster culling.
   The resolution probe rules out expecting CPU migration alone to achieve the
   desired full-resolution renderer performance.
5. **Resolve the Lua/focus differential:** inspect those paths independently;
   their costs are real but not established as conversion-layer overhead.

Acceptance must be based on repeatable whole-frame gains at full resolution,
CPU and GPU costs together, no new visual/picking/mod/save regressions, and a
preserved control arm. A faster individual scope or successful build is not
enough. The current evidence does not justify a guaranteed FPS target or complete
mod compatibility, and does not isolate memory-pressure/paging cost.

## Evidence and validation

All paths below are under `C:/VulkanMW-FrameProfile-V2-Test/Benchmarks`:

- `vulkan-off-a/20260926-204255-gameplay-21020`
- `vulkan-profile-a/20260926-204422-gameplay-159560`
- `opengl-profile/20260926-204548-gameplay-134860`
- `vulkan-half-resolution-final/20260926-205442-gameplay-77360`

Each contains `manifest.json`, private effective settings, source/binary identity,
raw logs, automated wall-clock result, and `frame-profile-summary.json`.
All four successful runs exited normally and verified the original config chain
unchanged. Vulkan logs contain 19 error-level mod/config lines; OpenGL 20.
Unsupported texture warnings also remain. This is not a clean compatibility pass.

Two failed diagnostic setup attempts are preserved rather than hidden:

- `vulkan-half-resolution-diagnostic/20260926-204718-gameplay-159364`: the driver
  appended duplicate resolution settings, causing startup configuration rejection.
- `vulkan-half-resolution-retry/20260926-204946-gameplay-159520`: helper definition
  appeared after the CLI entry point, producing a Python `NameError` before launch.

Both launcher errors were fixed; regression tests cover single resolution entries,
unexpected input rejection, and helper definitions preceding the CLI entry point.
Normal settings were not changed, and neither failed attempt is benchmark data.

Validation on the final instrumented C++ and current Python tooling:

- Windows/MSVC `RelWithDebInfo` optimized `openmw` build succeeded. Log:
  `build/phase3c-engine/frame-profile-v2-final-build.log`. Existing conversion
  warnings remain; this was not a warning-free or fresh GitHub CI claim.
- Focused CTest **9/9 passed**; Python suites include 11 benchmark-control tests
  and 4 report-validation tests. Accounting tests cover nested/repeated scopes,
  bounded overflow, and all-or-nothing logger batching.
- Architecture boundary check **117 source files passed**.
- `git diff --check` passed (line-ending normalization warnings only).
- Built/tested locally, **uncommitted and unpushed**; earlier dirty work preserved.
- No downloads or deletion of older evidence. C: free space at final check:
  97,805,860,864 bytes (about 91.1 GiB). No user save changes.

## Files changed for this investigation

Existing files (some also contain earlier uncommitted work):

- `apps/openmw/engine.cpp`: previously unlabelled gameplay/GUI and OpenGL scopes.
- `components/debug/gameplaydiagnostics.hpp`: sampled nested profile integration.
- `components/debug/runtimediagnostics.hpp`: bounded atomic summary enqueue.
- `components/render/backend/vsg/parallelrecordtask.hpp`: per-graph worker wall time.
- `components/render/backend/vsg/vsgruntimehost.cpp` and `.hpp`: resident update,
  scene publication, workload counters, and nonblocking GPU query integration.
- `components/render/backend/vsg/vsgsemanticsession.cpp`: population flush timing.
- `tools/v4/cp4/architecture-benchmark.py`: explicit profile/resolution controls.
- `tools/vulkanmw/tests/CMakeLists.txt`: accounting and report tests.
- `tools/vulkanmw/tests/test-benchmark-control.py`: resolution and CLI-order tests.

New files:

- `components/debug/frameprofile.hpp`.
- `components/render/backend/vsg/framegpuprofile.hpp`.
- `tools/vulkanmw/frame-profile-report.py`.
- `tools/vulkanmw/run-frame-profile-cohort.py`.
- `tools/vulkanmw/tests/frame-profile-tests.cpp`.
- `tools/vulkanmw/tests/test-frame-profile-report.py`.
- This report.

## Deferred correctness observations

- Disconnected/separated NPC body parts reported from an earlier automatic launch:
  unresolved, not a performance success or compatibility pass.
- Character/rock intersection in
  `C:/Users/LSCha/AppData/Local/Temp/codex-clipboard-6ea98bb0-6af3-44a9-995d-c8892ef3dccf.png`:
  explicitly deferred by the user. Attachment creation time 20:33:35 falls inside
  the exploratory OpenGL run; confirm reproduction/backend before attributing it
  to Vulkan. The image alone does not distinguish collision placement from visual
  geometry/transform problems.
- Existing Lua/configuration errors and unsupported texture warnings in the
  modded profile remain separate compatibility defects; normal exit is not a
  clean-mod-compatibility result.
