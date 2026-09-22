# CP4F manual gameplay diagnostics

Latest runtime repair: see `EXTERIOR-RUNTIME-REPAIR-2026-09-19.md` for current
binary provenance and verification (historical results below are retained).
Independent `v4_model_load` operation records now identify model preparation
costs; sampled `static_sync` measures static planning/publication on gameplay
frames. Nested/inclusive timings must not be added to their parent totals.
The new publication and texture-identity control variables are captured by the
collector's existing inherited OPENMW environment manifest. Use them only for
explicit same-executable controls, not the default user test.

Opt-in observational mode for the Sept 18 prison-ship failures: frozen/deformed
NPCs, stale Barrel targeting, and low frame rate. It does not repair or bypass
those failures. No Lua worker changes, no forced OSG cull, no GPU waits, no
OpenGL fallback, no resource-cache disablement and no input injection.

## Run and evidence

Use the existing `run-runtime-qc.ps1` with `-Mode Manual`, `-Executable`,
`-UserData` (normal config directory), `-PythonExecutable`, and the same
`-DllDirectory`/`-OsgLibraryPath` as the working binary. `-SourceHead` and
`-SourceDiffSha256` record the source checkpoint. Manual mode ignores the legacy
timeout policy and waits for natural exit, never killing the game. The helper
can run hidden; only the game should be visible.

The helper creates a unique writable config/log layer, copies settings, input,
shader configuration and Lua storage, and inherits normal content configuration.
Original config files and old dumps remain untouched. Normal user-data/save
selection is retained: the tool does not save, overwrite, remove or copy saves.
Manual in-game saving still has its ordinary effect. Configuration paths loaded
by the engine must be checked in the new log before interpreting the test.

Each evidence directory contains executable/config hashes and arguments in
`manifest.json`, `console.log`, `openmw.log`, `gameplay.jsonl`, and automatically
generated `report.md`/`report.json` after exit. A fresh crash dump, if produced,
uses this private log/config directory. Report generation is also available
while a run is incomplete: `python gameplay-diagnostics.py report <directory>`.
It does not label an unfinished stage as a crash just because the game is still
running. If the collector itself is interrupted, rerun the report command.

## Measurements and limits

- Frame/viewer progress and camera update callback count.
- Skeleton traversals accepted or suppressed by inactive/off-screen policy.
- Current gameplay view versus retained camera view, and actual picking-view
  comparison plus hit reference identity. It does not issue a second pick ray.
- Up to 16 actors per sample: stable world/slot/generation IDs, skeleton identity,
  bone count and local/global pose fingerprints.
- Up to 64 draws and 32 vertices per draw for each sampled actor: deformed CPU
  geometry variation, nonfinite values, and comparison against VSG resident
  position arrays. Also actor placement variation and actor/effect reuse counts.
  This is not GPU readback or a canonical-versus-neutral skinning oracle.
- Inclusive wall times for mechanics, physics, world, OSG updates, focus, Vulkan
  capture, dynamic realization/compile, pipeline audit, submit/present and Lua
  waits. Nested times overlap. These are not GPU execution times or a benchmark.
- Independent start/end records for cell loads, exterior grid changes, and
  interior/exterior transitions, including elapsed time and exception unwinding.
  Unique operation IDs support nested loads. These records do not turn on the
  expensive sampled actor/geometry checks.
- First V4 coordinator failure is logged immediately before unwinding/fatal
  dialogs and emitted to the diagnostic stream outside sampled frames. Fatal
  game-log entries and nonzero process exit codes are report findings even if
  sampled frame invariants were clean. Running collectors and older captures
  without loading records are explicitly incomplete coverage, not passes.

The first 12 engine frames and every 30th through frame 36,000 are sampled.
Afterward, every 300th frame is sampled so late transitions retain sparse
coverage instead of losing all detail. Detail is capped per sample; a marker reports the 100,000
record file limit. Sampled records are flushed to preserve evidence after native
failure. Failures between samples may not have a last-entered-stage record.
Unsampled frames retain no detailed history. Diagnostic overhead is not zero;
performance acceptance requires a separate diagnostics-off control run.
Independent operation/failure records still share the bounded file limit;
the ordinary game log retains the first coordinator failure regardless of the
diagnostic mode or file cap. Arbitrary native failures can still need a dump.

The report lists multiple observed discrepancies and clearly labels movement
with unchanged pose as a hypothesis. Missing actor/pick samples are coverage gaps,
not passes. Legal but wrong transformations/materials can pass Vulkan validation;
pixel correctness, complete skin-space equivalence, texture interpretation, map
output and GPU budget/long-run retirement still need further evidence.

## Verification and source state

Archive control block plus checkpoint 111 were read before implementation.
Branch remains `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`; instrumentation is uncommitted WIP
on top of the preserved recovery changes, not CP4F acceptance or CP5 readiness.

Native behavioral tests cover JSON escaping, invalid/mismatched camera matrices,
and observing skeleton suppression without changing the policy. Python fixtures
cover multiple findings, partial crash lines, missing capture, stable versus
moving actor hypotheses, generation separation, capture limits, and manual
launch argument/profile preservation. No fixture proves full gameplay parity.

Files changed for instrumentation:

- components/debug/gameplaydiagnostics.hpp (new)
- apps/openmw/engine.cpp
- apps/openmw/mwrender/camera.cpp
- apps/openmw/mwrender/renderingmanager.cpp
- apps/openmw/mwrender/v4semanticsource.cpp
- apps/openmw/mwrender/v4enginerenderbridge.cpp
- components/sceneutil/skeleton.cpp
- components/render/backend/vsg/vsgruntimehost.cpp
- tools/v4/cp4/effect-capture-tests.cpp
- tools/v4/cp4/runtime-blocker-contract.py
- tools/v4/cp4/run-runtime-qc.ps1
- tools/v4/cp4/gameplay-diagnostics.py (new)
- tools/v4/cp4/test-gameplay-diagnostics.py (new)
- tools/v4/cp4/GAMEPLAY-DIAGNOSTICS.md (new)

The source contract now permits reading the cached camera for diagnostics while
requiring the Vulkan output view to remain sourced from calculateViewMatrix.
Engine hooks are direct V4 source changes; the historical V3 generator chain was
not rerun or promoted. No GitHub CI, commit or push was requested for this batch.

Verified 2026-09-19: MSVC RelWithDebInfo production build/link PASS, all three
native CTest suites PASS (effect suite now 39 cases), eight Python report tests
PASS, seven CP4 source contracts PASS, PowerShell runner syntax PASS. Existing
compiler warnings remain. Binary SHA256:
`2c578713d542be50d6d825527b4861b4d3a00877982a54fd733a449203726152`.

Manual capture launched in `20260919-000224-gameplay-62756` under the normal
profile's runtime-qc-evidence folder. Startup log confirms VSG/Vulkan and the
private final config/log layer; structured trace is being written. Initial
records already show viewer_done=1 with OSG frame stamp 0 while engine frames
continue. This is an observed update-lifecycle discrepancy, not yet a complete
diagnosis of actor composition or proof that every symptom shares one cause.
The report counts frame_begin records independently because OSG stamps may be
stuck; it also reports the number of distinct OSG stamps.
