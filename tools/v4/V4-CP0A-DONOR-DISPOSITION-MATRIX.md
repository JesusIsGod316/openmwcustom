# V4.0 CP0A — Donor Disposition Matrix

This matrix is an implementation handoff, not a license to copy donor behavior over final V3.25. If a donor conflicts with the materialized final V3.25 semantic source, V3.25 wins unless a later explicit project decision changes the contract.

Disposition vocabulary:

- `PORT_DIRECT_PATTERN`: narrow infrastructure pattern suitable for early transplantation after build/ownership adaptation.
- `PORT_ADAPT`: substantial donor implementation is useful, but semantics/ownership/API must be reconciled.
- `ALGORITHM_ONLY`: retain data/algorithm design; rewrite backend integration for V4.
- `REFERENCE_ONLY`: use to cross-check behavior or design, not as primary code.
- `REJECT`: do not make this the V4 architecture/default.

## A. David P — primary VSG/Vulkan implementation donor

| Subsystem | Donor location / evidence | Disposition | V4 rule |
| --- | --- | --- | --- |
| SDL3 window/input base | root CMake + SDL3 migration | PORT_ADAPT | CP1B; prove OpenGL parity before Vulkan dependency is blamed for SDL changes. |
| Vulkan SDL surface | `components/render/backend/vsg/vsgsdlwindow.*` | PORT_DIRECT_PATTERN | Adapt to our Windows build matrix and renderer selection. |
| VSG device/viewer/frame loop | `vsgrenderer.*` | PORT_ADAPT | Keep backend-local; do not expose VSG through game/mod-facing APIs. |
| SPIR-V compilation | `spvcompiler.*` | PORT_DIRECT_PATTERN | Reuse compiler plumbing where compatible with postfx/mod shader contract. |
| VSG image/texture loading | `components/vsgrender/vsgimagemanager.*`, `vsgtextureloader.*`, `vsgvfsreaderwriter.*` | PORT_ADAPT | Reconcile current V3.25 image/cache/resource semantics and 8GB budget. |
| Shader templates/pipelines | `vsgshadermanager.*`, `vsgshadertemplate.*`, `vsgmeshpipeline.*`, `vsgmaterialdefines.*` | PORT_ADAPT | Must reproduce current PBR/alpha/lighting/postfx inputs before optimization. |
| NIF loader | `components/nifvsg/nifvsgloader.*` | PORT_ADAPT | V3.25 NIF semantics win; confirmed RCN conflict requires correction. |
| NIF skin/morph/controllers | `nifvsgskin.*`, `nifvsgmorph.*`, `nifvsganimation.*`, `nifvsgcontrollers.hpp` | PORT_ADAPT | Preserve animation text keys, controller behavior, FBFP/actor assembly and final V3.25 source ordering. |
| NIF particles | `nifvsgparticle.*` | PORT_ADAPT | Visual/event parity before perf claims. |
| VSG actor animation | `apps/openmw/mwrender/vsganimation.*`, actor/NPC donor code | PORT_ADAPT | Must compose with materialized Mode150/151 source ownership rather than overwrite it. |
| NPC assembly/equipment | `vsgnpcassembly.*`, `objectsvsg.*` | PORT_ADAPT | Preserve body-part priority, equipment, first-/third-person and owner-hide semantics. |
| Terrain | `vsgterrain*`, terrain donor code | PORT_ADAPT | Preserve world/LOD/material behavior; later map persistent statics into RenderWorld. |
| Groundcover | VSG groundcover donor | PORT_ADAPT | Preserve density/distance/animation; Clockwork-style GPU scene may replace submission architecture later. |
| Water | `components/vsgrender/vsgwater.*`, `apps/openmw/mwrender/vsgwater.*` | PORT_ADAPT | Preserve reflection/refraction/underwater behavior; water visibility RTT gate is a candidate mechanism. |
| Sky/weather/ripples | VSG sky/ripples/weather donor files | PORT_ADAPT | Visual parity gate. |
| Shadows | VSG shadow pipeline/HUD/math + mwrender shadow work | PORT_ADAPT | Preserve current V3.25 owner/head/secondary-view semantics and current shadow fixes. |
| HBAO | `vsghbao.*` | PORT_ADAPT | Known donor visual-strength mismatch; V3.25 reference images win. |
| Postprocessing framework | VSG FX/fullscreen/composite path | PORT_ADAPT | Must reproduce API revision 5 and current technique/shader-mod surface. |
| GUI/MyGUI | `vsgguibridge.*`, UI pipeline | PORT_ADAPT | Keep MyGUI in V4.0; no RmlUI side quest. |
| Maps/previews | `localmapvsg.*`, `characterpreviewvsg.*`, related donor code | PORT_ADAPT | Explicit CP0 visual corpus cases. |
| GPU/per-pass timing | VSG backend/progress instrumentation | PORT_DIRECT_PATTERN | Required by CP2; add VRAM/residency telemetry for 8GB project limit. |
| Async queue/threadpool concepts | donor backend/runtime helpers | PORT_ADAPT | Must obey V3.24/V3.25 QoS rule: no frame-critical FIFO-then-wait. |
| `components/render/IRenderer` | exposes OSG viewer/root/camera/node in nominal common API | REJECT | Build a new high-level semantic interface; backend-specific handles stay below it. |
| live `VsgWorldBridge` ownership | mutable VSG scene objects updated directly | REJECT_AS_FINAL | Transitional adapter only; final target is update packets → persistent RenderWorld → immutable/versioned frame state. |
| whole-repo GLM sweep | broad donor migration | REJECT_AS_CP1_STRATEGY | Start with targeted renderer-boundary GLM envelope to avoid colliding with 103 generated V3.25 changes. |
| current Hi-Z default | donor progress documents false visible occlusion | REJECT_DEFAULT | May be revisited switchably only after zero-false-occlusion gate. |

## B. Project Clockwork — primary GPU-scene / structural optimization donor

| Mechanism | Disposition | V4 translation |
| --- | --- | --- |
| persistent mesh table / arenas | ALGORITHM_ONLY | `RenderWorld` mesh resources map to persistent Vulkan buffers/allocations. |
| persistent material table | ALGORITHM_ONLY | Neutral material records map to Vulkan descriptors/bindless indices; preserve V3.25 PBR semantics. |
| persistent instance/chunk table | ALGORITHM_ONLY | Stable slot + generation identity becomes CP1 core design. |
| generation checks | ALGORITHM_ONLY | Mandatory stale-update/stale-draw protection. |
| all-view compute visibility | ALGORITHM_ONLY | Main/shadow/reflection/refraction visibility processed from shared persistent data with distinct view masks. |
| per-view frustum/LOD/small-feature filters | ALGORITHM_ONLY | Port semantics selectively; verify parity and current project settings. |
| indirect command generation | ALGORITHM_ONLY | Vulkan indirect draw buffer generation in CP7/8. |
| multi-draw submission | ALGORITHM_ONLY | Use Vulkan indirect/batched submission rather than GL MDI calls. |
| bindless materials | ALGORITHM_ONLY | Vulkan descriptor-indexing path only when hardware/driver support and 8GB budget evidence justify it. |
| texture-array fallback | ALGORITHM_ONLY | Retain fallback tier for unsupported/pressure cases. |
| legacy material fallback | ALGORITHM_ONLY | Unsupported content remains correct rather than disappearing. |
| mesh residency budget/eviction | ALGORITHM_ONLY | Required from first persistent-GPU-scene implementation; project 8GB cap overrides donor heuristic. |
| texture residency/proxy tiers | ALGORITHM_ONLY | Useful low-memory mechanism; quality degradation must be explicit/measurable, not silent. |
| retire/fence lifetime tracking | ALGORITHM_ONLY | Translate to Vulkan frame/fence/timeline lifetime model. |
| GPU skinning | ALGORITHM_ONLY | CP8 candidate after CPU/GPU measurement; temporal path must retain previous pose where motion vectors need it. |
| GPU ray queries | REFERENCE_ONLY_FOR_V4.0 | Not required to reach renderer parity; avoid scope expansion unless it replaces a measured cost. |
| lighting range/list data | ALGORITHM_ONLY | Reconcile with current clustered-lighting semantics; do not duplicate incompatible light systems. |
| Hi-Z occlusion | ALGORITHM_ONLY_EXPERIMENTAL | Default off until zero false occlusion; evaluate only after basic GPU scene is correct. |
| OSG `GpuCaptureCallback` / CullVisitor capture | REJECT | Feed GPU scene from RenderWorld, not from a recaptured scenegraph. |
| OSG `StateSet` material capture | REJECT | Material authority is neutral V4 material data, not OSG state. |
| GL SSBO/barrier/MDI code | REJECT_DIRECT_PORT | Rewrite using Vulkan/VSG backend mechanisms. |

## C. Current OpenMW upstream — forward-port semantic reference

`REFERENCE/PORT_ADAPT` only where final V3.25 or David diverges from newer upstream behavior.

Confirmed CP0A items:

1. **Project Magnus / volume-tiled clustered forward lighting** is already represented in final V3.25 source via cluster-grid/cull compute and SSBO light data. V4 must preserve that semantic capability; David is not lighting authority where it differs.
2. **RCN extra-data behavior**: with RCN flag, recursively search the subgraph; without it, use the immediate-root rule; only the selected collision node is collision geometry/hidden. Final V3.25 already contains the relevant recursive-selection behavior and therefore wins over older donor handling.

Upstream is not automatically preferred over final V3.25 if it changes gameplay/mod-visible behavior after our freeze; each delta gets an explicit disposition.

## D. Secondary/reference donors

| Donor | Role | Current CP0A disposition |
| --- | --- | --- |
| VSGOpenMW | independent VSG/Vulkan implementation | REFERENCE_ONLY / selective PORT_ADAPT when it resolves ambiguity in David or supplies a missing VSG pattern. It remains OpenMW 0.49-era and is not a base. |
| Swood TAA/TAAU patch | temporal/render-scaling prototype | REFERENCE_ONLY until CP9; useful for jitter/history/render-scale plumbing, not a substitute for engine-native per-object/skinned motion vectors. |
| Free FPS lineage | OSG occlusion/cache reference | REFERENCE_ONLY; compare fixes only. Our V3.22–V3.24 research supersedes its architecture for project decisions. |
| OpenMW-VR | multi-view/shared-shadow precedent | REFERENCE_ONLY for CP8 secondary-view/shadow reuse. |
| VORT / injector experiments | temporal diagnostic reference | REFERENCE_ONLY for validating motion vectors/present behavior; no screen-derived-vector dependency in final DLSS path. |
| NVIDIA Streamline | official DLSS/Reflex integration source | FUTURE_PRIMARY_SDK_REFERENCE at CP10–12 after native temporal contract is validated. |
| Jolt MR/source | physics donor | DEFER_TO_V4.1; do not merge with renderer rewrite. Adapt later onto GLM-neutral V4 core. |
| Godsring source | pending | AUDIT_IF_OBTAINED; never a blocker. |

## E. Cross-donor conflict rules

1. **Final materialized V3.25 semantics beat donor convenience.**
2. **David supplies the fastest working Vulkan implementation, not the public engine architecture.**
3. **Clockwork supplies the persistent GPU-scene target, not the OpenGL integration layer.**
4. **Current upstream supplies newer semantic/fix evidence, not an automatic rebase target.**
5. **VSGOpenMW is a tie-breaker/reference, not a foundation.**
6. **No performance promotion before comparable visual/workload parity.**
7. **8GB VRAM is a hard architecture input:** explicit budgets, bounded residency and pressure telemetry are required before aggressive bindless/persistent-scene defaults.

## F. CP1 design consequences already locked by CP0A

- Do not copy David `IRenderer`.
- Do not extend current `RenderingInterface` into a large OSG-shaped abstraction.
- Do not construct RenderWorld as a VSG scenegraph mirror.
- Use stable typed resource handles with generations.
- Make resource creation/update/retire and instance create/update/remove explicit.
- Keep previous-frame identity/state available from the beginning so CP9 motion vectors do not require another ownership rewrite.
- Define per-view masks/visibility state early enough that main, shadow, reflection/refraction and future temporal passes can share persistent scene data without pretending their visibility sets are identical.
- Carry explicit memory/residency accounting from CP2 onward; Clockwork-style budgets should not be bolted on only in CP7.
