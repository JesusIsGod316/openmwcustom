# CPU fast paths and Vulkan occlusion follow-up

Local work on `codex/cp4f-material-frame-repair`, base
`efa4f3694fad904b3546e7c846c756403950f0d2`. Uncommitted candidate, not runtime-promoted.
The user's September 23 instruction allows CP4/5/6 work to overlap. Compatibility,
same-executable controls and runtime acceptance gates remain in force.

## Evidence motivating this batch

The September 23 17:13:57 gameplay capture produced a valid 20-second Nsight
report: 86 presentation intervals, median 224.994 ms, p95 256.366 ms. In a
representative 225.082 ms interval, the presenting thread was on-CPU for
224.291 ms; five Vulkan fence waits totalled 0.598 ms. This points to CPU-side
frame production/recording, not a sustained GPU-fence wait. These are profiled
intervals, not an uninstrumented performance acceptance run.

Matching local symbols identify mesh validation, evaluated-geometry capture,
and immediate-effect stream updating among sampled functions. Most leaf samples
remain unresolved: no precise exclusive function percentages are claimed.
Sparse engine diagnostics show roughly 2,000 evaluated draws and 34.8 MiB of
geometry copied at publication. Inclusive stage envelopes overlap and must not
be added together. Detailed notes and provenance remain beside the raw capture
in `nsight-analysis.md` and `nsight-analysis-evidence.json`.

## Implemented mechanisms

All three switches are unset by default. Set a switch to `1` to opt in; remove
it to disable. Presence controls follow the existing process-launch convention.

| Switch | Mechanism | Safety boundary |
| --- | --- | --- |
| `OPENMW_V4_PARALLEL_EFFECT_PUBLICATION` | Up to three persistent dedicated workers plus the caller copy and validate independent evaluated draws. Small batches stay inline. | CPU-neutral immutable input; no live OSG, Lua, world mutation or GPU submission on workers. Synchronous completion and exception propagation; serial fallback if threads cannot be created. |
| `OPENMW_V4_VALIDATED_EFFECT_REUSE` | Reuse validation already performed by the immutable deep-copy owner. | Owner plus checked draw index, never an unverified caller boolean. Destination stream checks and resident layout/dependency/fence checks remain. |
| `OPENMW_V4_BULK_EFFECT_STREAMS` | Compare packed streams in bulk, convert only changed streams, and skip unused opaque sorting-bound work. | No raw memcpy into nontrivial VSG vectors; padded layouts use component conversion. Dynamic sorted and billboard bounds still update. |

No animation or controller evaluation is skipped. No geometry is removed. No
save format, mod loading order, material decoding or postprocessing semantics
are changed by this batch. The prior picking/material/ownership repairs remain.
The legacy frame-handoff control naturally disables owner validation reuse.

Diagnostics record actual publication worker count, owner validation reuse and
bulk-stream mode, alongside existing stage timing and residency events. The
launch manifest records all effective environment controls, executable hash,
shader package identity and requested CPU profile.

## Test build usage

Package: `C:\OpenMW-CPU-Test` (separate from prior tested builds and captures).

- `Start-CPU-Fastpaths.cmd`: all three optimizations enabled.
- `Start-CPU-Control.cmd`: same executable, only these three optimizations off.
- `gameplay-diagnostics.py launch --cpu-fastpaths parallel|validated|bulk`:
  isolate one mechanism. The default `inherit` preserves explicit environment
  controls; `all` and `control` provide reproducible combined comparisons.

Ordinary candidate/control launches need no administrator privileges or Nsight.
They retain a visible command window when double-clicked and pause at exit.
Use New Game; the helper isolates writable user data and does not copy or expose
normal saves. Evidence is under `Test-Results/<timestamp>-gameplay-<pid>/`.
This does not validate existing saves or all mods.

Compare fresh processes at the same ship/exterior/Office positions and view.
Keep settings and diagnostic mode identical. Exercise moving NPCs, direct
crosshair interaction, appearance preview, animated objects and cell revisits.
Use dense frame-time measurement for tails; the engine's sparse diagnostic
samples cannot establish p95/p99. Also compare with diagnostics off before
promoting performance. Do not run Nsight and RenderDoc simultaneously.

## Occlusion is a separate unfinished Vulkan integration

Source audit: `Engine::frame` calls native Vulkan presentation instead of
`mViewer->renderingTraversals()`. `V4UpdateOnlyViewer` explicitly rejects legacy
rendering traversals. Existing MSOC setup/query callbacks are OSG CullVisitor
callbacks in `apps/openmw/mwrender/occlusionculling.cpp`; therefore their presence
in source/settings is not evidence that they cull native Vulkan submissions.
The native realizer also needs an explicit audit of static/group frustum bounds;
pixel clipping is not CPU draw rejection, and frustum culling is not occlusion.

Preserve the useful coarse paged/groundcover foundation. Historical constraints:

- Broadened individual-building occluder work was rejected.
- V3.23 stronger same-frame parallel MSOC budgets regressed cull/render cost.
- V3.24 documents private worker ownership, no shared preload queue for critical
  work, exact-generation results and visible/fail-open behavior for stale or late
  results. The present CPU pool follows the dedicated-worker lesson but does not
  claim to port MSOC.

Next integration order:

1. Establish conservative per-view static/group bounds and native frustum
   rejection. Include every GPU instance placement and double-precision origin;
   never reuse undeformed bounds for animated geometry or billboards.
2. Expose immutable, exact-active terrain/static occluder and coarse candidate
   snapshots to the native visibility owner. Reuse proven MOC kernels without
   invoking a fake/general OSG cull traversal.
3. Apply visibility only to the intended view's draw traversal. A hidden main-view
   object may still cast a visible shadow or appear in reflection/refraction.
   Preserve world updates, physics, scripts, picking and auxiliary views.
4. Use private raster buffers and exact camera/world/generation matching;
   missing, late, stale or invalid results must keep geometry visible. Do not
   revive aggressive budgets without new evidence of a net win.
5. Test near-plane/camera-inside cases, teleports, cell changes, moving doors,
   foliage/alpha, terrain holes, reflection and shadow coverage. Require no false
   occlusion, lower net CPU/draw work, and no worse severe tails before promotion.

This batch does **not** implement or enable Vulkan MSOC. Rafael/PBR interpretation,
white-water gameplay artifacts, complete postfx/mod/save compatibility and runtime
picking acceptance also remain open. The user did not test NPC interaction in
the profiled run.

## Validation and files

CPU parity tests cover serial/parallel ownership, independent mode combinations,
animation, sorted and opaque draws, billboards, empty optional streams, padded
layouts, malformed payloads, duplicate identities, exceptions, checked owner
indices and rejection before partial destination writes.

Initial repeated Release CPU microtests (not FPS measurements): a 24.6 MB,
2,000-draw publication fixture including its parity check took about 18.4-18.6 ms
serial versus 6.4-6.9 ms parallel. Forty unchanged 300,000-vertex updates took
about 285-295 ms on the old full-validation/scalar path, versus 18-21 ms opaque
or 34-36 ms sorted with both reuse and bulk mode enabled. Publication order and
allocator/cache state can influence these microtests; no whole-game gain is
inferred from their ratios.

Exact files added/edited in this batch (other dirty files predate it):

- `components/rendercore/boundedparallelfor.hpp`
- `components/rendercore/effectframe.hpp`
- `components/rendercore/framerenderstate.hpp`
- `apps/openmw/mwrender/v4enginerenderbridge.hpp`
- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `components/render/backend/vsg/immediateeffectrealizer.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgsemanticsession.cpp`
- `tools/v4/cp4/frame-handoff-tests.cpp`
- `tools/v4/cp4/effect-stream-fastpath-tests.cpp`
- `tools/v4/cp4/rendering-tests/CMakeLists.txt`
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- `tools/v4/cp4/Start-CPU-Fastpaths.cmd`
- `tools/v4/cp4/Start-CPU-Control.cmd`
- this note.

Final build hashes, test results and package file hashes are recorded in the
package's `cpu-fastpaths-build-manifest.json`. No runtime promotion, commit, push
or replacement of a previous test installation is implied by a successful build.
