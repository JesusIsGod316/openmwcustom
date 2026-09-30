OptimizedMW Phase 9 - audited temporal ownership and terrain preparation

This candidate resumes the Event 161 audit from e017315a, whose engine parent
was the user-tested 651edc7b. Runtime promotion requires matched user testing.
Extract into a NEW directory and use START-OptimizedMW-Test.bat.

The launcher uses the existing configuration and saves, temporarily applies the
common OptimizedMW foundation, restores settings on exit, and records hashes.
All three new mechanisms are independently disabled in the reference mode.

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
Type A to display retained unpromoted HITCH controls: 6 HITCH, 7 HITCH-TRACE.

Ownership preserves the global dynamic completion barrier and normal actor
ownership. Only an acquired supported OSG 3.6.5 SceneView's immutable native
presentation uses the new path. Custom mutable PostFX, stereo, unknown renderers
and unsupported threading retain the normal DYNAMIC canvas fallback. Resize and
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
Test 2 versus 8, 1 versus 9, then 10 and the isolated 11 motion candidate.
Exercise resize/fullscreen, cell transitions, camera cuts and moving actors.
Do not carry old benchmark claims onto this binary. No FPS gain is claimed here.

Detailed ownership and native test contracts are in the source tree under
tools/optimizedmw/phase9/TEMPORAL-OWNERSHIP.md and interop/README.md.
