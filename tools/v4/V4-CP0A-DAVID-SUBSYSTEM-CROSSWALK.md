# V4.0 CP0A — David P VSG/Vulkan Subsystem Crosswalk

Status: **CP0A source-audit handoff**  
Authority: final materialized V3.25 semantics on this branch  
Primary implementation donor: David P `sdl3_vsg_gl_math` archive SHA-256 `39c34e63913354bea45d813b9407932e4117468403e1b0cb17e971b46672d490`

This crosswalk is intentionally narrower than a feature checklist. It identifies where David's mature VSG implementation can be transplanted, where it must be adapted to final V3.25 semantics, and where its ownership model must stop at a transitional boundary.

## 1. Backend bootstrap and device layer — PORT_DIRECT pattern

Primary donor files:

- `components/render/backend/vsg/vsgrenderer.*`
- `components/render/backend/vsg/vsgsdlwindow.*`
- `components/render/backend/vsg/spvcompiler.*`

Port as a narrow implementation pattern:

- SDL3 Vulkan surface creation;
- Vulkan instance/device/swapchain setup;
- VSG viewer/window wiring;
- command graph/render graph bootstrap;
- compile manager/deletion/resource-lifetime helpers;
- SPIR-V compilation plumbing;
- GPU timing/per-pass instrumentation;
- Windows Vulkan surface support.

Do **not** port David's common `components/render/IRenderer` contract as the V4 common API. It exposes `osgViewer::Viewer`, `osg::Group`, `osg::Camera`, `osg::Node` and therefore preserves OSG ownership above the backend boundary even when the active backend is Vulkan.

V4 rule: SDL/window/device/present objects are backend-private. Common code receives only renderer-semantic state and stable logical handles.

## 2. RenderingManagerVsg — PORT_ADAPT / transitional orchestration only

David's `RenderingManagerVsg` is valuable because it demonstrates that essentially the entire OpenMW gameplay-facing rendering surface can be made to drive a Vulkan renderer: object lifecycle, projectiles, transient lights, sky/weather, fog, cells, terrain, water, ray queries, effects, actor animations, maps, settings, camera state, projection offsets and debug rendering.

However it is **not** the final V4 ownership seam.

Reasons:

- it still mirrors the giant `RenderingManager` method surface rather than publishing a compact renderer-state contract;
- it retains transitional OSG-returning methods and compatibility objects;
- it writes directly into live VSG objects/UBOs;
- it lets game-side calls reach backend-specific state rather than first updating a persistent neutral `RenderWorld`.

Use it as an implementation lookup table when bringing features online. Do not make it the common base class for V4.

## 3. ObjectsVsg / VsgWorldBridge — PORT_ADAPT implementation, REJECT ownership model

Primary donor files:

- `apps/openmw/mwrender/objectsvsg.*`
- `apps/openmw/mwrender/vsgworldbridge.*`

Useful donor behavior:

- stable game-object key to placed-render-object mapping;
- model caching by path;
- create/move/remove/clear lifecycle;
- cell association tracking;
- NPC equipment in-place update before full rebuild fallback;
- runtime compile of newly attached equipment subgraphs;
- uncached animated effect placement;
- particle lifetime tracking by object;
- actor proxy fallback.

The long-term ownership model is rejected because the bridge maps game identities directly to mutable VSG nodes and backend object IDs.

V4 translation:

`game semantic object -> InstanceHandle / ChunkHandle update -> RenderWorld -> backend placement/residency`

No `vsg::Node*`, VSG object ID, `MWWorld::Ptr`, `LiveCellRefBase*`, or pointer-derived key may become a persistent common-renderer identity.

## 4. NIF loading and material conversion — PORT_ADAPT

Primary donor files:

- `components/nifvsg/nifvsgloader.*`
- `components/nifvsg/nifvsganimation.*`
- `components/nifvsg/nifvsgcontrollers.*`
- `components/nifvsg/nifvsgskin.*`
- `components/nifvsg/nifvsgmorph.*`
- `components/nifvsg/nifvsgparticle.*`
- `components/vsgrender/vsgmaterialdefines.*`

Strong donor value:

- working static, rigid, skinned and morphed NIF conversion;
- bone maps and bind transforms;
- animation/controller conversion;
- VSG mesh/material/pipeline creation;
- alpha blend/test and two-sided handling;
- diffuse + normal/specular auto-map support;
- instanced geometry path;
- particles and animated effects.

Required V3.25 precedence:

- recursive root `RCN` collision-node semantics stay authoritative;
- current material/PBR/alpha semantics stay authoritative;
- current animation source ordering/event semantics stay authoritative;
- current shader/postprocess compatibility remains authoritative;
- marker/hidden/collision meshes must preserve game/physics semantics even if render geometry is skipped.

NifVsg should ultimately emit immutable logical mesh/material/skeleton payloads (or backend resources derived from them), not become the owner of game animation state.

## 5. NPC assembly and animation — PORT_ADAPT with strong semantic separation

Primary donor files:

- `apps/openmw/mwrender/vsgnpcassembly.*`
- `apps/openmw/mwrender/vsganimation.*`
- donor `animation*` / actor animation code where relevant.

David already models several important concepts that V4 should preserve at the neutral boundary:

- ordered animation sources;
- per-group tracks and priority/blend masks;
- per-actor source selection;
- bone map / skeleton mesh identity;
- attached equipment parts by body slot;
- skinned-part ownership separate from actor-root skins;
- head morph timing for talk/blink;
- actor head/body yaw/pitch state;
- weapon/shield visibility;
- first-person actor distinction;
- equipment diff update without discarding playing animation state.

But final V3.25 is the semantic authority for animation preparation and publication. Mode150/151 source batching, ordered main-thread publish, serial fail-closed fallback, FBFP behavior and mod-facing animation semantics cannot be replaced by donor behavior merely because the VSG representation works.

V4 rule:

- gameplay/animation chooses active groups, source order, pose, morph weights and attachment state;
- `RenderWorld` stores stable skeleton/mesh/material/instance identities;
- immutable `FrameRenderState` publishes current dynamic pose/morph/attachment transforms;
- VSG/Vulkan consumes that state.

A VSG skeleton may be an implementation object, never the semantic owner.

## 6. Terrain, distant statics and groundcover — PORT_ADAPT

Primary donor areas:

- `vsgterrain*`
- `vsgterrainworld*`
- VSG groundcover implementation;
- static/distant object placement.

Donor value:

- playable terrain/quadtree implementation;
- distant statics;
- instanced groundcover;
- backend-specific mesh/material creation.

V3.25 precedence:

- active-grid and worldspace semantics;
- current object paging content/visibility decisions;
- groundcover density/distance/animation behavior;
- promoted exact-active/post-transform batching lessons;
- cell/preload ordering and compatibility behavior.

V4 translation:

- terrain/static/groundcover producers publish generation-safe `ChunkHandle` replacement/update operations;
- a chunk owns logical instance membership and bounds;
- backend derives flat GPU instance ranges/indirect batches from the chunk;
- stale old-generation chunk results cannot mutate the replacement.

This deliberately avoids recreating an OSG-style scenegraph pager in VSG.

## 7. Lighting and shadows — PORT_ADAPT, V3.25 semantics win

David demonstrates working Vulkan scene lighting, transient dynamic lights and shadows. Use its Vulkan descriptors/UBOs/pipelines as implementation donors.

Do not use donor lighting state as semantic authority where it differs from final V3.25. Current V3.25 supports the classic point-light behavior plus clustered-lighting capability/settings. The neutral contract therefore carries all logical point-light properties and sun/environment state independently of the backend method used to shade them.

Required common light semantics:

- stable light identity for persistent/dynamic lights;
- position;
- diffuse/specular/ambient where applicable;
- constant/linear/quadratic attenuation;
- effective radius;
- actor fade/enable state where used;
- sun direction/colour/visibility separately from point lights.

Shadow implementation is backend-specific. FBFP/owner visibility and visual parity are not.

## 8. Water, reflection/refraction and ripples — PORT_ADAPT

Primary donor areas:

- `vsgwater.*`
- `vsgripples.*`
- reflection/refraction render-graph wiring.

Donor value:

- working water surface;
- reflection/refraction passes;
- ripples;
- conservative water-pass visibility gate.

V3.25 water appearance/behavior is the parity authority. The visibility gate is a strong CP8 optimization candidate because it removes work only when water cannot be visible. It is not required to define CP1 ownership.

The frame contract must nevertheless support multiple concrete views (main, reflection, refraction, shadow, maps/previews) from the beginning so water does not force another architecture rewrite later.

## 9. Sky, weather and precipitation — PORT_ADAPT

Primary donor areas:

- `vsgsky.*`
- precipitation occlusion support;
- weather/environment writes in `RenderingManagerVsg`.

Use donor VSG geometry, pipelines and dynamic-data update patterns, but publish semantic environment state first:

- sky enabled;
- weather state;
- sun and moon state;
- night/day state;
- fog;
- glare/time-of-day factors;
- precipitation state and occlusion inputs.

Known donor exterior/weather lighting residuals remain parity blockers, not acceptable new defaults.

## 10. Postprocessing/HBAO — PORT_ADAPT, CP6 parity target

Primary donor areas:

- VSG offscreen scene -> composite -> window path;
- `vsghbao.*`;
- transparent-depth/depth resources;
- GUI after composite.

Strong donor value: the modern backend already has the correct broad pass topology.

Required V4 precedence:

- current postprocessing API revision 5;
- current technique preprocessing and shader-mod behavior;
- scene color, depth, opaque depth, normals, distortion and light inputs where exposed;
- GUI/UI composited after scene postprocessing;
- HBAO appearance must be matched rather than accepting the donor's documented darker result.

CP1 only reserves the semantic render-target/frame inputs. CP6 performs compatibility completion.

## 11. GUI, maps, previews and debug renderers — PORT_ADAPT

David already has working MyGUI bridge, local/global maps, character previews, debug/nav/path rendering and screenshot paths. These are implementation donors.

Do not replace MyGUI with RmlUi in V4.0. That would enlarge the compatibility surface without helping the renderer migration.

Views used for maps/previews/debug should be modeled as explicit frame views with their own visibility/pass policy rather than implicit backend cameras hidden above the common boundary.

## 12. Hi-Z occlusion — REFERENCE_ONLY until corrected

David's Hi-Z implementation is useful as a Vulkan compute/visibility reference but remains default-off because documented false occlusion violates the project promotion rule.

Any future V4 Hi-Z path must satisfy:

- zero visible false occlusion in the parity corpus;
- fail-open uncertainty behavior;
- generation-safe inputs;
- no same-frame CPU stall introduced merely to consume the result.

## 13. SDL3 / GLM migration ordering

David proves SDL3 and a broad GLM migration are feasible. The order for this project is narrower:

1. CP1A: introduce neutral GLM renderer-facing types at the ownership seam while OpenGL remains intact;
2. CP1B: SDL3 migration with OpenGL still active;
3. CP2: bring in David VSG/Vulkan backend infrastructure;
4. expand GLM conversion incrementally after the renderer seam is protected.

Do not perform David's whole-repository GLM sweep before the materialized V3.25 semantic overlap is reconciled.

## 14. David donor acceptance summary

| Subsystem | Disposition | Semantic authority |
| --- | --- | --- |
| SDL3/Vulkan surface/device/VSG bootstrap | PORT_DIRECT pattern | V4 backend contract |
| SPIR-V/GPU timing/compile lifetime | PORT_DIRECT pattern | V4 backend contract |
| RenderingManagerVsg | PORT_ADAPT/reference orchestrator | V3.25 + V4 contract |
| ObjectsVsg/VsgWorldBridge | PORT_ADAPT implementation; ownership REJECT | V4 RenderWorld |
| NifVsg | PORT_ADAPT | V3.25 content/material/collision semantics |
| NPC/animation | PORT_ADAPT | V3.25 Mode150/151 + gameplay semantics |
| Terrain/statics/groundcover | PORT_ADAPT | V3.25 paging/content semantics |
| Lighting/shadows | PORT_ADAPT | V3.25 lighting/FBFP parity |
| Water/sky/weather/effects | PORT_ADAPT | V3.25 visual/behavior parity |
| Postfx/HBAO | PORT_ADAPT | current postfx API + visual parity |
| GUI/maps/previews/debug | PORT_ADAPT | current feature semantics |
| Hi-Z | REFERENCE_ONLY/default-off | zero-false-occlusion gate |
| David common OSG-leaking IRenderer | REJECT | V4 neutral renderer contract |

This crosswalk is the implementation lookup for CP2–CP6. CP1 must define the common data/ownership contract without importing donor backend types.