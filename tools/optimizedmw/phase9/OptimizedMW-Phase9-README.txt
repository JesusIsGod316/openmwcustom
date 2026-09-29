OptimizedMW Phase 9 - root-cause and shared-resource checkpoint (revision 2)

Extract the complete Windows ZIP into a NEW folder. Use only
START-OptimizedMW-Test.bat. Do not overwrite your previous tested build.
Normal mods and saves remain in their usual locations.

PRIMARY MODES
1 REFERENCE: the same P8U1 COMBINED / P8G4 CULL-CPU foundation; new changes off.
2 OPTIMIZED: reference plus shared static rig/morph buffer preparation and
  traversal-local camera-inverse reuse in the existing MSOC callbacks.
3 ROOT-CAUSE-TRACE: reference plus live render-leaf and selected GL-call tracing.
4 OPTIMIZED-TRACE: identical tracing with the two new optimizations enabled.
HITCH1 orphan/refill is OFF in ALL four primary modes. None is performance-promoted.

FIRST TEST
Run ROOT-CAUSE-TRACE through the troublesome area for about 1-2 minutes and quit
normally. The launcher must report a valid live trace. Upload its ZIP even when
invalid/partial: failure is evidence, not zero cost. Use an untraced REFERENCE
and OPTIMIZED pair (same save/route/settings/wait/power state) for performance.
Do not compare traced FPS directly with untraced FPS as an optimization result.

ADVANCED MENU (A)
5 TEMPORAL | 6 COMBINED (optimized plus temporal) | 7 MOTION-VIEW
8 HITCH1 | 9 HITCH1-TRACE | 10 CULL-INPUTS only | 11 SHARED-PREP only
The old buffer-refresh experiment is retained, unpromoted, not the new normal.
Camera/static RG16F motion is preserved. Dynamic object/deformation/wind motion,
GL/Vulkan interop, and NVIDIA evaluation remain unfinished. DLSS, DLAA and frame
generation are NOT available. Scene jitter remains OFF. Native/NIS/PostFX/UI stay.

WHAT THE TRACE MEASURES
p9-draw-phases.csv: slow render leaves, with matrices/state/draw/completion CPU
  scopes, drawable identity, name, camera and source class when available.
p9-draw-phases.csv.frames.csv: totals for ALL instrumented calls, including fast
  calls absent from the sparse file. Pool/frame overflow is reported, not hidden.
p9-gl-calls.csv: selected OSG GLExtensions dispatch calls >=0.20 ms with exact
  frame/context/generation, current leaf phase, API and first scalar arguments.
p9-gl-calls.csv.frames.csv: selected API category totals, including fast calls.
p9-gl-calls.csv.totals.csv: whole-run per-API totals and maximum individual call.
p9-shared-prep.txt: opt-in shared buffer preparation engagement and bytes.
TRACE-HEALTH.json: requested/valid/complete, missing live calls and bounded loss.
The fixed trace installs BEFORE Viewer::realize starts graphics workers. An
unsupported/late/zero-view installation fails explicitly. Final health failure
still preserves raw evidence and returns launcher error code 3.

LIMITS
GL tracing wraps selected extension dispatch, NOT every OpenGL entry point.
Core calls inside prebuilt OSG (e.g. glBindTexture, glTexImage2D, glDrawElements),
render-stage setup, OS scheduling and driver internals remain outside the
selected-call split. Their time can still appear in enclosing leaf/draw scopes.
No new glFinish, glFlush, fence waits, GPU query waits or frame readbacks are added.
Timed calls may include CPU/driver blocking. Leaf and GL times overlap; NEVER add
them as independent costs. dynamic_draw_wait is a CPU safe point, NOT a GPU fence.
Slow rows are bounded first-event storage, not an unlimited or rolling capture.
Diagnostics have observer cost. Missing/dropped measurements are not zero time.

CULLING AND QUALITY
No new visibility hierarchy and no newly culled objects. Existing cell, paged,
individual-object and groundcover visibility, wind bounds and fallback stay.
The new camera-inverse reuse is traversal/thread/camera/matrix-specific, with
normal recomputation on mismatch or invalidity. No stale visibility reuse.
Normal shadows remain 3 x 2048 at distance 4096 with the established far policy.
No cascade/resolution cuts, main-view rejection of offscreen shadow casters,
new actor occlusion, whole-cascade reuse or broad render sorter is introduced.

SHARED PREPARATION
The existing context-owned compile operation can now prepare eligible STATIC
shared rig/morph attributes and index buffers. OSG still separately collects
programs/textures. Private live pose arrays are not evaluated or compiled here.
Preparation is revision-aware, bounded to 2 MiB per geometry call, and retains
normal draw fallback for unsupported, dynamic or oversized resources. This is
not a hard driver latency/memory bound or a claim of GPU completion.

PACKAGING
Settings/environment are restored before post-run processing. A per-entry
SHA256-verified RAW ZIP is created BEFORE the optional report (30-second limit).
A verified enriched ZIP replaces it only on success. The archive is beside
openmw.exe, with protected-directory fallback to the profile parent in Documents.
Raw folders remain. Crashes may lose deferred CSVs; partial captures still zip.
