# Exterior repair batch, 2026-09-23

Status: implemented candidate; runtime performance NOT accepted. No save/content
format changes, mod edits, or normal user-config edits. No claim of complete
OpenMW shader/mod parity. The existing F2/.omwfx compatibility gap remains.

## Evidence and scope

Source: `codex/cp4f-material-frame-repair`, base
`efa4f3694fad904b3546e7c846c756403950f0d2`, plus preserved uncommitted work.
The older Documents checkout is not the implementation source.

Input: `C:\OpenMW-Incremental-Test\Test-Results\20260923-225841-gameplay-136536.zip`.
Its EXE SHA256 was
`7b6c2a251e4dc1d1063b503e99b467ec8554aed124e59f134e0e551d0e5603bf`.
The 84 embedded changed-source hashes matched this checkout before this batch.
Normal exit 0; both incremental controls were enabled; profiler was null.

Sparse complete-frame observations: 247 ship samples median 17.8410 ms, eight
exterior samples 123.1684 ms, seven office samples 27.6234 ms. Previous persistent
capture exterior median was 123.6520 ms (15 samples). Different route/cameras and
small samples: this is not a controlled A/B or evidence of a meaningful outdoor gain.

Exterior CPU envelopes included capture 34.577 ms (object capture 30.5999 ms),
static planning 10.5619 ms, dynamic realization 22.2416 ms, pipeline audit
3.6286 ms and task submission 29.1299 ms. Nested scopes must not be added;
submission wall time is not GPU execution time. Population reconciliation was
only 0.4515 ms; a bottle placement still triggered unrelated individual/LAND
planning. Existing main-view static visibility did not cover ~2,000 evaluated draws.

## Mechanisms and controls

All new switches are default-off in the engine. The optimized launcher enables
them explicitly. No live OSG/Lua/gameplay evaluation runs on new workers.

| Switch | Mechanism | Safety boundary |
|---|---|---|
| `OPENMW_V4_INCREMENTAL_INSTANCES` | Reuse unchanged individual/LAND plans before scanning mesh payloads | Depends on incremental population path; any asset publication invalidates conservatively, including hidden dependencies; source/epoch/options/instance revisions checked |
| `OPENMW_V4_PARALLEL_ACTOR_PREPARATION` | Pose/deformation preparation across at most three workers plus caller | Published neutral inputs only, exclusive output slots, join before VSG updates; batches of eight, serial control retains one actor |
| `OPENMW_V4_EFFECT_FRUSTUM` | Cull eligible evaluated-object/effect graphs before recording | Current geometry bounds, each view's own frustum, no main-view mask reused for reflection/shadow; unknown displacement/stencil/depthless/refraction/soft/line/point paths stay visible |
| `OPENMW_V4_DEDUP_PIPELINE_AUDIT` | Inspect shared DAG objects once per view census | Fresh visited set per audit; failures, new graph nodes and different view IDs remain checked |
| `OPENMW_V4_ACTIVE_SWITCH_CAPTURE` | Capture only enabled `osg::Switch` children | Reads evaluated switch values; no controller ticks, no changed LOD selection, traversal masks retained |
| `OPENMW_V4_SUBMIT_BREAKDOWN` | Observe start/wait, record and finish/late-transfer/queue CPU scopes | Uses pinned VSG instrumentation; original submit/transfer/fence logic unchanged; does not replace existing profiler instrumentation |

The last switch is measurement, not a performance optimization. Early transfers
remain in the inclusive submit envelope, not a separately timed stage. The
installed VSG DLL's scope callbacks are verified by the pixel tests.

Existing static frustum and conservative terrain occlusion remain enabled.
The evaluated-draw addition is frustum culling, NOT new occlusion or pre-capture
rejection. Capture can remain expensive; no FPS target is promised by this batch.

## RAM/cache findings

The cache exists to reduce movement/loading stutters, not to maximize retention.
Its architecture is open for replacement, per the user's clarification. No blanket
flush or arbitrary shorter TTL was added here.

The supplied run already performed 33 pressure sweeps removing 18,393 entries
(entry count is NOT reclaimed bytes). Preload ownership was released under pressure
and resumed after recovery. In the exterior, sampled available physical memory
rose above 6 GB while render work remained costly. The ~183.6 MiB minimum in the
broader transition window is a pressure warning, not proof of hard page faults or
a leak. Cumulative OS page faults include soft faults.

The healthy-policy cache is configured for 96/192 cells and 1,800 seconds, but
pressure release bypasses the retention floor. A redesign should preserve useful
nearby/recent assets while bounding speculative admission, reacting to physical
AND commit pressure, and measuring actual ownership/bytes and repeat load misses.
Live gameplay state and GPU-in-flight resources cannot be evicted as cache.
This batch bounds new actor scratch retention; it does not claim to redesign the
whole cache or account for all process memory.

## Validation

- Rendering/resource CTest: 18/18 passed (3.94 s). Includes 48 distinct actor
  poses across 30 parallel repetitions, exact serial parity, bounded sub-batches,
  missing frame inputs, static hidden-resource/options/epoch invalidation and
  fenced retirement, switch changes without update callbacks, DAG mutation checks.
- Validation-enabled Vulkan pixel matrix passed on NVIDIA GeForce RTX 5050
  Laptop GPU with the new submission hooks and capture/audit switches enabled;
  no validation messages. New check verifies actual record-traversal rejection,
  independent auxiliary camera visibility, movement restoring identical pixels,
  and mirrored/nonuniform placement. Existing terrain, water, material, shadow,
  preview, UI and postprocess fixtures also passed.
- Full Vulkan MSVC Release build linked, 0 errors, 85 warning occurrences.
  OpenGL Release target build succeeded, 0 errors/warnings in its incremental
  invocation (no affected OpenGL production translation units needed rebuilding).
  Warning counts are build-output occurrences, not unique/new warning counts.
- Launcher tests 28/28; configuration tests 17/17; runtime blocker and generated
  materialization contracts passed. `git diff --check` passed.
- Runtime-report unit tests: 17 passed, one native-writer integration test
  skipped because its separate writer-fixture capture directory was not supplied.
- The validation-enabled pixel matrix also passed with all new switches absent.
- Package/startup verification passed: 82 shader files verified, EXE version
  smoke exit 0. The new folder initially occupied 319,506,933 bytes (~305 MiB),
  and about 121 GiB free remained on C:. No downloads or old-build deletion.
- Both launch modes passed prepare-only preflight without starting the game;
  optimized mode has all six new switches, same-EXE control has none. Terrain
  occlusion stays on in both. Original configuration-chain files still match
  captured hashes, normal saves were not copied, and the old EXE is unchanged.
- All 92 packaged changed-source hashes match the current source tree. Candidate
  EXE SHA256: `1eb473a66c90b855baec3f91af2752265fe52acf0e6365215f886a2380751bf0`.
- Initial validation exposed missing test-harness link dependencies and a fixture
  missing dynamic transforms; fixed, not ignored. A direct reconstructed submit
  path could not link a non-exported VSG DLL type, so it was replaced with the
  library's supported instrumentation hooks before the successful full build.

Build/test logs under the source root:

- `build/cp4f-engine-vulkan/exterior-repair-build.log`
- `build/cp4f-engine-opengl/exterior-control-build.log`
- `build/cp4f-rendering/exterior-ctest.log`
- `build/cp4f-rendering/exterior-pixels.log`
- `build/cp4f-rendering/exterior-pixels-control.log`

## Candidate and testing

Staged destination: `C:\OpenMW-Exterior-Test` (new directory, older packages kept).
Use `Start-Exterior-Repairs.cmd`; normal launch does not require administrator.
Choose NEW GAME in the isolated diagnostic profile, test the exterior where the
slowdown occurred, and quit normally. Normal saves are not copied or altered.
Results go under `Test-Results` with effective controls and source/EXE hashes.

`Start-Exterior-Control.cmd` is an optional SAME-EXE control. It keeps the earlier
persistent/incremental paths and disables only the exterior switches. No old
rejected EXE rerun is requested. `Start-Exterior-Profile.cmd` is optional Nsight
CPU profiling and needs the user to launch an administrator terminal; no automatic
elevation. Its F12 capture is bounded to 20 seconds.

No commit/push or runtime promotion was performed. Gameplay FPS, full save/mod
compatibility and reduced traversal stutter still require user validation.

## Exact source/tool files changed in this batch

Relative to the active worktree; other pre-existing changes were preserved:

- `apps/openmw/mwrender/v4effectcapture.hpp`
- `components/rendercore/boundedparallelfor.hpp`
- `components/rendercore/effectframe.hpp`
- `components/render/backend/vsg/staticworldresidency.hpp`
- `components/render/backend/vsg/dynamicactorpreparation.hpp` (new)
- `components/render/backend/vsg/effectvisibility.hpp` (new)
- `components/render/backend/vsg/vsgsubmission.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `tools/v4/cp4/actor-preparation-tests.cpp` (new)
- `tools/v4/cp4/static-sync-state-tests.cpp`
- `tools/v4/cp4/native-visibility-tests.cpp`
- `tools/v4/cp4/persistent-resource-tests.cpp`
- `tools/v4/cp4/rendering-capture-tests.cpp`
- `tools/v4/cp4/rendering-pixel-tests.cpp`
- `tools/v4/cp4/rendering-tests/CMakeLists.txt`
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- `tools/v4/cp4/stage-persistent-candidate.ps1`
- `tools/v4/cp4/Start-Exterior-Repairs.cmd` (new)
- `tools/v4/cp4/Start-Exterior-Control.cmd` (new)
- `tools/v4/cp4/Start-Exterior-Profile.cmd` (new)
- `tools/v4/cp4/EXTERIOR-REPAIR-BATCH.md` (new)

Read-only-input analysis output is separately retained at
`C:\OpenMW-Incremental-Test\Test-Results\20260923-225841-analysis.json`.
