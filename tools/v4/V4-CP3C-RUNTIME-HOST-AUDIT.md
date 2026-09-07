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
- The host consumes frame ambient/sun state and fog clear color.
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

## Deliberate fail-closed boundaries

The host rejects rather than silently approximates:

- dynamic transforms, animated actors and dynamic material overrides;
- simple-mesh populations not yet routed through the static-model path;
- local lights, sky, water and distance fog;
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
- Retained CP3B RenderCore, static asset, sort-policy and texture-identity smoke tests pass.

## Next integration step

Add a distinct, build-gated Vulkan engine bootstrap factory without disturbing the OSG viewer dependencies used by GUI,
input, world rendering and mod-visible behavior. Do not create an SDL Vulkan window inside the current hardwired OSG
`Engine::createWindow()` path. The Vulkan bootstrap should own its separate window/host construction and accept a minimal
semantic producer that publishes a static test world and frame state into `VsgRuntimeHost`. Only after that route exists
may the executable advertise `.vsgVulkan = true`, and `Auto` must remain OpenGL while any compatibility facet is missing.
