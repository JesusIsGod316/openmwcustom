HITCH REDUCTION TRIAL v1 (2026-10-01)
The launcher preserves modes 1-13 and the accepted Phase 9 foundation. Every new
I/O/catalog mechanism defaults OFF. Type H to select a separate hitch profile:
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

Diagnostic detail revision 3:
  Mode 3 adds causal terrain job/wait links, exterior transition scopes,
  all-read stream totals, hash bytes/blocks/rewind, and Lua allocation/cache
  activity. The resource catalog uses reusable scratch while reading live
  mutable state on every call; its diagnostic wall cost remains visible.
  Lua counters describe inclusive logical allocations, not retained memory.
  See HITCH-DIAGNOSTICS.md for coverage and interpretation limits.

  For additional Windows diagnostics: right-click START-OptimizedMW-SystemTrace.bat and Run as
  administrator. Wait for recording started, then start the NORMAL test
  launcher in another window and choose mode 3. Use the same outdoor save,
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
