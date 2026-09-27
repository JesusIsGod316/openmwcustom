# Persistent CPU/resident work — 2026-09-27

## Scope and state

This is an implementation checkpoint following `WORK-AND-PLAN-AUDIT-20260927.md`,
not completion of the native-renderer or performance objective. Post-processing
was deliberately left alone. Existing unrelated dirty changes were preserved.

- Source: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`.
- Branch: `vulkanmw/phase3c-native-runtime-substitution`.
- HEAD: `336a4cf8668afa1a4f03069785421e9a5e2b74ef`.
- Changes are local, uncommitted and unpushed. HEAD alone does not identify them.
- Runtime: `build/phase3c-engine/RelWithDebInfo/openmw.exe`.
- Current isolated package: `C:/VulkanMW-CPU-Residents-Test`.
- Runtime SHA256: `5773b78ebc46cf20ca2c54b65d3fb1980bd1168e521fcf7607313b360812a75d`.
- Its `source-changes.json` records dirty/untracked source hashes at packaging.
  Later harness-only overlay isolation and this report do not change that binary.
- The shared archive was consulted read-only. Its native-producer, unknown-content
  fallback, GPU-safe retirement and save/config isolation constraints influenced
  the implementation. Source and local result manifests remain authoritative.

## Implemented mechanisms

All three renderer switches below are independently opt-in. Absence preserves
the previous path. No global settings, normal saves, mod files, shared archive,
driver settings or overlay configuration were edited.

### 1. Persistent actor bindings and sparse frame streams

`OPENMW_VK_PERSISTENT_ACTORS=1` binds an immutable `RenderCore::ActorProgram` on
model/skeleton/mesh dependency changes. It retains geometry, controller-result
bindings, bone indices, cancellation ancestry and draw metadata. It groups
identical ordered vertex influence lists once, without normalizing or truncating
authored weights. Nearly unique influence sets do not retain grouping metadata.

Frames evaluate the pose and emit changed render inputs without copying the
complete asset plan, index/color/UV arrays, or repeating bone-name/skin-contract
resolution. Immutable inputs feed the existing bounded actor worker pool; VSG
publication and mutable array updates stay on their owner thread after joining.
Unsupported bindings use the old actor evaluator rather than disappearing.
Dependency changes rebind affected plans; retirement/reset prune the registry.

Warm GPU residents update deformation streams and placements. Rigid authored
vertex streams are not repeatedly validated, compared or dirtied. Full payloads
are materialized only for cold/rebound residents. The existing fence-completed
resource-version ring remains the write-safety boundary.

This still performs CPU skinning/morphing and stream uploads. It does not remove
the OSG update world or the earlier actor-capture producer, and is not GPU skinning.

### 2. Per-placement, per-view frustum rejection

`OPENMW_VK_PLACEMENT_FRUSTUM=1` attaches current world-space bounds above individual
static placement subgraphs. Bounds are built once per resident generation, not
by walking mesh vertices during recording. Each VSG view tests its own frustum;
main-view invisibility cannot suppress a shadow/reflection view. Unknown or
incomplete bounds fail open. Hardware-instanced groups keep their existing path.

This is finer frustum culling, not newly implemented GPU occlusion or an early
gameplay-update skip. Existing main-view CPU occlusion remains separate.

### 3. Bounded idle particle resident retention

`OPENMW_VK_RETAIN_PARTICLE_SLOTS=1` keeps eligible small particle-quad resources
across short emitter-population gaps instead of immediately destroying completed
slots and compiling them again when they return. Limits: 256 idle versions,
32 MiB conservative estimated charge, 120 frames maximum age. Unknown/oversized
resources are ineligible. Active/in-flight slots retain their original bounded
ring and fence rules; inactive retention never selects a resource for drawing.

The byte charge includes a descriptor/graph allowance and conservative texture
storage; it is not an exact allocator/VRAM/RSS bound. Count and age are additional
hard limits. This is not an unlimited world/texture cache.

## Files changed in this implementation pass

These are this pass's files, not the entire already-dirty checkout:

- `components/rendercore/actorprogram.hpp` — new neutral bound actor program.
- `components/render/backend/vsg/dynamicactorplan.hpp` — stable plan owners and
  dependency-driven replacement.
- `components/render/backend/vsg/dynamicactorpreparation.hpp` — retained metadata,
  bound evaluation, sparse streams and cold-only authored payload materialization.
- `components/render/backend/vsg/staticassetrealizer.hpp`
- `components/render/backend/vsg/staticassetrealizer.cpp` — placement span and
  deformation-only warm updates; static authored arrays remain static.
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp` — opt-in integration, worker
  input, bounds publication, safe idle collection and bounded evidence counters.
- `components/render/backend/vsg/nativevisibility.hpp` — placement bounds adapter.
- `components/render/backend/vsg/frameresourcepool.hpp` — budgeted idle retention.
- `components/render/backend/vsg/effectidlebudget.hpp` — new small-quad admission
  and conservative retention-charge estimator.
- `tools/v4/cp4/actor-preparation-tests.cpp`
- `tools/v4/cp4/actor-skin-space-tests.cpp`
- `tools/v4/cp4/native-visibility-tests.cpp`
- `tools/vulkanmw/tests/frame-resource-pool-tests.cpp` — new fence/budget tests.
- `tools/vulkanmw/tests/CMakeLists.txt`
- `tools/vulkanmw/run-publication-cohort.py`
- `tools/vulkanmw/vulkan-profile.py`
- `tools/vulkanmw/tests/test-publication-cohort.py` — cumulative switches and
  process-local RTSS-layer diagnostic isolation.
- This checkpoint document.

## Validation

- Optimized MSVC RelWithDebInfo runtime build succeeded.
- Metadata/native-visibility CTest suite: 20/20 passed with runtime DLL directory
  on PATH. An earlier run without that directory had loader errors; that was
  corrected before accepting the suite result.
- Actual engine actor-space fixture: rigid creature passed, 32/32 actor skin-space
  cases passed, 4/4 rigid attachments passed, attachment-skeleton regression passed.
- Actor fixtures exercise retained ownership, changed membership, revision
  replacement, reset/retirement, morph weights and serial/worker parity.
- Visibility fixtures exercise actual VSG traversal under changed/separate view
  matrices, fail-open partial bounds and old-generation immutability.
- Pool fixtures exercise in-flight safety, reappearance, missing-object exclusion,
  age/count/byte eviction, unsupported resources and the old immediate-GC control.
- Overlay harness tests: 7/7 passed. Only the launched child's environment changes.
- `git diff --check` passed.
- The overall architecture-boundary check still fails on pre-existing OSG
  dependencies in `fximagecapture.hpp` and `omwfx.cpp`. Those post-processing files
  were not changed by this pass; this is not a clean architecture-gate claim.

Fixture success and natural benchmark exit do not prove complete mod/save
compatibility, moving-camera visual correctness or elimination of existing body,
water, shadow and post-processing defects. User runtime promotion remains pending.

## Controlled evidence

All runs use isolated settings/content chains and user-data, fixed Seyda Neen,
seed 123456, 1920x1080, 15 seconds warm-up and 30 seconds steady measurement.
No old OpenGL build was rerun. Existing group publication, chunk transactions and
resource inventories remain enabled in all actor/cumulative arms. Post-processing
is off. Do not add CPU totals to overlapping GPU intervals or graph-worker times.

### Actor A/B/B/A — earlier binary in this pass

Package: `C:/VulkanMW-Bound-Actors-Test`.
SHA256: `d4b3deb52b1007b4729dc4b08e21f42b4f745bc62aaf2c906e510ecce0f19f13`.

| Run under Benchmarks/ | Mean frame ms | p95 ms |
| --- | ---: | ---: |
| persistent-actors-0-combined | 61.953338 | 68.3063 |
| persistent-actors-1-groups+transactions+inventories+actors | 58.463397 | 66.2498 |
| persistent-actors-2-groups+transactions+inventories+actors | 58.988311 | 66.2849 |
| persistent-actors-3-combined | 61.659073 | 68.3854 |

Arithmetic average of run means: 61.8062 -> 58.7259 ms, a 3.0804 ms / 4.98%
reduction. Sampled actor preparation falls about 3.19 -> 1.37 ms; stream updates
about 2.97 -> 1.61 ms. This is a repeatable scene-specific gain, not acceptable
overall performance. Results belong to this exact earlier executable.

A subsequent single cumulative probe on that same binary enabled cached admission,
evaluated LOD and split particle capture: 58.1280 ms. Object capture fell to
6.3956 ms, but dynamic compile rose to 5.5724 ms. That identified recurring resource
churn instead of proving a cumulative speedup.

### Particle retention + placement culling A/B/B/A — current binary

Package: `C:/VulkanMW-CPU-Residents-Test`, SHA256 above. Prefix `resident-repairs`.
Control already enables persistent actors; candidate adds particle-slots and
placement-frustum.

| Run | Mean frame ms | p95 ms |
| --- | ---: | ---: |
| 0 control | 59.227975 | 66.1089 |
| 1 candidate | 58.702485 | 66.7557 |
| 2 candidate | 57.606663 | 64.3083 |
| 3 control | 58.096609 | 64.4671 |

Average: 58.6623 -> 58.1546 ms. The ~0.51 ms difference is small relative to
between-run variation; neither mechanism has an independently established
whole-frame benefit from this cohort. Do not promote them on this alone.

### Integrated and overlay follow-ups — current binary

Integrated arm adds all of: actors, particle-slots, placement-frustum, admission,
lod, particles to the publication trio. It preserves the same graphics settings.

- `resident-cumulative-probe-0-...`: 56.121163 ms mean, 66.2160 ms p95.
  Sampled object capture 6.5456 ms, dynamic compile 1.2421 ms (median zero).
- `resident-overlay-probe-0-...+no-overlay`: 55.971792 ms mean, 65.1892 ms p95.
  Confirmed RTSS Vulkan layer absent from loaded modules; its general hook DLL
  still loads. This is not a claim to have removed all overlay instrumentation.
- `resident-overlay-probe-1-...` with normal overlay: 57.360188 ms mean,
  66.4223 ms p95. This second integrated run supports a possible modest improvement,
  but does not isolate which switches contribute. Overlay difference is not
  sufficient evidence that the overlay explains the performance problem.

Closing controls on the same current executable:

- `resident-closing-controls-0-groups+transactions+inventories+actors`:
  58.889228 ms mean, 66.0374 ms p95. Returning to the actor-only arm brings the
  mean back near the earlier 58–59 ms range.
- `resident-closing-controls-1-combined`: 61.001636 ms mean, 69.2845 ms p95.
  This is the publication trio without this pass's actor/resident switches.

The two normal-overlay integrated means average 56.7407 ms, versus 61.0016 ms
for the single closing publication-only control (4.2610 ms difference). This
supports a modest cumulative improvement, not a fully balanced repeated estimate
of every component. The earlier actor A/B/B/A is the stronger isolated result.
The integrated run is still only about 17–18 FPS and does not meet the goal.

All nine current-package manifests report natural exit code zero and unchanged
original configuration-chain hashes. The final executable and package executable
hashes match. No benchmark process remains running. C: has 88,622,116,864 bytes
free at final validation (about 82.5 GiB). No unrelated files were deleted.

The log health counters do not mean the complete log is error-free. Both control
and candidate report an invalid localization context for third-person-alt-attacks,
missing `ErnPerkFramework` in two perk scripts, and a DDS decode/placeholder for
`textures/betterbars_bar.dds`. These remain unresolved compatibility issues, not
evidence of a clean all-mods run. No content was disabled to remove them. No fatal
renderer/deformation mismatch was found by the bounded console-log check.

## What is still expensive, and why the API change has not won

The current Vulkan path retains OSG update (~4.8 ms) and object inspection
(~6.5 ms integrated), then additional actor capture (~2.6 ms), CPU actor preparation
(~1.3 ms), stream updates (~1.6 ms), and a large command-recording critical graph
(~13–14 ms). Lua synchronization and focus/picking also cost several milliseconds.
These Vulkan-only observations do not establish a measured per-stage OpenGL delta.

The integrated GPU spans are also substantial: main ~31.4 ms, shadows ~15.5 ms,
refraction ~4.5 ms in the first integrated probe. They are command-buffer intervals
including stalls, on a different timeline from CPU scopes; do not sum them with
CPU numbers or promise those amounts as recoverable optimization budgets.

There is already parallel recording of independent main/refraction command graphs.
The expensive main graph and inline shadow passes are not evenly split across
cores. More workers on the existing small side graph will not solve that. A
further recording change needs separate traversal/bin/pool ownership and correct
inherited-view handling for each substantial batch, followed by measured parity.

Verified environment checks: optimized main executable; optimized VSG Release DLL
matching the local dependency; no loaded Nsight/RenderDoc/validation layer in the
examined benchmark. Registered layer entries alone do not mean a profiler is active.

## Next work, ranked by remaining cost

1. Remove the supported-object inspection producer, not another cache around it:
   register known objects at load/change boundaries; engine controllers, movement,
   equipment and unload publish explicit deltas. Only unknown mutation classes keep
   deep guards/capture. Gameplay, Lua and off-camera events continue normally.
2. Split substantial recording work with isolated per-frame/per-worker recording
   state. Inspect main/shadow pass ownership first; preserve independent visibility
   for secondary views, command order, fence retirement and conservative bounds.
3. GPU actor deformation from persistent bind geometry and pose/morph buffers,
   reusing this bound program's semantics. Exact candidate picking needs its own
   broad phase; do not recreate all CPU-deformed vertices for focus every frame.
4. Investigate the remaining GPU main/shadow spans and repeated actor resident
   rebuilds. Keep CPU and GPU evidence separate; resolution scaling/upscaling does
   not remove CPU bridge/recording work.

Do not resume post-processing polish, disable mod/gameplay semantics to inflate
FPS, or describe these changes as an end-to-end OSG-free renderer. Do not stack
speedups from different executable hashes.

## Primary references consulted

- VSG/OpenMW donor skin implementation (bind once, evaluate pose results):
  https://github.com/vsgopenmw-dev/vsgopenmw/blob/2830e7e2b4f18ef08ee24eff45a13568ec917061/components/animation/skin.cpp
- Khronos command-buffer ownership, batching and parallel recording guidance:
  https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html

These inform ownership and lifetime choices; neither is evidence of a speedup
in this fork. No third-party example's advertised percentage is reused here.
