# V4.0 CP3C production runtime host audit

## Status

This batch establishes a production-shaped, single-window Vulkan/VSG host behind the neutral
`RenderWorld`/`FrameRenderState` seam. It does not replace OpenMW's existing OSG/OpenGL engine host and it does not make
`Auto` select Vulkan. The compatibility gate in `components/rendercore/renderer.hpp` remains authoritative.

## Implemented contracts

- SDL remains the window, platform and input authority. `SdlVulkanWindow` supplies only the Vulkan surface/swapchain seam.
- The host owns one VSG viewer, swapchain task, main view and persistent static scene root.
- Static residents are realized and compiled before publication, then committed at a frame boundary.
- Replaced objects remain alive through their last observed GPU fence completion.
- Static traversal order follows deterministic `RenderWorld` planning even when an existing resident is replaced. This is
  required for legacy traversal/no-sort transparency compatibility.
- Resident and mutation lookups use generation-aware instance keys rather than quadratic full-list scans.
- `FrameRenderState` projection metadata now declares clip-depth range, depth direction, clip-space Y direction, near/far
  planes and infinite-far intent. This removes an ambiguity that would otherwise break depth reconstruction for postfx and
  shader mods across OpenGL and Vulkan.
- Current and previous camera matrices cross into VSG without parameter reconstruction.
- Frame production uses prepare/present/commit semantics. Swapchain skips during minimize, resize or acquisition do not
  consume a frame ID or promote an unpresented camera to temporal history.
- A minimized or temporarily zero-sized SDL pixel extent is a non-consuming skipped frame. A real semantic, realization,
  submission or presentation failure is sticky for the session, preventing unsafe reuse of a possibly submitted frame ID.
- The host consumes frame ambient/sun state and fog clear color.
- Constant point lights are delivered through an OpenMW-owned per-view storage buffer rather than VSG's inverse-square
  point-light encoding. The compatibility shader retains diffuse, ambient, specular, negative-light, actor-fade, exact
  constant/linear/quadratic attenuation, and classic/non-classic radius semantics. The packed layout is suitable for a
  later clustered-selection consumer without changing the neutral records.
- Interior fog is delivered through a separate frame-safe view descriptor. The shader implements OpenMW's planar/radial
  distance and linear/exponential curves, including `NiFogProperty` color/depth overrides, material fog disablement, and
  source-alpha/one additive fading toward black.
- VSG 1.1.15 record/submit and present `VkResult` values are checked instead of discarded.
- The completion bridge is pinned to VSG 1.1.15's exact three-buffer `RecordAndSubmitTask` ring.
- Every rejected frame or realization publishes a backend diagnostic suitable for later engine logging/UI routing.
- Startup configuration now has additive `auto`, `opengl` and `vulkan` values plus a strict/fallback control. The engine
  resolves this policy before it creates the established OSG viewer/window path.
- The production executable deliberately advertises Vulkan as unavailable until its distinct bootstrap and semantic
  producer are connected. Explicit Vulkan therefore either falls back with named missing compatibility facets or fails
  before window creation; it cannot accidentally create an OpenGL/Vulkan mixed-ownership window.
- Configuration and content discovery are now an independent automatic-compatibility facet. Existing content/config
  semantics cannot be hidden inside the broader gameplay/save/Lua qualification bit.
- `Auto` fails closed in an unqualified Vulkan-only build, and an OpenGL request may fall back to Vulkan only after the
  complete parity mask passes.
- Startup policy lives in the GLM-free `backendselection.hpp`; the OpenGL engine does not inherit semantic world/math or
  Vulkan/VSG dependencies merely to choose a renderer. `renderer.hpp` layers the heavier frame/world service on top.
- Active cells and individually addressable static references now have a backend-neutral, atomic producer. Source
  identities survive transform updates and cross-cell moves while generation-safe handles reject removed or reset state.
- Asset translation and cell publication allocate from one `RenderWorldPublisher` sequence, so later terrain, actor and
  streaming producers can share the world without independently assuming sequence 1.
- The Vulkan integration target compiles production source adapters for stable cell/`RefNum` identity, authoritative static
  placement, and the live gameplay camera's Vulkan reversed-Z/down-Y state. The adapters observe game state directly and
  do not scrape OSG nodes or expose VSG objects. They remain outside the legacy OpenGL target graph until the explicit
  production Vulkan factory owns GLM/VSG dependencies.
- Published static NIF bindings are cached by normalized winning-VFS identity plus content identity. Repeated references
  reuse one logical resource graph; a mid-epoch path/content mismatch fails closed pending a real hot-reload transaction.
- `VsgSemanticSession` gives the future engine factory one build-gated owner for the semantic world, shared publisher,
  model/cell/frame producers, distinct SDL/Vulkan bootstrap and runtime host. Reset synchronizes GPU work before changing
  epoch, and member order tears the Vulkan runtime down before source bindings.
- `MWWorld::Scene` owns an optional backend-neutral `SceneRenderLifecycle` observer and emits authoritative cell
  activation/deactivation, object add/remove/mutation, and world-reset events. `MWWorld::World::init` accepts ownership of
  that observer, allowing a build-gated Vulkan factory to attach before the first cell load while the OpenGL route passes
  no observer and gains no GLM, VSG, or Vulkan dependency. Retirement/reset callbacks cannot throw during gameplay cleanup.
- The build-gated `V4SceneRenderLifecycle` adapter uses corrected model paths from real `Ptr` state, parses the winning VFS
  NIF, invokes the accepted CP3B static translator, publishes/reuses it through `StaticModelCache`, and upserts stable
  reference identity into the active-cell producer. Missing models and semantic publication failures fail the explicit
  route. Actors, animated objects and visible light models now fail the explicit route with a sticky diagnostic instead
  of being silently omitted; their deferred compatibility facets remain incomplete.
- `V4EngineRenderBridge` consumes a backend-neutral `CameraState`, not the OSG-backed gameplay camera class. The current
  OSG camera conversion is isolated in the source adapter and can be replaced by a native Vulkan camera producer without
  changing engine/session ownership, frame history, or backend submission.
- The transitional engine-frame adapter captures the established gameplay camera, FOV, clip distances, neutral fog,
  night-eye and timing into one immutable source after world update. OpenGL reads the same neutral fog snapshot.
- `OPENMW_ENABLE_V4_VULKAN_RUNTIME` adds guarded production targets for the VSG backend, accepted NIF translator and game
  adapters and forces the archive chain through an OpenMW executable link; default/OpenGL builds retain their old graph.
- The configured application bridge maps OpenMW's current resolution, selected display, exclusive/borderless/windowed
  mode, border, minimize-on-focus-loss and disabled/enabled/adaptive vsync policy into the distinct Vulkan window. It does
  not manufacture a second hard-coded window policy, and minimized/hidden windows skip before frame history is consumed.
- Desktop window policy lives outside semantic source conversion. Headless/capture/multiview/upscaler hosts can reuse the
  same world and frame contracts without importing fullscreen or present-mode assumptions.
- `V4EngineFrameCoordinator` provides the backend-owned loop boundary identified by the donor audit: after gameplay/world
  update it captures extent, underwater state, camera, environment and timing, then submits only immutable neutral state.
  Surface skips remain retryable while source/backend failures poison the coordinator with the first diagnostic.

## Deliberate fail-closed boundaries

The host rejects rather than silently approximates:

- dynamic transforms, animated actors and dynamic material overrides;
- simple-mesh populations not yet routed through the static-model path;
- flicker/pulse/controller-driven, spot, and clustered local-light behavior;
- sky and water;
- per-instance disabled lighting;
- material/controller effects counted as unresolved runtime-context effects;
- unsupported texture bindings;
- multiple, masked, map, preview, reflection, refraction or shadow views;
- unequal render/output extents and temporal jitter;
- projections other than reversed zero-to-one depth with down-facing Vulkan clip Y.

These are nonclaims, not compatibility waivers. Their renderer compatibility facets remain incomplete, so OpenGL remains
the safe automatic backend. Explicit Vulkan selection is only for checkpoint/runtime testing until the rejected surfaces
are implemented and validated.

## Validation

- CP3C static planning/residency smoke passes, including replacement-order preservation and stale mutation rejection.
- CP3C frame completion smoke passes, including submit preflight and capacity rejection.
- CP3C camera smoke passes, including column/row mapping, double-precision placement and malformed projection rejection.
- CP3C backend-selection smoke passes, including stable setting parsing, strict/fallback behavior, configuration/content
  qualification and the unqualified Vulkan-only fail-closed case.
- The runtime host and submission bridge syntax-compile with warnings-as-errors against exact VSG 1.1.15 headers.
- The compatibility shader gate compiles plain and fully textured variants and checks both OpenMW per-view descriptor
  layouts, material fog packing, and the local-light/fog equation source contracts.
- Retained CP3B RenderCore, static asset, sort-policy and texture-identity smoke tests pass.
- Active-cell producer smoke and component tests cover transform revisioning, atomic cell moves/unloads, shared sequencing,
  missing dependency rejection and world-epoch reset.
- Static-model cache smoke covers stable reuse, no-op cache hits, shared producer sequencing, content-conflict rejection and
  generation-safe republish after a world reset.
- The normal Windows OpenGL control build remains GLM-free; CI rejects an accidental `v4semanticsource` leak into its source
  list, while the Windows Vulkan integration target separately compiles that adapter against its declared `glm::glm` dependency.

## Next integration step

Connect the configured `V4EngineRenderBridge` before `World::init` from a distinct Vulkan application route and drive it
from a backend-owned loop. The primary vsgopenmw donor confirms this ownership shape: its VSG render engine owns the SDL
Vulkan window/viewer and invokes gameplay through a per-frame callback, rather than entering the OSG viewer loop. Do not
create a Vulkan window inside the current hardwired OSG `Engine::createWindow()` path or keep a hidden OpenGL window as a
temporary dependency. The next extraction must preserve the existing input, GUI, world and mod-visible behavior behind a
backend-neutral gameplay-update boundary. Only after that explicit route exists may the executable advertise
`.vsgVulkan = true`, and `Auto` must remain OpenGL while any compatibility facet is missing.
