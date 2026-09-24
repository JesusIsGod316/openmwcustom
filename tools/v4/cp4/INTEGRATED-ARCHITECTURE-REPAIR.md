# Integrated Vulkan architecture candidate — 2026-09-24

This is an uncommitted development candidate, not a promoted performance result.
Base: efa4f3694fad904b3546e7c846c756403950f0d2, codex/cp4f-material-frame-repair.

Eight independently selectable mechanisms extend the previous exterior repair:

- Persistent, guarded object binding plans: reuse topology and compute transforms
  once per node; unknown traversal/deformation retains the original capture path.
- Guarded material-value translation: exact consumed values, raw uniform edits,
  mutable attributes, texture semantics, and winning VFS identity are checked.
- Immutable resident read-sharing across in-flight frames: only identical private
  snapshots/material/placement may share; changed data retains fence-safe versions.
- Allocation-free bounded pipeline-audit deduplication: collisions fail open;
  preserve every pipeline validation while avoiding the prior hash-set regression.
- Two-worker independent-view recording: water reflection/refraction and main
  graph record concurrently, join before one ordered submission/fence ring.
- Coarse per-view population frustum rejection: reflection, map, and shadow
  traversals use their own frusta; main-camera occlusion never hides other views.
- Startup-only caching of named opt-in feature flags; the old live-environment
  lookup remains the control. Avoid repeated Windows CRT environment searches.
- Experimental bounded parallel metadata reads for known physical texture
  providers, deduplicated by backing file/archive, refreshed every capture.
  Unknown providers and errors retain their original checked path. This mechanism
  has NOT demonstrated a runtime gain and must not be promoted merely for passing
  tests; it remains separately selectable for investigation.

Direct executable launch leaves these flags off. Start-Architecture-Repairs.cmd
selects all repairs; Start-Architecture-Control.cmd preserves the prior exterior
mechanisms on the SAME executable. Neither changes the user's normal settings or
copies normal saves. Unknown mutation paths remain conservative.

The bounded local benchmark starts directly in Seyda Neen with a temporary Lua
script, measures after 15 seconds, and requests a normal exit at 45 seconds. It is
not the user's exact camera/route and cannot prove complete mod/save compatibility.

## Validation and outcome

Production MSVC Release builds passed for both Vulkan and OpenGL configurations.
The rendering fixture build and all 22 CTest configurations passed. Headless GPU
pixel fixtures passed with the installed Vulkan validation layer, parallel-view
recording, per-view frustum gates and flat pipeline audit enabled; no VUID or
validation-error messages were found in `architecture-final-pixels.log`.
Python checks: gameplay 29/29, configuration 17/17, runtime 17 passed/1 skipped.
Runtime blocker contracts and generated-output materialization verification pass.
The source-contract check was adjusted to recognize the RAII capture scope now
created through a timed lambda; its lifetime still spans the entire capture.

These are correctness/build results, not performance acceptance. The eight-path
candidate is `C:\OpenMW-Architecture-Test-4`; its build manifest pins the compiled
source state. Later research/benchmark-driver edits do not change that executable.
No commit/push or normal config/save change was made. Test-3 is an incomplete
staging attempt, explicitly marked DO NOT RUN, and has no valid build manifest.

The same-executable private exterior experiment still has a large OpenGL gap.
See [reference review and exact initial results](VULKAN-REFERENCE-REVIEW-2026-09-24.md).
Repeated runs vary; initial frame-duration data uses the engine's 200 ms-clamped
step. A corrected monotonic-clock Lua fixture is now in source but has not yet
been runtime validated. No new user playtest is requested for this candidate.

No new Rafael .omwfx/F2 compatibility or complete native producer migration is
claimed. The guarded binding plans still inspect live source state, and terrain
occlusion did not reject any groups in these startup-scene samples.
