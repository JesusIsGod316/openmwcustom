# OptimizedMW Phase 9 repair checkpoint (2026-09-30)

Current branch: `codex/optimizedmw-phase9-audited`. This repair starts from
`f412af6e9819547ca5b1da6d3004695b13eee701`, the September 29 tested package.
Current user instructions govern; archived constraints are historical reference.

The mode 9 resource-only compile lifetime is explicit and the shared cache
worker does not dereference an absent scene node. Actual cache-worker and
terrain-composite integration fixtures cover resource-only and ordinary jobs,
completion, cancellation, abandonment, release and required fallback.

Supported custom PostFX uses acquired SceneView snapshots with private uniforms
and UBOs, retained target generations and scoped inherited FX state throughout
nested draws. Same-generation DYNAMIC fallback preserves history. Serialized
context teardown releases source and owned internals and reinitializes targets.
Unsupported ownership paths retain the ordinary DYNAMIC canvas.

All Phase 9 modes use bounded capture transport with explicit loss/completion
status. Deep trace descriptors default to 131,072, preserving complete identity
joins. The launcher retains independent valid channels, verified raw archives
and actual child-process exit codes. This changes evidence delivery, not a
benchmark conclusion.

The complete local MSVC game compiles. All 16 focused native tests pass on the
local graphics hardware. Four partial-TU MSVC AddressSanitizer PostFX variants
pass with zero GL errors: serial/threaded, UBO/scalar-array globals. Final full
game tests, CI and installed-package results are indexed in the delivered
Validation directory, alongside corrected development failures and identities.
Those final results, rather than older sections below, define the delivery state.

Start user testing with 1/9, then 2/8, then 10. Gameplay stability, visual
acceptance and controlled performance measurements remain pending. DLSS
evaluation, scene jitter and frame generation remain off.

The following September 29 notes are retained historical context.

# OptimizedMW Phase 9 telemetry + DLSS-input continuation (2026-09-29)

Active staging branch: `optimizedmw/phase9-telemetry-dlss`.
Parent: current `optimizedmw/phase9`.

The user ROOT-CAUSE-TRACE capture established that dynamic_draw_wait is primarily
downstream of draw-thread stalls. Large TerrainDrawable State::apply events are
concentrated in RefractionCamera/SceneCam and include synchronous compressed
texture realization. A persistent unnamed osg::Drawable also dominates many slow
draw submissions. OPTIMIZED-TRACE separately crashed inside nvoglv64.dll while
the new static actor-resource prewarm was enabled; that experiment is rejected
for this continuation.

## This continuation

- Static actor-resource prewarm is removed from RigGeometry/MorphGeometry
  production hooks and is not selectable by the launcher.
- TerrainCompositeMapRenderer and its PRE_RENDER camera now have explicit
  identities. Direct composite telemetry separates FBO setup, State::apply and
  inner draw work, queue state, required work and yields.
- Slow leaf rows retain node-path/owner identity, geometry size, camera class and
  largest StateSet texture image paths/bytes. Camera totals cover scene,
  refraction, reflection, shadow and terrain-composite views.
- Selected GL hooks retain up to eight integral arguments and an in-memory
  entered-call breadcrumb for crash-dump attribution. No GL waits/flushes were
  added.
- Existing asynchronous GPU timestamp telemetry is enabled in trace/temporal
  launcher modes and now covers terrain composite work and the temporal motion
  pass.
- TemporalMotion publishes a consumer-ready frame: RG16F motion, current/
  previous/inverse view-projection matrices, history/reset state, jitter
  metadata and render/output extents.
- p9-temporal-inputs.csv reports scene color/depth/motion/matrix readiness.
  Dense dynamic actor/morph/grass motion remains false, so dlss_ready remains
  false and scene jitter remains off. NGX/Streamline evaluation is not yet
  integrated.
- Temporal modes also emit p9-dlss-capabilities.csv once per GL context. It
  records GL_EXT_memory_object[_win32], GL_EXT_semaphore[_win32] and the actual
  import/storage/wait/signal function pointers needed by the planned Vulkan
  shared-image bridge. This is capability evidence only; it does not create a
  Vulkan device or initialize NGX.

This is a substantive diagnostic + temporal-input checkpoint, not a telemetry-
only performance promotion and not a working DLSS release. Full Windows/package
CI and user runtime validation are still required.

Validation trigger after chat-stream recovery: branch contents re-read and CI requested from this exact checkpoint.

---

# Phase 9 root-cause and static-prewarm checkpoint (2026-09-29)

Publication base: bb4876a6ea287745c8f535f5525c5381a631a24c.
The complete base tree was recovered and verified as
78ac535aa941cb703707a42fc946910fc0b03c61 before editing. The later named
RootCause source ZIP from the interrupted turn was not available in the active
runtime or indexed file search, so these changes were implemented from the
verified published source, not claimed restored from that missing archive.

See CULLING-AUDIT.md and the installed README for exact scope and limitations.
The lifecycle bug is fixed at Engine pre-realize, not by stopping running draw
threads. New startup, static-buffer and dispatch-contract tests complement the
retained native/sanitizer/pixel gates. A pinned Windows header compile is added
before the long engine compile. Source validation is not hardware hitch proof.

The independent optimization is shared-static rig/morph buffer preparation.
There is no second culling hierarchy, new DLSS evaluation, dynamic motion-vector
implementation, or promotion of the old orphan/refill experiment. Current source
supports the same camera/static temporal input pass with scene jitter off.

Local source/tests and the Windows/package run are recorded in the release
checkpoint/archive. Never infer a finished binary or performance improvement
from this document's presence.

---
## Retained implementation history (superseded where the checkpoint above differs)

# OptimizedMW Phase 9 — first implementation slice

Publication base: `8d2ceac87eb263bc4c49dddc257abe1b84b35ef0` on
`optimizedmw/phase9`, following the successful P8U1 build at `72a19330`.
The prior 25-file checkpoint based on `3d80eb3` was restored byte-for-byte and
applied to the full authoritative 8d2ceac tree; the extra base changes are the
source/SDL-header archive workflow only. No implementation was reconstructed
from memory, and no synthetic partial-recovery Git history is a push target.
Status: source integration and local native preflight complete; consolidated
Windows/PowerShell/package validation is still required. Not runtime/performance
accepted and not a working DLSS release.

## Implemented

### Private rig/morph vertex-buffer refresh experiment

`DynamicStream::install` is wired into the existing private internal geometries
created by both `RigGeometry` and `MorphGeometry`. It preserves their two-buffer
CPU design and OSG's dynamic-draw safe point. For eligible dirty, already-created
VBOs it requests fresh storage behind the same GL name, then refills ALL arrays
in that private VBO before ordinary drawing. It does not touch shared asset
buffers, index buffers, live CPU arrays, or global OSG threading configuration.

Admission is at most 4 MiB per buffer and 32 MiB per frame/context. Cold/clean,
rebound, changed-layout, unsupported, over-budget, and OSG sentinel-revision cases
use normal OSG handling. These limits bound admitted requests, not physical
in-flight driver allocation. Orphaning is an experiment, not a guarantee that the
driver never stalls. It is not a fence-owned ring, GPU skinning, actor prewarming,
or a demonstrated solution for every hitch.

### Bounded draw/state attribution

A diagnostic-only OSG 3.6.5 RenderLeaf/CullVisitor implementation preserves stock
matrix, state-graph, draw and dynamic-completion ordering. It distinguishes CPU
matrix, inherited-state application, drawable submission, and completion scopes.
Metadata is copied BEFORE dynamic completion can release the update thread.
Pooled leaves and fixed slow-record arrays avoid growing draw-time containers;
pool/row overflow is explicitly reported and uses normal drawing, not omission.

The trace is startup-only, refuses unknown cull visitor classes or installation
while draw threads run, and is initially mono. CPU timers include possible driver
waits; they are not GPU timers. RenderStage setup and graphics-thread swap are
outside this trace. Native performance comparisons must leave tracing disabled,
or separately quantify its observer overhead.

### Real camera/static motion integration

The existing common temporal history now has a production `TemporalMotion`
consumer, hooked through `PingPongCull`, `PostProcessor` and `PingPongCanvas`.
The cull-owned projection is retained and copied by the draw owner after OSG can
finalize near/far. The pass outputs render-resolution RG16F current-to-previous
motion with top-left pixel conventions. A debug display is independently opt-in.

Camera identity/mode/cut, world category, lens, extent, frame gap and invalid-input
handling protect history. Changes only in the projection's depth coefficients
do not unnecessarily reset it. Ordinary exterior cell crossings are not treated
as cuts. The adapter restores framebuffer, viewport and raster state, rejects
input/output aliasing, and leaves normal color/PostFX/NIS/UI untouched unless the
explicit debug view is selected. Missing shaders keep the normal renderer.

Scene jitter remains OFF until a reconstruction consumer exists. The common
Halton implementation remains tested, but deliberately jittering an otherwise
unreconstructed game would degrade its image. Status always marks this
camera/static-only field as NOT complete dynamic motion. All four actual motion
and debug shaders were added to the runtime CMake staging list and verified after
the pinned PBR overlay was applied.

## Startup controls

All new behavior is off by default. These are process-local engineering controls,
not permanently written user settings:

| Variable | Effect |
| --- | --- |
| `OPENMW_P9_DYNAMIC_STREAM=1` | Private VBO refresh candidate |
| `OPENMW_P9_TEMPORAL_INPUTS=1` | Generate camera/static motion without changing normal color |
| `OPENMW_P9_MOTION_VIEW=1` | Generate and display the motion-debug view |
| `OPENMW_P9_DYNAMIC_TRACE_FILE=<path>` | Bounded private-stream preparation/draw trace |
| `OPENMW_P9_LEAF_TRACE_FILE=<path>` | Bounded state-versus-draw trace |

The installed Phase 9 launcher now owns these variables after clearing inherited
experimental flags. It has REFERENCE/HITCH/TEMPORAL/COMBINED plus MOTION-VIEW and
paired REFERENCE-TRACE/HITCH-TRACE diagnostic modes. All arms retain the identical
P8U1 COMBINED foundation; tracing and debug presentation stay off in performance
arms. The same single public START-OptimizedMW-Test.bat validates the retained
PBR and all four temporal shader hashes. No extra public BAT was added.

Packaging creates and checks every raw archive entry by SHA256 BEFORE running
a child-process report (30-second deadline). Partial/crash captures retain their
identity and list missing expected evidence rather than refusing to zip. Only a
verified replacement can replace a verified raw ZIP. Protected install folders
fall back to the profile parent. Report failures/timeouts retain the raw archive.
Windows PowerShell 5.1 and PowerShell 7 fixtures cover these behaviors in CI;
local C++ tests are not claimed as PowerShell validation.

## Completed validation

- Four native tests passed, including real OpenGL buffer readback through 200
  revisions, current/previous camera-motion pixel checks, and exact control versus
  traced pixels in BOTH SingleThreaded and DrawThreadPerContext.
- The same four tests passed under Clang ASan/UBSan. The existing narrow
  `libGLX_mesa.so.0` process-lifetime leak suppression remains; this is not a
  global leak-disable or ThreadSanitizer result.
- The three previously implemented temporal core/input/GPU tests still pass.
- Changed production RigGeometry, MorphGeometry, PingPongCanvas, PingPongCull,
  PostProcessor and Camera translation units ALL compile with real OSG/MyGUI/
  Bullet and the pinned SDL3 headers (8e37db5e797b6167f3a00d697d816a684bd259c7).
  The previously missing SDL SDK blocker is closed, without stubs. The actual
  TemporalMotion.cpp is compiled, linked and executed in the pixel test.
- P9_REQUIRE_GAME_SDK=ON adds those six real integration translation units to the
  focused native CI target. This is still not the complete game link.
- OpenGL builds now explicitly consume GLM. If an older dependency bundle lacks
  a GLM package, CMake fetches only the official 0.9.9.8 headers at exact commit
  bf71a834948186f4097caa076cd2663c69a10e1e, without adding a runtime DLL or invoking
  that old project's CMake policies. Existing installed glm::glm is reused.
- Shader staging verifies 87 files, preserving the pinned PBR overlay and exact
  bytes of all four new motion/debug shaders.
- Focused source contract passes. Exact modified-file preimages are checked
  against live GitHub blob identities; recovery/patch verification is recorded
  separately in the source checkpoint.

The two mistaken Geometry-reference dereferences in the initial rig/morph hooks
were caught by real production-TU compilation and corrected. The original failed
compile logs remain in evidence. Tests are authored fixtures, not Morrowind asset
QA, NVIDIA hardware timings, or a full game executable/link test.

## Remaining before a playable build / working DLSS

1. Run the consolidated native/PowerShell/Windows/installed-package gate. Local
   production compilation and menu/packaging integration are implemented; their
   complete Windows validation and user gameplay acceptance remain separate.
2. Validate temporal input behavior in the real modded game, including alternate
   views, first-person bodies, near/far changes, camera cuts and resize.
3. Add correctly owned previous/current transforms and deformation, dynamic
   motion overlays, grass wind and relevant transparent/effect handling.
4. Validate same-GPU OpenGL/Vulkan sharing and NVIDIA runtime initialization;
   integrate actual DLSS evaluation only after coherent temporal inputs.
5. Use new draw attribution to decide which residual first-use, buffer, state,
   or presentation mechanism merits the next intervention. Do not assume the
   VBO candidate removes all observed stutters.
6. Benchmark whole-frame tails/clusters, median, GPU time, memory and latency
   on the user's matched RTX 5050 workload. No performance promotion yet.

DLSS, DLAA, dense dynamic motion, GPU actor deformation and the Vulkan/NGX bridge
are NOT working features of this checkpoint. Frame generation is out of scope.
Clockwork/VulkanMW data-ownership principles inform the design; no wholesale
renderer code has been imported. The newer archive events 155/156/157 belong to the
separate VulkanMW track and are not overwritten.
