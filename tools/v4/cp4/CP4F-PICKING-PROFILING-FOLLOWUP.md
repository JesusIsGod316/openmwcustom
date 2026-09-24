# CP4F picking / population lookup / profiling follow-up

2026-09-23. Uncommitted work on codex/cp4f-material-frame-repair, based on
efa4f3694fad904b3546e7c846c756403950f0d2. This adds to, and does not replace,
the earlier authored-material/frame-handoff batch. Preserve the earlier build
manifest and 15:32 gameplay capture as historical evidence.

## Implemented in this follow-up

- Headless Vulkan picking evaluates skinned and morphed geometry on the marked,
  synchronous intersection visitor. Previously it could intersect old buffers
  because the OSG cull traversal that normally populates them does not run.
  Masks, ignore lists, and the ordinary OpenGL intersection path are preserved.
  The regression moves actual triangles over six frames: the old query misses
  their current location, the repaired query hits, and masked queries miss.
- Static population residency indexes full chunk/model handles, including both
  generations. Rendering order remains the ordered vector, not hash order.
  Duplicate checks and replacement/removal lookups use bounded-to-population
  indexes. Dependency, placement, revision, and fence checks remain live.
  An omitted removal is rejected instead of bypassing fence retirement.
  This is not a cache that assumes all useAnim objects are static.
- Preload admission's five counters now have their own event. The old 21-field
  event exceeded the recorder's 16-field limit and lost those counters.
- Opt-in Nsight launcher: one F12-triggered capture, default 20 seconds (5-60
  allowed), no game termination, no automatic elevation, no environment dump,
  no automatic report export/duplication. Refuse less than 8 GiB free at start.
  The time limit is not a hard byte cap. Raw traces remain local and are not put
  in the normal evidence ZIP. A profiler exit code is labeled as the wrapper's,
  not independent proof of the game's exit status.

Controls are environment-variable presence switches; setting 0 still selects
the control. Change only one per same-executable comparison:

- OPENMW_V4_LEGACY_PICK_DEFORMATION_CONTROL: use old picking-buffer behavior.
- OPENMW_V4_LINEAR_POPULATION_LOOKUP_CONTROL: use linear resident lookup.
  The duplicate/removal validation indexes remain in both modes.
- Earlier frame-handoff, population-plan, preload-admission and authored-map
  controls remain documented in CP4F-MATERIAL-FRAME-REPAIR.md.

## Local validation

- MSVC Release complete Vulkan and OpenGL openmw.exe rebuilds: exit 0.
  Existing C4305 settings and YAML DLL warnings remain; not warning-free.
- Headless update/picking executable: 6/6 tests passed.
- Focused rendering CTest: 8/8 passed. The 128-group test runs indexed and linear,
  checks generations, reorder, failed commits, middle removal, shifted indexes,
  and last-use fence preservation. Existing placement/dependency tests remain.
- Full production-function pixel suite passed on RTX 5050 Laptop with Vulkan
  validation enabled, including water inputs, authored materials, LAND, sky,
  previews, shadows and normal mapping. Synthetic fixtures are not gameplay
  acceptance or proof of all mod compatibility.
- Gameplay diagnostic Python tests: 23 passed.
- Runtime diagnostic Python tests: 18 run, 17 passed, one native-fixture skip.
- git diff --check: passed (Git also reported line-ending normalization notices).

Vulkan Release remains /O2 /Ob2 /DNDEBUG. The isolated Vulkan build was relinked
with /DEBUG:FULL /OPT:REF /OPT:ICF /INCREMENTAL:NO /MAP for a matching linker PDB
and address map. Compilation did not add full /Zi source-line debug information;
do not promise source-line/inlined-frame attribution. The map contains the
captureDynamicFrameState and updateImmediateEffectRealization function symbols.
dumpbin /PDBPATH found the matching PDB.

Build logs, relative to worktree:

- build/cp4f-engine-vulkan/build/cp4f-picking-index-vulkan-build.log
- build/cp4f-engine-vulkan/build/cp4f-picking-index-vulkan-symbol-link.log
- build/cp4f-engine-opengl/build/cp4f-picking-index-opengl-build.log

## Tools and capture requirements

Verified installed commands: Nsight Systems 2026.5.1.161 and RenderDoc 1.46.
The official installers are in Downloads/OpenMW-Profilers and have valid NVIDIA
Corporation / Baldur Scott Karlsson Authenticode signatures respectively.
No additional SDK or Blender download is required now. Vulkan 1.4.357 headers,
libraries, validation and API-dump components already exist in the local deps.

Nsight status reports no administrator privileges in this agent process.
A Vulkan fixture trace attempt failed before running the target, with:
"Failed to register Vulkan extension JSON file(s). This operation requires
registry writing permissions." No performance trace was captured.

The packaged Start-CP4F-Profile.cmd must be explicitly run as administrator by
the user. It invokes the private gameplay harness with CPU sampling and Vulkan
batch tracing. Close other game instances normally. Choose New Game, reach the
slow exterior, remain at the problem view, then press F12 once. Wait at least
20 seconds and quit normally. Do not run RenderDoc or validation simultaneously.
The collector retains normal config/content ordering but writes only private
config/log/user-data; it does not expose/copy normal saves. This is not a save
compatibility acceptance run. A later explicitly copied-save test is required.

The private Test-Results/<timestamp>-gameplay-<pid>/ directory contains
openmw.log, console.log, gameplay.jsonl, runtime.jsonl, manifest.json and reports.
When Nsight succeeds, nsight.nsys-rep is in that same directory. Do not upload
raw reports automatically: inspect before sharing and preserve their executable
hash/symbol association. Profile overhead is not an uninstrumented benchmark.
The F12 path and elevated gameplay tracing still require runtime validation.

## Findings and remaining work

The 15:32-15:45 test is still unacceptable outdoors (sparse sampled exterior
frame median about 211 ms, only 13 complete samples). Those inclusive CPU
envelopes overlap and are not GPU timings or reliable tail statistics.
Non-actor evaluated capture, effect realization, snapshot publication and static
population synchronization remain investigation targets. No FPS claim is made
for the index change before a user test.

The headless Vulkan path explicitly disables legacy OpenGL postprocessing.
The saved Rafael postfx chain therefore is not causing this run's 4 FPS.
It is a compatibility gap, not a successful port.

The staged Rafael compatibility lighting shader interprets specular textures as
packed metallic/roughness/AO/SSS data (interpretation 2); the native legacy Vulkan
shader instead consumes RGB as colored specular plus alpha shininess. That is a
strong, source-backed explanation for cyan/green highlights, not a completed
PBR implementation. A faithful profile needs matching decode/color-space, BRDF,
terrain defaults, environment and preview behavior, with a legacy control.
Do not "fix" it by globally clamping tint or treating every mod's spec map as PBR.

White water still needs actual gameplay reflection/refraction/depth probes or
a RenderDoc frame. Normal/pixel fixtures passing does not establish real scene
coverage. Do not attribute every material, terrain, water and interaction issue
to one postprocessing setting.

Complete mod/save/postfx compatibility, exterior performance acceptance,
gameplay interaction parity and the remaining visual fixes are NOT completed.
No commit, push, release, archive write, or modification of normal saves/configs
is part of this follow-up.

## Exact source/test additions and edits in this follow-up

Relative to the isolated worktree; existing earlier repair edits are retained:

- apps/openmw/mwrender/renderingmanager.cpp
- apps/openmw/mwworld/cellpreloader.cpp
- components/sceneutil/deformationintersectionvisitor.hpp (new)
- components/sceneutil/riggeometry.cpp
- components/sceneutil/morphgeometry.cpp
- components/render/backend/vsg/staticpopulationresidency.hpp
- components/render/backend/vsg/vsgruntimehost.cpp
- tools/v4/cp4/update-only-tests.cpp
- tools/v4/cp4/static-population-smoke.cpp
- tools/v4/cp4/rendering-tests/CMakeLists.txt
- tools/v4/cp4/runtime-diagnostics.py
- tools/v4/cp4/test-runtime-diagnostics.py
- tools/v4/cp4/gameplay-diagnostics.py
- tools/v4/cp4/test-gameplay-diagnostics.py
- tools/v4/cp4/Start-CP4F-Profile.cmd (new)
- tools/v4/cp4/CP4F-PICKING-PROFILING-FOLLOWUP.md (new)
