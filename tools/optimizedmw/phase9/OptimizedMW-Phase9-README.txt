FOCUSED RENDERER TRIAL (2026-10-02) - preparation progress and supplied blendmap mips

Use C:\OptimizedMW_Test\START-OptimizedMW-Test.bat and choose F.
The focused menu uses readable presets; old choices 1-11 retain their meanings:
  F/1 = Retained CONTROL, no warm keyframes/cleanup/new terrain mechanisms.
  F/2 = Cleanup only, on the same retained control.
  F/3 = Original STEP, without early preparation or the progress repair.
  F/4 = Early STEP with bounded preparation progress.
  F/5 = Terrain + supplied blendmap mips: F/4 plus the independent CPU mip chain.
  F/6 = Combined renderer: F/4 plus cleanup; choose mips off/on separately.
Choose N for Normal performance or D for Deep engine diagnostics. Normal does
not start Windows recording or require elevation. Focused Deep defaults to engine
diagnostics only; choose W when a Windows trace is specifically useful.

First comparison: F/1, F/5, F/5, F/1, all Normal. Use the SAME outdoor save,
first-person view, frame cap and timed walking route. Hold the identical heavy
view for 45 seconds before walking; quit normally. Supply the same save-file path,
route and cap labels at each launch. The manifest hashes the declared save file;
it does not prove that you actually loaded it. Cap/view labels are declarations,
not automatically verified measurements. Keep first visits, revisits and loading
separate; a fresh process does not prove cold OS/driver caches. Upload the four
verified engine ZIPs. No system trace is needed for this initial comparison.

If this package merits further testing, compare F/4 against F/5 to isolate mips,
and F/1 against F/2 to isolate cleanup. The launcher does not automate repeated
gameplay, change saves, clear caches or enable warm keyframes in these presets.
Progress and supplied-mip switches are explicit and OFF on all old presets.
Preparation can decline safely; a selected flag is not proof of completed work
or a faster frame. CPU mips may retain the original upload on unsupported inputs.

Command-line equivalents (the same launcher and manifest):
  START-OptimizedMW-Test.bat -FocusedPreset CONTROL -TraceLevel Normal
  START-OptimizedMW-Test.bat -FocusedPreset TERRAIN-MIPS -TraceLevel Normal
  START-OptimizedMW-Test.bat -FocusedPreset EARLY-STEP -TraceLevel Deep
  START-OptimizedMW-Test.bat -FocusedPreset COMBINED-MIPS -TraceLevel Deep -SystemTrace
Add -SavePath "C:\path\same-save.omwsave" -RouteLabel "same route" -FrameCapLabel
"uncapped" to record the comparison declarations. For advanced isolation only,
Custom/CLI keeps -CompositeProgress and -CompositeCpuMips independent of masks.
Do not combine explicit mechanism switches with -FocusedPreset; the preset is
the complete named selection. Normal/Deep never changes the gameplay vector.

HITCH REDUCTION TRIAL v3 REPAIRS - retained prior choices and reference instructions

Use C:\OptimizedMW_Test\START-OptimizedMW-Test.bat.
The first four choices retain their previous meanings. Enter is Normal CONTROL.
New choices isolate the animation lifetime change:
  5 = Normal ANIM control: existing source preparation, new retention OFF.
  6 = Normal ANIM retained: the same preparation, new retention ON.
  7 = Diagnose ANIM retained: choice 6 plus Deep/automatic Windows tracing.
Choices 5 and 6 do not start Windows recording or need elevation.
The repaired mechanisms have separate choices; choices 1-7 retain their meanings:
  8 = Normal ANIM repaired: choice 6 plus charged completed warm-keyframe reuse.
  9 = Diagnose ANIM repaired: choice 8 plus Deep/automatic Windows tracing.
 10 = Normal repair combination: repaired ANIM + incremental composite preparation
                                + bounded delete admission.
 11 = Diagnose repair combination: choice 10 plus Deep/automatic Windows tracing.
Choices 8 and 10 do not start Windows recording or need elevation.

First check Normal CONTROL (2) for correct loading, camera, visuals and quitting.
Check Normal repaired ANIM (8) briefly for correct loading and visuals, then use
Diagnose repaired ANIM (9) to verify activation/ownership and capture coverage.
To isolate the warm-keyframe repair, run 6, 8, 8, 6 on the SAME save, fixed heavy
view, route and cap state. Choice 6 is the earlier unrepaired retention control.
Hold the fixed view about 45 seconds, walk the same 2-3 minute route and quit.
Upload the resulting ZIPs. A fresh process is not proof of cold OS/driver caches.
Keep first visits and revisits distinguishable; record any workload deviations.
Use 9 later if a missed dependency, hitch or memory change needs explanation.
Test the renderer repair pair separately through Custom attack mask 320 before
using combination 10; use 11 when a combined Deep diagnosis is needed.
Do not begin by enabling every new feature together.

Custom retains the old 0..15 feature mask and adds a separate 0..511 attack mask:
  1  ANIM-RETAIN: one nearest predicted target, charged completed dependencies,
                 fresh-job progress reuse, coherent pressure/cancellation policy.
  2  ANIM-PLAN: reuse bounded exact ordered animation source descriptors.
  4  EARLY-BAKE: prepare real terrain composite dependencies before draw demand.
  8  WATER: prepare the actual static water program/normal-map resources only.
 16  WEATHER: decode the already-selected upcoming cloud image on a worker.
 32  LUA-PACKAGES: reuse per-container package registration descriptors.
 64  DELETE-ADMIT: admit GL deletion using measured headroom, pressure and age bounds.
128  WARM-KF: repaired charged completed keyframe lease; requires ANIM-RETAIN.
256  STEP: prepare terrain composite dependencies incrementally under a byte bound.
Add bits for combinations. The launcher enables required prerequisites explicitly:
ANIM-RETAIN requires the old ANIM switch; EARLY-BAKE requires P9 composite
preparation, which is distinct from the old BAKE switch. STEP also requires P9
composite preparation, without enabling EARLY-BAKE or the old BAKE switch. WARM-KF
without ANIM-RETAIN is rejected. All new switches default
OFF outside selection. Original required paths remain fallbacks. Optional work
may decline because of pressure, identity changes or lack of actual lead time.
No shader-quality, mod, save, Lua thread-count or DLL substitution is performed.
STEP's shared 32 MiB estimate charges descriptor metadata, the private target and
the queued/unconsumed complete layer; target debt lasts through every layer. This
bounds additional queued preparation, not total map memory or physical GPU/VRAM
residency. A complete layer above the cap declines preparation and retains the
original complete bake; pressure/cancellation retain required rendering fallbacks.

Examples through the same launcher:
  START-OptimizedMW-Test.bat -HitchMask 4 -AttackMask 0 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 4 -AttackMask 1 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 4 -AttackMask 3 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 0 -AttackMask 4 -TraceLevel Deep -SystemTrace
  START-OptimizedMW-Test.bat -HitchMask 0 -AttackMask 32 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 4 -AttackMask 129 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 0 -AttackMask 320 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 4 -AttackMask 449 -TraceLevel Deep -SystemTrace -GpuQueryDefer
Optional -ObserverLocal is a separate Deep-only diagnostic aggregation trial.
Existing D1 optimized catalog and ObserverLocal remain explicit diagnostic trials;
they are not enabled by the new presets.

Diagnosis choices 9 and 11 defer GPU timestamp collection by at least two draw
frames, poll once per frame/context, and back off unavailable results. The old
query collection path remains available without -GpuQueryDefer. Both use the
same bounded query ring and read only available results. No shutdown GL drain
is performed. Final queries may remain pending; that is missing data, not zero
GPU time. p9-gpu-passes.csv.queries.csv and .pending.csv preserve accounting and
exact unresolved ownership. GPU-QUERY-CAPTURE.json checks final independent CSV
writer completion and collected-row counts. Partial GPU results do not invalidate
an independently complete CPU timeline. Timestamp intervals may include command
submission gaps and idle time; they are not proof of GPU-active utilization.

New captures contain bounded per-frame observed pause/GUI/menu/hidden/loading/
exception flags and sampled camera/player/cell/weather/time receipts. Reports
retain every slow frame and show separate observed-state cohorts and adjacent
variation. Missing state evidence is unknown; sampled poses do not prove an
identical route or viewpoint, and engine wall time is not presentation time.
The camera zoom guard retries the actual stored value if it is unavailable or
malformed; it preserves user storage and records one warning per unresolved episode.

Native/source tests establish implementation behavior only. Runtime performance,
memory, content and visual acceptance remain unproven until user comparisons.
Known-owner water/cloud work and optional lifetime changes are included. Ordinary
texture mipmap relocation, new shadow traversal, generic Lua binding rewrites,
further UI reflow changes and a Mercy mod overlay remain gated on missing proof.

RETAINED HITCH REDUCTION TRIAL v2 (2026-10-01)
Double-click START-OptimizedMW-Test.bat normally and choose ONE test:
  1 = Diagnose CONTROL: Deep engine diagnostics + automatic Windows trace.
  2 = Normal CONTROL: performance comparison with candidates off.
  3 = Normal ALL: performance comparison with all four hitch candidates.
  4 = Diagnose ALL: Deep engine diagnostics + automatic Windows trace.
  C = Custom combination / I/O / trace depth; A = earlier Phase 9 modes.
For 1 or 4, accept the Windows administrator prompt for the recorder only.
The game starts automatically with the launcher's ordinary permissions after
recording is ready. Use the same save, hold the fixed view about 45 seconds,
walk the same 2-3 minute route, quit normally, and let this window finish.
Explorer selects the single upload ZIP. It contains unchanged engine/system
archives, the final qualification when available, and a hash/status manifest.
Raw captures remain preserved if a channel or packaging step fails. A partial
result never claims complete Windows coverage. No second recorder window needs
to be started manually. Do not Run as administrator the main test launcher.
Automatic recording defaults to 600 seconds, stops on game exit, and preserves
the recorder's existing disk-space guard. Normal choices start no recorder.
Custom retains all 16 combinations and separate I/O choices. Earlier modes keep
their old meanings INSIDE A; first-screen 3 now means Normal ALL, not old mode 3.
Both depths use the SAME selected gameplay features and reference foundation.
Normal retains frame/pacing measurements; Deep adds detailed attribution.
START-OptimizedMW-SystemTrace.bat remains available for manual advanced tracing.

Independent mechanisms (OFF unless selected):
  UI: preserve text measurements by avoiding redundant size/font/wrap changes.
  BAKE: batch graphics-state restoration within one terrain composite map.
  ANIM: prepare the NPC animation sources missing from normal cell preloading.
  TERRAIN: wait for the actual demanded terrain target, publish only ready targets.
All 16 combinations are available. 0 = CONTROL, 1 = UI, 2 = BAKE, 4 = ANIM,
8 = TERRAIN, 15 = ALL. Pair/triple combinations have descriptive names in Custom.
I/O selection is separate: CONTROL, I1, I2, I1I2. Enter keeps original I/O.
D1 is available only with Deep and selects the existing diagnostic collector.

Reproducible invocations through the same shader-checked BAT:
  START-OptimizedMW-Test.bat -HitchMask 0 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 15 -TraceLevel Normal
  START-OptimizedMW-Test.bat -HitchMask 15 -TraceLevel Deep
  START-OptimizedMW-Test.bat -HitchMask 0 -TraceLevel Deep -SystemTrace
  START-OptimizedMW-Test.bat -HitchMask 3 -TraceLevel Deep -Candidate I1
Use -NoPrompt only with an explicit selection. The capture name contains the
combination, depth and I/O choice. RUN-MANIFEST.json records a gameplay feature
hash independent of trace depth, plus effective settings, source/runtime hashes,
restoration and capture qualification. Requested switches alone do not prove
resource preparation happened; invalidation/pressure/cancellation can fall back.

Normal captures determine gameplay performance. Deep captures explain work and
include diagnostic overhead. The first diagnostic run is choice 1. Compare
Normal CONTROL (2) versus Normal ALL (3) on the
same save/route; use single/pair combinations to isolate a promising or regressing
change. All combinations are available without requiring 32 runs immediately.
Whole-frame distribution, adjacent changes, hitch clusters, transition duration,
visual correctness and memory are relevant; a lower average alone is insufficient.

Deep Trace also breaks UI property/layout/measurement, optional animation source
preparation, terrain readiness publication, and composite pass/restore work into
nested spans in the existing p9-hitch-spans.csv. Use parent/child exclusive time
and causal links; do not add inclusive child times to their enclosing duration.
Fast details retain the existing threshold, and recorder loss still invalidates
completeness. Preparation receipts show work performed, not measured FPS gains.
Lua splits additionally cover event decoding/handler dispatch, timer queue/delivery
and lateness, script sandbox/package/handler preparation, saved-data/interface and
timer restoration. Script/container/callback identities use the existing bounded
fields. Lua allocation counters remain observations of churn, not direct GC timing.
Deep native API splits also separate inventory/equipment, ray/path queries,
storage, serialization and UI creation. The existing 250 microsecond detail filter
is preserved: an absent API row does not prove that the API was not called.

The previous Phase 9 numbered modes remain available with the four new mechanisms
OFF. The following section documents the earlier I/O and attribution mechanisms.

HITCH REDUCTION TRIAL v1 - retained I/O/diagnostic mechanisms
The launcher preserves modes 1-13 and the accepted Phase 9 foundation. Every new
I/O/catalog mechanism defaults OFF. Select it independently in Custom:
  CONTROL: mode 1, exact 8 KiB refill, resident seek/tell OFF, original catalog.
  I1: mode 1, first 64 KiB refill trial, resident seek/tell OFF.
  I2: mode 1, resident-window seek/tell only, exact 8 KiB refill.
  I1I2: mode 1, both I/O candidates; use only after incremental qualification.
  D1: mode 3, ordered catalog collector; I/O candidates OFF.
Mode 3 CONTROL remains the deep diagnostic reference with I1/I2/D1 OFF. New P0
compile ownership/operation attribution is explicitly enabled in deep modes and
written to compile-attribution.csv; original p4-compile.csv is unchanged. This
additional family differs from the older deep reference and is declared in the
manifest. Basic mode 1 leaves it OFF. D1 is a diagnostic-cost/equivalence trial,
not evidence of a normal-gameplay FPS improvement.
P0 also records queue preparation, candidate building, admission, selected
operations, delete flushing, metadata/CSV work, and async startup/join envelopes.
Wall-only envelopes preserve existing CPU measurement ownership. The owned
CompileTextureOp texture/image0 fields describe the operation's target and first
source-image snapshot, not actual GL bindings, mip levels or driver arguments.
Op age and selected-set age are separate from the global oldest queue age.
First-demand/progress fields are scheduler observations; producer times remain
unknown. Context 4294967295 means unknown. A frame coordinate with frame_known=0
is a join placeholder, not proof of an actual viewer frame. Registry overflow,
metadata/format allocation failures and missing/lost rows prevent full compile
family qualification even when other CPU evidence remains usable. GPU/driver
internals remain subject to the separately qualified Windows/GPU trace coverage.
All new profiles, including basic CONTROL, request one new startup file-policy
acknowledgement log line. The manifest declares this low-rate metadata addition;
old/new-OFF parity testing still applies. It acknowledges the frozen requested
policy and does not prove every stream received a larger buffer.

Explicit invocation (beside the delivered openmw.exe):
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\OptimizedMW_Test.ps1 -Mode 1 -Candidate CONTROL
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\OptimizedMW_Test.ps1 -Mode 1 -Candidate I1
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\OptimizedMW_Test.ps1 -Mode 3 -Candidate D1
Use -NoPrompt only with an explicit -Mode to suppress final UI/prompts. Optional
-SavePath <file>, -RouteLabel <label> and -Cohort fresh-process-first-visit record
caller-declared workload identities; they do not select or verify the loaded save.
Other cohort labels: same-process-revisit, later-fresh-launch, unclassified.

Exact environment controls, recorded and restored by the launcher:
  OPENMW_P9_FILE_REFILL_BYTES=8192|65536 (I1 OFF|ON)
  OPENMW_P9_FILE_RESIDENT_SEEK=0|1 (I2 OFF|ON)
  OPENMW_P9_FILE_BUFFER_BUDGET_BYTES=33554432 (initial maximum; lower caps allowed)
  OPENMW_P9_CATALOG_OPTIMIZED=0|1 (D1 OFF|ON, deep capture required)
  OPENMW_P9_COMPILE_ATTRIBUTION=0|1 and OPENMW_P9_COMPILE_ATTRIBUTION_FILE
  OPENMW_P9_FILE_POLICY_ACK=1 (one startup requested-policy acknowledgement)
The first I1 ceiling is 32 MiB of actual newly owned candidate allocation charge,
including declared owned overhead. It is a trial bound, not a measured optimum.
-BufferBudgetBytes may lower it (0..33554432); cap/allocation failure keeps the
exact 8 KiB fallback. Runtime requested/effective stream-policy rows identify
which streams actually used a candidate. A requested switch does not prove use.
I2 only retains a valid window for supported seeks/tell; out-of-window operations
keep the control behavior. Assets and the 4096-byte hash algorithm stay intact.

New profile/ZIP names include -HITCHV1-CONTROL/I1/I2/I1I2/D1. RUN-MANIFEST.json
records executable/build identity, on-disk DLL and script hashes, family/candidate
hashes, applied switches/configuration, declared workload, native game PID/birth,
restoration proof, activation/fallback evidence and qualification-file identities.
On-disk DLL hashes do not establish loaded-module identity. Missing asset/save,
activation or Windows/GPU coverage remains explicitly unverified. The archived
RAW-FILE-MANIFEST.json binds the raw file bytes and excludes its own self-hash.
Captured ZIPs are never overwritten by a different run. Partial/crashed runs are
preserved, with losses and failed families visible. All existing loss, completion,
parent/dependency and writer-output checks remain in force.

Extended constrained-stream records distinguish native read/seek/tell counts,
refill policy, fallback/budget charge, checkpoint net get-pointer consumption,
putback, discarded unread bytes and unread-at-close bytes. Consumption is observed
at refill/seek/destruction boundaries, excluding explicit seek movement; it is not
a per-character service count. Close-time charges summed across stream lifetimes
are not concurrent peak memory. Returned/consumed/hash bytes overlap and are not
physical disk bytes. Extended completeness requires every successful open to have
the original totals and all four extended rows; missing families stay incomplete.
Requested-candidate streams also need their post-release file.stream_budget row.
Its current bytes/active counts are independent concurrent loads, not one coherent
instantaneous snapshot; observed active-count maxima are not continuous peaks.
The budget peak counter and per-stream lifetime charge are reported separately.

STAGED USER TESTING (after native/reader/oracle/package checks pass)
Use the prescribed fixed save/assets/config/cap/threading/window/power state and
route checkpoints. Record deviations and errors. First visit after fresh launch,
same-process revisit, and later fresh launch are separate cohorts. OS/driver
cache state is uncontrolled; a fresh process is not proof of cold caches.
Do not flush caches, change security/driver settings, alter mods or cut quality.
  Observer feasibility: at most four launches / 25 minutes gameplay, descriptive
    basic/deep x WPR OFF/ON pilot. Reuse an older cell only if its workload and
    manifest qualify; one run per cell does not establish causality.
  Old-binary/new-all-OFF parity: two launches / 10 minutes smoke. If uncertain,
    one separate paired block, at most six launches / 30 minutes, before ON claims.
  I1 basic-mode pilot: three counterbalanced CONTROL/ON pairs, at most six
    launches / 30 minutes, first visit plus same-process revisit in each launch.
    Predeclare useful-effect/no-harm margins and primary endpoint. Three pairs do
    not guarantee significance. One follow-up of at most two pairs / 20 minutes
    is allowed only for a specific unresolved question; otherwise stop inconclusive.
Stop at either launch OR time cap. I2/combined/D1 get later small targeted stages
only when their individual evidence warrants them. Do not test every candidate
against every observer combination. Assess production benefit in basic mode;
deep traces attribute work and keep their observation cost visible. Do not subtract
diagnostic cost to invent hypothetical gameplay frame times. Run-level uncertainty,
whole-frame tails/adjacent variation, content/visuals, load stalls, memory, actual
target-operation counts and complete evidence decide promotion. No gain is promised.

PowerShell-only validation: candidate-launcher-tests.ps1 and
candidate-io-capture-tests.ps1 plus compile-attribution-capture-tests.ps1 work on Windows PowerShell 5.1 and PowerShell 7,
using synthetic files and mocked launches; they start no game, WPR or UI. Preserve
the installed known reference/rollback separately. The supplied external LuaJIT
ZIP is a provenance/compatibility reference only; no runtime swap is performed.
COMPILE-ATTRIBUTION-CAPTURE.json qualifies the separate schema1/52-column family:
every row needs a unique retained CPU span with the exact frame and operation or
scheduler phase. Missing joins, partial observation tracking, metadata/format
allocation failures, writer loss or incomplete output prevent full qualification.
Selected-set age, scheduler-first-observed op/demand age and global oldest age
stay separate; unknown_capacity/allocation never become invented producer history.
Image0 metadata is not complete texture-image or GL-binding provenance. Failure
in this family preserves independently valid CPU span evidence.

HITCH TELEMETRY UPDATE (2026-09-30)
Start with mode 3 for reference hitches. It now adds per-object/cell insertion,
controller setup, scene/NIF/image/collision loading, named Lua callbacks, thread
CPU time at root scopes, and nested PostFX binding/state/draw timings.
Mode 4 is the traced temporal control; new mode 13 is ownership-only tracing.
Mode 12 remains both candidates with tracing. Hold combined testing while the
ownership performance regression is unresolved.

The p9-hitch-spans.csv timeline is opt-in, preallocated, bounded to 524288 rows,
and formatted only after workers stop. HITCH-CAPTURE.json validates parent/thread
identity, time containment, record count, finish status and explicit loss.
Short details under 0.25 ms are filtered; retained children keep their ancestors.
children_ms includes filtered children. unattributed_ms is the remaining scope,
not a named cause. Root thread CPU time uses the OS clock (coarse on Windows);
wall minus CPU is not automatically a GPU wait. Asset truncation is flagged.
Selected GL tracing additionally includes FBO binding/attachment/status, mipmap
generation and buffer binding. It still does not intercept every core GL call
or expose driver internals. A missing/invalid trace never proves no hitch.
These are diagnostic runs with overhead, not performance-promotion runs.
Modes 1/2/8/9 retain the unarmed control paths.

OptimizedMW Phase 9 - audited temporal ownership and terrain preparation

This candidate resumes the Event 161 audit from e017315a, whose engine parent
was the user-tested 651edc7b. Runtime promotion requires matched user testing.
Use START-OptimizedMW-Test.bat in the delivered OptimizedMW_Test directory.
This repair addresses the September 29 mode 9 crash, lost capture rows, saturated
resource catalogs and the custom-PostFX ownership fallback. Those repaired
captures have now been tested; composite preparation has no established gain,
and active temporal ownership regressed in the measured pair. Start with mode 3
for deeper diagnosis, then compare traced modes 4 and 13 to isolate ownership.
Hold mode 10/12 combined testing until that regression is understood.

The launcher uses the existing configuration and saves, temporarily applies the
common OptimizedMW foundation, restores settings on exit, and records hashes.
Temporal ownership, composite preparation and dynamic motion are independently
disabled in the reference mode. All modes use the repaired capture transport.

TEST MATRIX
 1 REFERENCE: retained OptimizedMW foundation, new candidates off.
 2 TEMPORAL-INPUTS: original camera/static motion and input telemetry.
 3 ROOT-CAUSE-TRACE: reference with detailed diagnostic capture.
 4 TEMPORAL-TRACE: original temporal inputs with diagnostics.
 5 MOTION-VIEW: visualize original camera/static motion.
 8 TEMPORAL-OWNERSHIP: acquired SceneView owns immutable temporal submission.
                       Compare with 2 using the same executable.
 9 COMPOSITE-PREPARE: producer-specific terrain dependency preparation.
                      Compare with 1 using the same executable.
10 AUDITED-COMBINED: ownership and composite preparation together.
11 DYNAMIC-MOTION: ownership plus supported opaque rigid/CPU rig/morph motion.
12 AUDITED-TRACE: combined candidate with detailed diagnostics.
13 OWNERSHIP-TRACE: ownership candidate with diagnostics; compare with mode 4.
Type A to display retained unpromoted HITCH controls: 6 HITCH, 7 HITCH-TRACE.

Ownership preserves the global dynamic completion barrier and normal actor
ownership. Only an acquired supported OSG 3.6.5 SceneView's immutable native
presentation and supported custom PostFX inputs use the new path. Private pass
uniforms, current point lights, UBO storage and retained target generations keep
delayed draws independent of later culls/reloads. Stereo, unknown state bindings,
unknown renderers and unsupported threading retain the DYNAMIC canvas. The CSV
reports ownership_requested, ownership_active and ownership_fallback_reason;
requested ownership alone does not prove the candidate was active. Resize and
camera resources must belong to the same submission. A temporal ConsumerFrame
is draw-local; future asynchronous consumers still need separately retired slots.

Composite preparation uses the existing incremental compile operation and a
shared discretionary preparation/bake budget. It validates exact image revision,
texture object, program, target, framebuffer and context before use. Required
terrain keeps the complete fallback. Optional work can defer; bounded age-based
progress avoids starvation. An admitted driver GL call can still exceed its
prediction or stall. Preparation does not guarantee GPU completion or savings.

Dynamic motion copies current visible opaque geometry and retains previous poses
only after successful submission. It covers supported rigid transforms and actual
CPU rig/morph deformation. Transparency, cutouts, wind, particles, custom vertex
effects, instancing and first-person coverage remain unsupported or incomplete.
Memory and traversal bounds fail back to camera/static motion.

DIAGNOSTIC EVIDENCE
Detailed tracing adds overhead. Compare performance with tracing disabled and
use separate traced runs to investigate causes. Leaf metadata includes direct
TerrainDrawable submissions and the actual submission/draw camera. Numeric GL
texture identity is recorded only at an explicitly instrumented texture apply;
uninstrumented core calls and callbacks remain unknown. The resource catalog is
bounded and records effective inherited textures; saturation/loss is explicit.
GPU query ownership uses the draw State's FrameStamp, with separate query and
writer loss counts. CSV readers use named columns and ignore status footers.
All launcher modes use the same bounded concurrent writer transport; status
sidecars record normal finish, output success and loss. WRITER-CAPTURE.json keeps
GPU/auxiliary writer failures visible without discarding complete CPU/temporal
substreams. Trace catalogs default to 131,072 truthful descriptors with hard
capacity/allocation limits. Saturation still invalidates resource attribution.

On exit the launcher restores settings/environment and creates a SHA256-verified
RAW ZIP before the bounded report. Partial/crashed captures are retained with
validity and loss status. A report timeout cannot discard the raw evidence.

DLSS / DLAA STATUS
denseDynamicMotion and dlss_ready remain false. Scene jitter, NVIDIA evaluation
and frame generation are off. The separate developer GL/Vulkan fixture qualifies
one shared RGBA8 image contract on supported Windows hardware. It is not connected
to the game and does not modify the VulkanMW renderer track. Fixture success does
not prove production color/depth/motion sharing or a playable DLSS integration.

RUNTIME VALIDATION
Use the same executable and save/location, identical assets/settings, separately
labeled first-visit/revisit runs, and whole-frame tails rather than isolated pass timings.
Test 2 versus 8 for ownership and 1 versus 9 for terrain preparation.
Compare 10 versus 2 for the combined candidate, or 10 versus 8 to isolate terrain
preparation with ownership enabled in both. Compare 11 versus 8 to isolate
dynamic motion; mode 11 also enables temporal inputs and ownership.
Exercise resize/fullscreen, cell transitions, camera cuts and moving actors.
Do not carry old benchmark claims onto this binary. No FPS gain is claimed here.

Detailed ownership and native test contracts are in the source tree under
tools/optimizedmw/phase9/TEMPORAL-OWNERSHIP.md and interop/README.md.
Frame-time consistency analysis (all modes):
  Phase9-BENCHMARK-REPORT.json now includes population standard deviation,
  variance, coefficient of variation, IQR, MAD, p01-p99.9 percentiles, tail
  concentration and consecutive-frame absolute/RMS changes. No absolute
  slow-frame cutoff is used for these metrics. It also includes contiguous
  120-frame local windows, so changing pacing is visible within a long run.
  Phase9-FRAME-PACING.csv preserves each valid frame, signed adjacent change,
  previous-61-frame median baseline, residual and robust deviation score.
  Relative spike flags begin after 15 prior samples and reset at frame gaps.
  A flag means investigate; steady oscillation is also visible in the changes
  even when no individual frame is flagged. Engine wall times do not measure
  display presentation intervals. Compare the same route/cap/warmup conditions.

Diagnostic detail revision 4:
  Mode 3 adds causal terrain job/wait links, exterior transition scopes,
  all-read stream totals, hash bytes/blocks/rewind, and Lua allocation/cache
  activity. The resource catalog uses reusable scratch while reading live
  mutable state on every call; its diagnostic wall cost remains visible.
  Lua counters describe inclusive logical allocations, not retained memory.
  See HITCH-DIAGNOSTICS.md for coverage and interpretation limits.

  Deep uses a bounded streaming queue and dedicated writer. Its 524,288 slots
  cover backlog rather than the entire run; complete captures can exceed it.
  Normal starts no hitch writer. Advanced -HitchTransport deferred retains
  the previous whole-run arena control. The normal launcher selects streaming.
  Accepted/written/pending counts, losses, queue pressure and final drain
  qualify the raw timeline separately from clock mapping and report bounds.
  Final observer/lifecycle snapshots are outside the queue. Observer counters
  include filtered scopes and selected catalog/recorder work; intervals overlap.
  Lua event decoding adds same-pass shape, bytes, outcome and delivery links.
  Payload contents, automatic GC and private driver internals remain unavailable.
  No diagnostic timing is subtracted to estimate Normal gameplay performance.

  Automatic Windows diagnostics: first-screen choice 1 (CONTROL) or 4 (ALL)
  starts the recorder, then the game, then saves one verified upload ZIP.
  For manual advanced diagnostics: right-click START-OptimizedMW-SystemTrace.bat and Run as
  administrator. Wait for recording started, then start the NORMAL test
  launcher in another window and choose A, then earlier mode 3. Use the same outdoor save,
  fixed view for about 45 seconds, and 2-3 minute route. Quit normally.
  WPR now records sequential File mode for up to 360 seconds and saves
  automatically after the exact game process exits. Launch promptly.
  Let both windows finish. Share the new mode 3 ZIP and matching SystemTrace
  ZIP, plus the engine ZIP companion .system-qualification.json when present.
  Retain the SystemTrace capture folder for any late qualification.
  For a longer recorder limit use -DurationSeconds 600. Enter stops early.
  A minimum-free-space guard may stop recording early to allow saving.
  Coverage/loss/GPU availability are qualified from retained events; success
  creating a ZIP does not establish completeness or a hitch diagnosis.
  Mode 1 remains the reference performance path. Diagnostic captures do not
  establish gameplay FPS gains or promote modes 8/9/10.

Six-group Deep diagnostic expansion:
  Custom keeps the four independently selectable mechanisms and all 16 combinations.
  Normal and Deep choose the same gameplay switches. Deep now adds per-spike
  work trees, preparation-to-first-API-retrieval lifetimes, model construction,
  physics/navigation work and waits, queue/cache retirement, and Lua allocator
  service plus explicit GC progress. See HITCH-DIAGNOSTICS6.md in the source.
  Mode 3 also arms these groups on the reference foundation.
  HITCH-DIAGNOSTICS6-CAPTURE.json reports metadata loss and unknown provenance;
  HITCH-SPIKE-REPORT.json qualifies the automatic frame trees separately.
  HITCH-SPIKE-WORK-TREES.jsonl references original HITCH-SPIKE-NODES.csv IDs.
  First retrieval is not first visible draw/GPU use. Owner release does not
  prove destruction. Automatic GC inside arbitrary Lua calls remains explicitly
  unavailable; the workload-altering LuaJIT sampler is disabled.
  Additional receipts share the bounded streaming recorder: check loss/status before
  making an exact attribution claim. No diagnostic timing cost is subtracted.
