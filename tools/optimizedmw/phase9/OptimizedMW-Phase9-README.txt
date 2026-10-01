HITCH TELEMETRY UPDATE (2026-09-30)
Start with mode 3 for reference hitches. It now adds per-object/cell insertion,
controller setup, scene/NIF/image/collision loading, named Lua callbacks, thread
CPU time at root scopes, and nested PostFX binding/state/draw timings.
Mode 4 is the traced temporal control; new mode 13 is ownership-only tracing.
Mode 12 remains both candidates with tracing. Hold combined testing while the
ownership performance regression is unresolved.

The p9-hitch-spans.csv timeline is opt-in, preallocated, bounded to 131072 rows,
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
Use the same executable and save/location, identical assets/settings, separate
cold and warm runs, and whole-frame tails rather than isolated pass timings.
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

Diagnostic detail revision 2:
  Mode 3 now splits Lua worker/script initialization, cache lock waits,
  VFS/physical file access, archive decompression, controller setup and
  SceneView acquisition versus traversal. It records native Windows thread IDs
  and QPC calibration for correlation with a Windows system trace.
  HITCH-PHASE-TOTALS.csv and HITCH-REMAINING-SCOPES.csv expose retained phase
  exposure and residual intervals. Nested/cross-thread timings overlap.
  Optional: right-click START-OptimizedMW-SystemTrace.bat, Run as administrator.
  Start the game from the NORMAL test launcher in a separate window, mode 3.
  Quit after a short hitch reproduction, then press Enter in the recorder.
  Share the mode 3 ZIP and the matching ETL/metadata under SystemTrace.
  The recorder uses bounded 320 MiB circular buffers. Stop promptly so early
  events are not overwritten. ETL completeness must be checked during analysis.
  See HITCH-DIAGNOSTICS.md for exact coverage, limitations and correlation.
