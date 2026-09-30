# Phase 9 temporal submission ownership

This candidate is independently selected by `OPENMW_P9_TEMPORAL_OWNERSHIP=1`.
The ordinary canvas stays DYNAMIC as the fallback. Only a verified, acquired
SceneView's immutable native-presentation submission is STATIC. Actors and the
global dynamic completion barrier are unchanged. Supported mono PostFX chains
capture private pass/root uniforms and the exact global uniform/UBO and point-light
inputs after world cull. Stereo, other thread models, unknown renderer visitors,
unknown state bindings and other OSG versions retain the fallback. The temporal
camera's frame ID alone does not prove ownership of mutable PostFX state.

ownership_requested, ownership_active and ownership_fallback_reason report the
actual acquired submission, independently of the launch mode. A requested path
that falls back preserves valid camera motion but cannot count as an ownership
performance result. The earlier custom-PostFX mode 8 capture was a fallback.

Each acquired canvas copies the pass order, flags, names, uniforms, their backing
arrays and resource references. Fx::Pass creates a new Program with copied Shader
sources for each dispatch build, so technique reload cannot alter already captured programs.
ShaderManager hot reload and global-define changes retain their existing viewer
thread stop/start protection. Candidate technique target rebuilds create fresh
textures before changing sizes or dirtying GL objects. Captured generation records
have five fixed holders: two acquired canvases, two legacy source canvases and the
current rebuild template. No retained-generation queue grows through reloads;
this bound excludes textures retained by the ordinary technique/asset cache.
Custom target history is initialized once per generation and serialized context.

The global FX state is first qualified before accepting a STATIC leaf, then its
exact placeholder StateSet is filled after world cull and before draw publication.
This captures late collected point lights. The current traversal selects provider
light arrays explicitly, even when the same visitor returns on a different frame
parity. Known UBO data receives private storage. A slot reuses its compatible GPU
buffer only after that exact SceneView's preceding draw has retired, avoiding a
new GPU buffer each frame. Unknown bindings use the fallback.
Context release visits retained owner canvases as well as the ordinary graph.
Internal source-canvas programs, FBOs and textures are released too. Context
teardown clears generation initialization; the next eligible DYNAMIC or owned
draw initializes that generation once. Switching to a DYNAMIC fallback on the
same supported context preserves already initialized target history. Unknown
or multiple-context paths keep the legacy initialization list and do not access
the shared generation marker.

The captured FX inputs also remain on the inherited state stack throughout the
canvas draw. In OSG 3.6.5, applying the leaf's final StateSet alone does not push
its uniforms and UBO bindings onto that stack. Internal PostFX applications could
otherwise restore live ancestor values. This scoped draw state contains only FX
inputs; it excludes the leaf's presentation viewport so intermediate passes keep
their own viewport. It is removed on every return and during exception unwinding.

The game-linked `p9-postfx-ownership` tests exercise the actual canvas and FX
updater, user/pass uniforms, late world point lights, private UBO storage,
retained target generations, intermediate/presentation viewports, actual
luminance passes and NIS presentation. Serial and DrawThreadPerContext cases
cover a delayed draw while the other acquired SceneView culls, repeated/skipped
frame IDs, resize/rebuild, same-generation DYNAMIC fallback, serialized native
context teardown/ID reuse and target reinitialization. The `-uniforms` variants
use scalar/array global uniforms instead of UBOs. These are synthetic production
canvas fixtures; they do not validate the user's complete modded PostFX chain.
Fresh result logs and the bounded MSVC AddressSanitizer scope are indexed in
the delivered Validation directory.

The proof uses the pinned upstream **OpenSceneGraph 3.6.5** implementation:

- [Renderer.cpp](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgViewer/Renderer.cpp):
  the two SceneViews have separate FrameStamps. `cull()` acquires a SceneView
  from `availableQueue`, completes culling and publishes it to `drawQueue`.
  `draw()` acquires that submission, finishes `SceneView::draw()`, and only then
  returns that SceneView to `availableQueue`. The queue mutex/condition ordering
  publishes the cull writes and prevents that same SceneView's next cull from
  reusing its payload during an unfinished draw.
- [SceneView.cpp](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgUtil/SceneView.cpp):
  cull installs that SceneView's FrameStamp on its CullVisitor; draw installs the
  same FrameStamp on State. The near/far-adjusted projection remains retained
  until the draw owner copies its finalized matrix.
- [ViewerBase.cpp](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgViewer/ViewerBase.cpp):
  DrawThreadPerContext culls on the main owner and retains EndOfDynamicDrawBlock.
  Other objects keep their normal dynamic completion. There is no threading
  model override, global barrier removal, fence or GL finish in this change.

The pool has **exactly two slots**, identified by the actual verified
SceneView CullVisitor, rather than `frame % 2`. Each slot retains its camera,
projection and exact scene/depth resources until the draw finishes. Repeated and
skipped frame IDs cannot select another SceneView's pending payload. Restart
with a new renderer creates a fresh owner. Same-size resize/reload, resource
replacement generations and context release invalidate temporal history. Normal
alternating same-generation depth surfaces do not reset history. A consumer must
request the exact submitted frame; a successful metadata comparison cannot
make a stale image valid. Fresh texture objects replace a resource generation;
resize/reload never dirties textures retained by a delayed draw. History commits after the actual linked shader/FBO
draw submission, never during cull or preparation.

RenderStage only observes its camera. Each acquired stage therefore retains a
private metadata camera through a user-object holder, with renderer, graphics
context, rendering cache, children and user data stripped to prevent ownership
cycles. Deferred setup reads the captured attachment map and viewport. The main
PostProcessor stage preserves its explicitly supplied primary/resolve pair,
including an empty camera attachment map; camera replacement cannot trigger
implicit setup that overwrites those resources. Transparent-depth, distortion
and first-person depth-clear callbacks also capture their exact FBO generation.
World and HUD leaf state contain private viewport overrides, covering OSG's
later insertion of the live camera viewport into SceneView local state. Native
single/threaded tests query the actual GL attachment and viewport before the
temporal draw after delaying before setup, over repeated SceneView acquisitions.

ConsumerFrame access is limited to the same serialized draw owner before its
next render/release on that context. Returning a retained texture pointer does
not lease the mutable motion target across subsequent frames. An asynchronous
GL/Vulkan/NVIDIA consumer must add its own image retirement/ownership protocol.
The independently gated interop fixture tests that distinct requirement.

The production SceneViewOwner and TemporalMotion shader are exercised by
`p9-temporal-owner` and `p9-temporal-owner-threaded`: a draw is deliberately held
until a second acquired SceneView captures another camera using the same frame
ID/parity, then real motion pixels are checked. The tests also cover skipped
IDs, fresh input generations, delayed camera attachment replacement before
deferred FBO setup, resource resizing, restart and stale consumer rejection. The adapter test
covers finalized projection, raster/FBO/viewport restoration and target release.
The separate `p9-nis-owner` test uses the production NisScaler compute shader
to check retained old/new inputs and captured settings over serial output
resizes, plus missing-compute and invalid-scale fallback. It adds a readback
barrier only for the test's CPU pixel consumer. This qualifies that scaler
contract, while full canvas composition and visual gameplay remain runtime gates.

OSG signals zero-dynamic completion at draw begin. While the second cull can
overlap a delayed first draw, the next frame may still wait for that queued draw
to start. Removing the temporal canvas dependency may therefore migrate wait
time to available/draw queue pressure or change latency. Passing the ownership
tests proves the lifetime contract, not a recovered number of milliseconds.
Performance promotion requires matched same-binary cold/warm isolated and
combined runtime measurements with whole-frame tails and latency.

# Independently gated opaque dynamic motion producer

`OPENMW_P9_DYNAMIC_MOTION=1` captures actual already-visible RenderLeaves after
the main world cull, including CPU-deformed rig and morph positions and rigid
model transforms. CPU skin/morph's two private buffers share one per-instance
identity, recreated when the source is replaced; shared asset containers are
not assigned the actor's identity. Shader cutouts retain an AlphaFunc marker
even after fixed-function alpha testing is disabled; capture rejects that marker.
Current positions and primitive indices are
copied into an immutable submission. Previous poses/projection/modelview are
retained only from a successfully submitted draw. Repeated/skipped frames,
cuts, resource/extent changes, topology changes and newly visible surfaces do
not invent previous deformation.
Only exact stock Geometry with standard DrawArrays/DrawArrayLengths or
DrawElements triangle, strip and fan primitives and valid vertex indices is
admitted. Unknown subclasses, indirect/patch and other primitive semantics use
the fallback rather than inherit a proof from a familiar class name.

The overlay is written into the existing RG16F motion surface and matches the
exact current scene-depth sample before replacing camera/static motion. It
does not change scene color, depth, quality, saves, settings or sampling jitter.
Motion is current-to-previous, in unjittered render pixels, positive right/down.
Capture is bounded to 16 MiB and 2048 surfaces per frame, 4 MiB per surface,
8192 visited leaf identities and 128 graph-parent levels; two SceneView inputs
and one submitted previous-pose set keep retention bounded. Rejected input uses
camera/static motion and increments explicit unsupported coverage.

This is a substantive opaque rigid/CPU rig/morph producer. Transparent/cutout,
custom vertex effects, GPU instancing, ambiguous shared graph paths, wind,
particle/effect and first-person coverage require their own exact contracts.
Both `denseDynamicMotion` and `dlss_ready` stay false. No NVIDIA evaluation,
scene jitter or frame generation has been enabled. Production rigid/rig/morph
pixel tests validate separate object/deformation motion with a static camera,
snapshot retention, reset/new-visibility, occlusion and conservative fallback.
Their real direct-child rig fixture also exposed and fixed an existing skin-to-
skeleton iterator overrun when the skin root preceded the skeleton in its path;
the retained MSVC AddressSanitizer evidence covers that earlier repair and the
independent cloned-actor identity. Fresh native and CI results are indexed in the
delivered Validation directory; historical sanitizer evidence is not a new
whole-game sanitizer run.
