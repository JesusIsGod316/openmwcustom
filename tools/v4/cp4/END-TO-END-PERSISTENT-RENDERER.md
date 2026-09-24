# Retained renderer checkpoint — 2026-09-24

## Status and resume point

Experimental implementation; **not performance accepted, not complete mod/save parity**.
The user requested a GitHub/archive checkpoint because usage is nearly exhausted.
Do not require another manual playtest merely to establish that performance is bad.

Active worktree: `C:\Users\LSCha\.codex\worktrees\cp4f-material-frame-repair\OpenMW custom Build`.
Branch: `codex/cp4f-material-frame-repair`. Starting HEAD:
`efa4f3694fad904b3546e7c846c756403950f0d2`.
This checkpoint also preserves the earlier uncommitted repair batches in this
same worktree. Their individual notes are in this directory. The app's ordinary
Documents checkout is not the active implementation checkout.

## Implemented in this pass

- A neutral persistent draw world with stream/generation/slot identities,
  immutable mesh/material/texture owners, sparse 64-slot copy-on-write pages,
  changed-slot publication, and complete recovery after missed snapshots.
- Supported load-bound NIF producers now publish retained handles and placement
  changes directly. They no longer emit a full ImmediateEffectDraw vector for
  the backend to reconstruct each frame. Material/UV/content changes replace
  only the affected draw resource; hidden changes remain pending.
- A retained VSG scene consumes those changes. Unchanged assets do not enter
  VSG's dynamic-data upload scan. Placement/visibility changes reuse geometry,
  descriptors and pipelines. Removed/rebound GPU owners wait for their last-use
  fence. Resource realization and compilation failures remain retry-safe.
- Stable actor/effect scene membership uses existing fence-backed resident pools
  instead of replacing the complete dynamic root each frame.
- Placement-changing populations reuse a placement-free immutable asset when
  all mesh/material/texture dependencies agree. Stable large groups retain
  instancing; moving groups convert once, then avoid repeated GPU compilation.
- Resource-owned pipeline inventories replace deep repeated graph audits.
  Actual live pipeline handles are still checked for every view; release and
  view-specific recompilation are covered by tests. Inventories are refreshed
  when topology/resource membership changes, not for matrix changes.
- Conservative local bounds and semantic masks remain view-specific. Main-view
  culling must not hide shadows/reflections/maps or suspend gameplay updates.
  Existing occlusion and parallel-recording controls are retained.

This is an end-to-end retained route for **covered objects**, not the complete
replacement of every scene producer. It extends the preceding native-producer
work rather than claiming that work as newly implemented here.

## Explicit limitations and next work

- RigGeometry, MorphGeometry, LOD and unsupported mutation/controller paths
  still use object-local evaluated capture. The measured scene has 31 such
  fallback objects, plus the existing actor/effect publication path.
- Supported producers still perform flat binding/edge/controller-revision
  checks and loaded texture identity access. This is not a fully event-only
  universal scene system. Actor skinning, morphs and attachment publication need
  their own retained direct bindings, not another whole-output cache.
- In the sampled retained run, capture still takes about 13 ms, backend dynamic
  realization about 11 ms, and command recording about 12 ms. These are sparse,
  inclusive CPU scopes, **not additive costs or GPU timestamps**. Profile the
  remaining fallback/actor work and command-record critical path next.
- The 131072-slot limit and producer's existing 64 MiB retained-data budget are
  bounded mechanisms, not a comprehensive process-RAM/VRAM hard cap. Long travel,
  cell transitions, equipment changes, memory pressure and GPU retirement soak
  still require validation. Do not infer that 32 GiB system RAM is sufficient
  from short stationary tests.
- Occlusion reported zero occluded candidates in this exterior. No occlusion
  speedup is claimed. General F2/OMWFX/Rafael shader compatibility is not delivered
  or certified by this pass. Save serialization and normal saves are unchanged;
  that does not prove complete save/mod compatibility.

## Controls

The four new mechanisms remain independently selectable and default off:

| Harness key | Environment variable |
| --- | --- |
| persistent-draws | OPENMW_V4_PERSISTENT_DRAW_STREAM |
| pipeline-inventories | OPENMW_V4_PIPELINE_INVENTORIES |
| persistent-membership | OPENMW_V4_PERSISTENT_DYNAMIC_MEMBERSHIP |
| persistent-populations | OPENMW_V4_PERSISTENT_POPULATION_ASSETS |

`Start-Retained-Vulkan.cmd` selects `--cpu-fastpaths retained`: existing repairs
plus these four mechanisms, excluding the separate batch texture metadata
experiment. `Start-Retained-Control.cmd` uses the same executable and prior
producer route with only these four disabled. Historical persistent/incremental/
exterior profiles explicitly exclude later retained mechanisms. OpenGL remains
available as the compatibility/performance control. No main-branch promotion.

## Verified measurements — Test-3 executable

All arms used the same executable, SHA256
`d90730a5327b93a4a04fed942593f76579611d6c880622ca531c90e29b88abc3`.
Fixed private Seyda Neen scene, 1920x1080, seed 123456, 15-second warmup and
30-second wall-frame sample, frame caps removed equally, same mod/config chain.
Runs were sequential with no concurrent builds. All exited 0 and verified the
original configuration chain unchanged. No normal save was copied or modified.

| Arm | Mean ms | Median ms | p95 ms | FPS from mean |
| --- | ---: | ---: | ---: | ---: |
| Previous Vulkan route, run 1 | 73.159592 | 71.9259 | 81.9391 | 13.67 |
| Previous Vulkan route, run 2 | 72.747951 | 72.1421 | 78.4223 | 13.75 |
| Retained route, run 1 | 61.005288 | 59.7783 | 68.7408 | 16.39 |
| Retained route, run 2 | 59.552607 | 59.3147 | 63.6463 | 16.79 |
| OpenGL control | 15.452486 | 15.5938 | 17.2839 | 64.71 |

Mean of the two Vulkan run means: 72.953772 -> 60.278948 ms, about **17.4% less
frame time**. Retained Vulkan still costs about **3.90 times OpenGL** here. This
is an experimental gain, not acceptable final performance or a general benchmark.

Evidence below `C:\OpenMW-Retained-Test-3\Local-Benchmarks`:

- `retained-01/20260924-080414-gameplay-60072`
- `previous-01/20260924-080610-gameplay-141068`
- `retained-02/20260924-080831-gameplay-61000`
- `opengl-01/20260924-081056-gameplay-126300`
- `previous-02/20260924-081516-gameplay-143952`

Each directory has `manifest.json`, `steady-summary.json`, and local logs.
Sampled median retained work: 1682 resident draws; zero steady-state resource
realizations; 191 placement changes; ~381 ordinary effect draws versus ~2064
previously. Static compilation disappeared from the steady sample. Pipeline
audit median decreased from 5.01615 to 0.7801 ms; persistent draw sync was 0.0875 ms.
These counters prove skipped work, not full-scene compatibility.

Both arms retain 19 error-level mod/config lines (l10n, missing cell references,
global_water handler, ErnPerkFramework scripts) plus texture warnings. They exited
normally. Sparse diagnostics also report incomplete sampled scopes; do not
mislabel those alone as process crashes or claim clean mod logs.

## Final source/build validation

After those measurements, final review fixed a failed-realizer phantom-resident
edge case and corrected launcher profile isolation. The final executable SHA256
is `68f8d655b536acb4992060d97387b22e709ef831df043d22dcd5ed56f9be0e8e`.
Do **not** silently attribute the Test-3 timings to this different binary. The
user requested checkpointing before another complete benchmark round.

- MSVC Vulkan Release `openmw`: PASS, `build/e2e-final-vulkan-build.log`.
- MSVC OpenGL Release `openmw`: PASS, `build/e2e-final-opengl-build.log`.
- Release fixture build: PASS, `build/e2e-final-fixtures-build.log`.
- CTest: 27/27 configurations PASS, including persistent ownership/deltas,
  recovery/slot ABA, failed realization/compilation retry, fence retirement,
  population dependency reuse, inventories and existing capture suites.
- Capture executable: 45 cases, exercised by the relevant CTest configurations.
- Vulkan validation-enabled pixels: PASS with inventories, multiview frustum,
  parallel view recording and flat audit, `build/e2e-validation-pixels.log`.
  Covers retained movement/hide/rebind/unload, auxiliary-view visibility,
  live pipeline release/recompile and the existing rendering fixtures.
- Launcher tests: 30 PASS. Runtime blocker contract and generated-output
  materialization verification PASS. `git diff --check` PASS.
- Existing MSVC float-conversion warnings remain. Local checks are not a claim
  that fresh GitHub CI or long-running gameplay validation has passed.

## Files in this pass

New: `components/rendercore/persistentdraw.hpp`;
`components/render/backend/vsg/{persistentdrawscene,pipelineinventory,retainedscenemembership}.hpp`;
`tools/v4/cp4/persistent-draw-tests.cpp`; retained launchers and this report.

Extended: `apps/openmw/mwrender/v4persistentobject.hpp`, `v4engineframesource.hpp`,
`v4enginerenderbridge.hpp/.cpp`; `components/rendercore/frameproducer.hpp`,
`framerenderstate.hpp`; backend `immediateeffectrealizer.hpp`,
`staticassetconformance.cpp`, `staticworldplan.hpp`, `vsgruntimehost.hpp/.cpp`,
`vsgsubmission.hpp`; CP4 tools `gameplay-diagnostics.py`,
`test-gameplay-diagnostics.py`, `architecture-benchmark.py`,
`stage-persistent-candidate.ps1`, `persistent-resource-tests.cpp`,
`rendering-capture-tests.cpp`, `rendering-pixel-tests.cpp`,
`static-sync-state-tests.cpp`, `rendering-tests/CMakeLists.txt`.

The shared archive was checked before implementation. Its neutral ownership,
GPU-safe retirement, bounded retention and preserved control-path requirements
shaped this pass. Historical rejected mechanisms were not globally re-enabled.
No new download, ordinary-save change, or normal-settings rewrite was needed.
