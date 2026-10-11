OptimizedMW standalone real DLSS adapter fixture

This independent Windows executable qualifies an OpenGL/D3D11 texture bridge
against an actual NVIDIA DLSS super-resolution evaluation. It neither launches
the game nor replaces its executable, presentation, renderer, motion pass or
settings. Game jitter, DLSS, DLAA and frame generation remain off.

Supply a locally installed official NVIDIA DLSS SDK through P9_NGX_SDK_ROOT.
The measured cohort uses official v310.9.1, commit
374959484e79a640feaba44c93ac8cfb0a03f5b5. Configure this directory as a separate
CMake project with an x64 MSVC Release toolchain; the SDK's x64 dynamic-CRT
import/bootstrap library must be present. Put the matching signed
lib/Windows_x86_64/rel/nvngx_dlss.dll beside this fixture executable. The CMake
project does not download, install, commit or redistribute SDK components.

Run the fixture with --output <fresh absolute JSONL path> through the existing
unswitched private-desktop host, with an outer process deadline. Acquire the
Phase 9 profile mutex and check all OpenMW/Morrowind processes and human test
reservations first. It creates a window without WS_VISIBLE and never calls
ShowWindow, SwitchDesktop or an audio API. A game or performance capture may
not overlap this GPU hardware qualification. The native completion deadline is
three seconds; an uninterruptible driver API is bounded by the outer owned job.

The hardware proof checks:
- A single nonzero GL device LUID/node and exact matching NVIDIA DXGI adapter.
- Real NGX initialization/capabilities, actual installed NGX core module and
  a configured, locally pinned signed DLL search path. SDK callback logging
  records snippet loading; ordinary module enumeration does not identify the
  DLSS snippet on the tested driver and must not be presented as that proof.
- Typed shared RGBA16F color/output, R32F depth/exposure and RG16F motion.
- Four actual evaluations per generation: initial reset, retained history,
  changed color with reset, then retained history for the changed input.
- Every input pixel remains exact and every output component is initialized
  and finite. Spatial patches follow the current producer. Independently read
  D3D and GL output pixels match exactly.
- Three generations: output 640x360 -> 800x448 -> 640x360. Actual submitted
  evaluation and verification work completes before SDK feature release and
  interop/resource retirement. No slot is reused while work is in flight.

The independently selectable --gpu-transfers mode uses output 1920x1080 ->
1600x900 -> 1920x1080. It produces all typed inputs and the negative sentinel
in actual shared floating-point OpenGL framebuffers, without CPU texture
uploads. A graphics framebuffer copy consumes the NVIDIA output into an
independent private GL target. Every copied pixel must equal the shared output
and the independent D3D readback. Input-change resets and safe recreation
remain mandatory. The same no-evaluation negative control applies in this
mode. This is a real GPU producer/consumer transfer proof at 1080p, with
synthetic zero-motion images; it is not an in-game acceptance result.

The --gpu-transfers --source-textures variant adds a separate GL source owner
in the engine producer's formats: RGBA16F color, DEPTH_COMPONENT32F depth and
RG16F motion. A same-context texelFetch shader samples these private sources
into the locked, typed shared NGX destinations; no source is registered with
D3D or uploaded from the CPU. History frames translate the quadrant boundaries
by (+2,+1) then (-2,-1) GL pixels, with current-to-previous motion in render
pixels and positive Y down. Reset frames have zero motion. Every private and
shared input component is checked exactly after evaluation, including sampled
depth and both signs of motion. The importer must refuse stale frame/generation,
wrong extent and source/destination alias descriptors before submission.
Separate sources retire after actual application work and verification, across
the same three output extents. This proves the format/import step under a
private owner; it does not prove production game integration or dense motion.

Build C adds a draw-owner-only, no-wait exact-frame source lease. The owner
refuses overwrite and retirement while borrowed. Ticket, frame, view, current
context, generation and extent must match; incomplete motion, HUD-in-color or
nonpixel motion refuse before any import. Old/released tokens cannot release a
later borrow, including a later import of the same frame. A real foreign GL
context is also tested and must refuse before graphics submission. Rejection
leaves the ordinary fallback image untouched. Release follows same-context
copy submission; GL ordering protects it against subsequent producer writes.
This is not an asynchronous lease, GPU completion fence or cross-context job.

The importer and GPU consumer now restore borrowed GL state. Adversarial
sentinels install distinct read/draw FBOs, a live program, separate active/unit-0
textures, a depth-comparison sampler, viewport/scissor, distinct indexed color
masks, depth mask, wire/point polygon modes, upper-left clip origin, both depth
conventions and alternating raster enables/clip distances. State observations
do not repair it before comparison. Inputs and cross-API output must still
match exactly through actual NGX. The bridge changes only color-mask index 0;
other draw-buffer masks remain untouched. The native cohort requires the
NVIDIA compatibility context's clip-control, indexed-mask and sampler APIs.
These checks qualify this bounded GL operation; OSG state-cache integration,
active query/conditional-render boundaries and complete game motion remain
separate integration work. No new setting enables in-game DLSS.

GPU transfer submission/locking is recorded separately from verification
readbacks. Completion observation includes the native event polling policy;
these measurements do not isolate the DLSS kernel or predict gameplay FPS.
Use qualify_adapter.py on finished positive/negative artifacts to seal
provenance and test_qualification.py to exercise tampered-receipt rejection.
Build C source manifests must include borrowed-gl-state.hpp and the external
components/rendercore/synchronousinputlease.hpp dependency. Run the separate
p9-dlss-synchronous-lease-tests executable for incomplete-contract, stale-token,
overwrite, retirement and ordinary-refusal checks without a GPU/game launch.

An initial prototype inserted a redundant empty-context D3D event after
ReleaseFeature and timed out on the tested driver. Its failure is preserved.
The qualified ordering drains actual application work before ReleaseFeature,
uses the documented SDK release contract, restores the application context,
then unregisters its completed resources. There is no claim to have diagnosed
NVIDIA's internal reason for the empty-context marker behavior. The native
completion bound remains on real evaluation/copy work, and recreation/pixel
checks verify subsequent generations.

The --negative-no-evaluation fixture omits NVIDIA evaluation deliberately.
The same real output validator must reject the untouched sentinel; success
means this negative control was detected, and never an actual evaluation.

Timing fields include synthetic uploads, locks, CPU API submission, bounded
GPU completion observation and verification readbacks. They are neither
isolated GPU algorithm time nor production bridge performance. The fixture
uses synthetic images, zero jitter and controlled exposure. Direct shared-input
variants use zero motion; the private-source variant includes translation and
reset. It does not prove dense dynamic game inputs, alpha-tested/animated
coverage, production color order/orientation, UI resolve, multi-view support,
gameplay image quality, latency or any FPS improvement.

Next integration gate: owned exact-frame engine color/depth/motion leases,
including all supported dynamic surfaces and reset reasons; a synchronous
draw-owner adapter with safe ordinary presentation fallback; then actual game
visual/latency/memory/performance acceptance including full bridge cost.

Primary API contracts:
https://github.com/NVIDIA/DLSS/tree/v310.9.1
https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1/include/nvsdk_ngx.h
https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/extensions/NV/WGL_NV_DX_interop2.txt
