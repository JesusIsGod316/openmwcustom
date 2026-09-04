# V4.0 CP0A — Donor Convergence Audit

Status: ACTIVE SOURCE AUDIT  
Base branch: `v4.0-cp0-freeze` @ `40794e34c6e392bb0400bc2dd11bfe6cfae4b14a`  
Final V3.25 raw source identity: `f7557829bcb14e339410cefb32b6612e5009e46d`  
Final V3.25 executable SHA-256: `34d7a715e25d92dcad6b20f807e8b44a272fd3382e2d2f0a22e03bedac3e25c2`

## 1. Plan audit

The locked V4 convergence plan remains directionally correct:

`final V3.25 semantics + David P VSG/Vulkan implementation + Clockwork GPU-scene architecture + current upstream semantic deltas`

CP0A is refined into five explicit deliverables before CP1 implementation begins:

1. **Materialized-authority inventory** — establish the exact final V3.25 built source, including generated source layers.
2. **Semantic-preservation matrix** — identify renderer-facing V3.25 behavior that must survive the backend migration.
3. **David donor map** — classify implementation pieces as `PORT_DIRECT`, `PORT_ADAPT`, `REFERENCE_ONLY`, or `REJECT`.
4. **Clockwork algorithm map** — classify GPU-driven structures and algorithms separately from its OpenGL/OSG integration.
5. **CP1 contract extraction** — define the neutral handle/update/frame-state seam from the semantic union, not from either donor's public interface.

This refinement does not change the CP1–CP12 order. It prevents CP1 from starting on an incomplete representation of final V3.25.

## 2. Critical authority correction: final V3.25 is generated source

The final Mode151 Windows build does **not** compile only the raw GitHub tree at `f7557829...`.

The V3.25 actor-batch workflow first runs `tools/v3/apply_diagnostic_harness.py`, which materializes the V3 optimization stack before compilation. The accepted preflight artifact contains the exact resulting patch:

- artifact: `9921224442`
- `V3-applied-source.patch` SHA-256: `ac15332aeff34a5ce6a442e169704aab6f5eb0f1600b914d863b4561452d4c35`
- canonical patch SHA-256 with presentation-only Git `index` lines removed: `a72c6456f2fd575f9c47db296e7bdf8befee016d10ec44c5829028cd15ba1502`
- `V3-applied-source-stat.txt` SHA-256: `c01f35f1a047b624b14daec279580805722fe4dbacdd5bb2b178e071ca28f045`
- changed files: **103**
- patch size: **+11,021 / -515 lines**

Therefore the semantic authority for V4 is:

`raw f7557829 source + exact applied-source patch ac15332a...`

Raw `f7557829` alone is insufficient. For example, raw `animation.cpp` still shows serial per-source controller cloning and immediate `AssignControllerSourcesVisitor`, while the generated build contains Mode150 actor-source batching plus Mode151 actor-batched parallel controller-clone preparation and deterministic main-thread publication.

### Materialization reproducibility finding

The first CP0A materialization run regenerated the same 103-file source delta and an identical source-stat file, but newer Git emitted ten-character object abbreviations on patch `index` lines instead of the seven-character abbreviations present in the accepted V3.25 artifact. A direct diff confirmed **zero changed patch lines outside those `index` lines**; after removing only those presentation-only lines, both patches hash to the same canonical SHA-256 `a72c6456...`.

The materializer now pins `core.abbrev=7` for byte-for-byte archival reproduction and independently verifies the canonical patch hash, source-stat hash and 103-file inventory. This is a provenance-format difference, not a source-semantic divergence.

**CP1 gate:** the V4 development lineage must materialize or otherwise exactly preserve this generated source before renderer ownership refactoring starts. V4 must not silently depend on the old V3 runtime patch harness as an undocumented semantic layer.

## 3. V3.25 renderer-facing preservation surface

These are first-class semantic contracts for the donor reconciliation:

| Area | V3.25 authority / requirement | CP0A disposition |
| --- | --- | --- |
| Animation source setup | Mode150 final actor-wide assignment + Mode151 batched parallel clone prep, fail-closed serial fallback, deterministic publish | `PRESERVE_EXACT` |
| FBFP / owner rendering | First-person camera semantics with full-body third-person animation/body behavior and camera-specific owner hiding/shadow compatibility | `PRESERVE_EXACT` |
| Camera/Lua | Current camera behavior and projection-offset Lua surface | `PRESERVE_API` |
| NIF collision | Root `RCN` extra data enables recursive `findRootCollisionNode(recursiveCollision)`; selected collision subtree hidden, not every RCN blindly hidden | `PRESERVE_EXACT` |
| Lighting | Current classic path plus clustered compute-lighting behavior | `PORT_SEMANTICS` |
| PBR/materials | Current V3.25 PBR and alpha/material semantics | `PORT_SEMANTICS` |
| Postprocessing | API revision 5; scene/LDR/depth/opaque-depth/normal/distortion surfaces; existing shader-mod chain behavior | `PORT_SEMANTICS` |
| NIS/render scaling | Current V3 generated NIS path is retained as comparison/reference functionality | `PORT_ADAPT_LATER` |
| Object paging / groundcover | Current exact-active/object-paging and groundcover semantics, including promoted/rejected optimization lessons | `PORT_SEMANTICS` |
| Shadows | Current owner/view shadow behavior and V3 generated shadow fixes | `PORT_SEMANTICS` |
| Water | Current water/reflection/refraction behavior | `PORT_SEMANTICS` |
| Frame-critical jobs | Immutable/pre-resolved worker inputs, worker-owned unpublished output, deterministic main publish, no frame-critical FIFO-then-wait | `ARCHITECTURE_RULE` |

The generated patch also changes world streaming, SceneManager/resource caches, Lua, sound, mechanics, physics, settings, shader management, and diagnostics. Those changes must be classified before broad GLM or renderer-boundary edits touch the same files.

## 4. David P donor map

Audited donor archive identity:

- archive SHA-256: `39c34e63913354bea45d813b9407932e4117468403e1b0cb17e971b46672d490`
- branch/source label: `sdl3_vsg_gl_math`
- OpenMW: 0.52
- SDL3: 3.4.10 requirement
- optional VSG/Vulkan backend present

### PORT_DIRECT candidate patterns

These are implementation patterns that can be transplanted with narrow adaptation, not assumed byte-for-byte compatible:

- SDL3 Vulkan surface/window bootstrap
- VSG device/viewer/command-graph/render-graph bring-up
- SPIR-V compiler plumbing
- GPU timestamp/per-pass timing infrastructure
- VSG compile/deletion/resource-lifetime helpers
- Windows-capable Vulkan surface code

### PORT_ADAPT

These systems are mature donor implementations but must be reconciled against final V3.25 semantics and the future ownership boundary:

- NIF → VSG conversion
- material/texture/alpha handling
- animation/skinning and NPC assembly
- statics/object bridge
- terrain/quadtree and distant statics
- groundcover
- shadows/lighting
- sky/weather
- water/reflection/refraction/ripples
- precipitation
- particles/effects/projectiles
- MyGUI bridge
- local/global maps and previews
- HBAO/postprocessing

### REJECT as final architecture

- David's `components/render/IRenderer` contract because it directly exposes OSG Viewer/Group/Camera/Node methods even for the Vulkan backend.
- live mutable `VsgWorldBridge` / direct VSG-object mutation as the long-term simulation-to-render ownership model.
- wholesale rebase onto David's repository.
- wholesale whole-engine GLM sweep before materializing/reconciling V3.25 custom changes.
- current Hi-Z path as a default because documented false occlusion remains unacceptable.

### Confirmed semantic conflict: RootCollisionNode

Final V3.25 OSG loading recognizes root `NiStringExtraData == "RCN"`, passes a recursive flag to `findRootCollisionNode`, and hides the selected collision subtree.

David's current NIFVSG loader skips meshes whenever the node itself is `RC_RootCollisionNode` (plus hidden/bounding/marker cases). That is not equivalent to the final V3.25 recursive RCN contract. NIFVSG is therefore `PORT_ADAPT`, with V3.25 behavior authoritative.

### Confirmed semantic delta: clustered lighting

Final V3.25 already contains compute-clustered point lighting (`LightManagerCullCallback`, cluster-grid compute, light-cull compute, SSBO cluster/light-grid/index buffers). David's renderer cannot be accepted as lighting authority where it predates or differs from this behavior. Vulkan lighting must reproduce current V3.25 semantics first, then optimize.

## 5. Clockwork donor map

Audited donor archive identity:

- archive SHA-256: `acc75476f5020c6facb5e8b573b2453b037892396a50fa44d5cc9716e82ed437`
- source label: `gpu-lowend-tuning`

Clockwork is an **algorithm/data-architecture donor**, not a graphics-API implementation donor.

### ALGORITHM_ONLY / PORT_ADAPT into RenderWorld

- persistent mesh records
- persistent material records
- persistent instance/chunk records
- generation-safe chunk/instance identity
- flat GPU-facing resource tables
- multi-view visibility in one compute stage with per-view masks
- indirect-command generation
- bindless → texture-array → legacy material fallback tiers
- explicit mesh/texture residency budgets and retirement
- optional GPU skinning data path
- shared lighting/range data where compatible
- unsupported-content fallback rather than all-or-nothing conversion

### REJECT direct port

- OSG `CullVisitor` capture callbacks as the source of the persistent scene
- OSG `StateSet` as the material authority
- OpenGL SSBO/memory-barrier/multi-draw submission code
- any OpenGL-specific context/fence/texture-array ownership code copied as-is

The V4 equivalent must be fed by `RenderWorld` update packets. VSG can be a transitional backend representation, but V4 must not rebuild the OSG scenegraph-capture model on top of VSG.

### Occlusion rule

Clockwork's occlusion architecture remains useful, but V4 promotion keeps the project rule: **zero visible false occlusion**. David's known-bad Hi-Z and any Clockwork-derived occlusion path stay switchable/default-off until that gate is met.

## 6. CP1 contract extracted from CP0A

CP1 should begin with a targeted renderer-boundary neutral layer, not a whole-engine math migration.

Required stable handle families:

- `MeshHandle`
- `MaterialHandle`
- `TextureHandle`
- `SkeletonHandle`
- `InstanceHandle`

Required identity model:

- index/slot + generation or equivalent stale-handle protection
- deterministic publish ordering
- explicit resource creation/update/retire operations

Required persistent state:

- `RenderWorld`: flat mesh/material/texture/skeleton/instance tables designed to map naturally to future GPU buffers
- `FrameRenderState`: immutable/versioned per-frame camera, previous-camera, environment, water/weather, dynamic transforms, skeletal state, visibility/pass masks, temporal identity/history validity

The high-level renderer boundary must not expose OSG or VSG types. A legacy OSG adapter may translate neutral commands/state into the existing V3.25 renderer while Vulkan comes online.

### GLM refinement

David's GLM migration is valuable donor work, but CP1 should use a **targeted GLM-neutral renderer envelope first**. Broad whole-project GLM conversion overlaps hundreds of files and can collide with V3.25's generated custom layers. Convert renderer-facing values at well-defined seams, then expand GLM migration incrementally after semantic parity is protected.

## 7. CP0A exit gates

CP0A is complete only when:

1. the 103-file generated V3.25 delta is inventoried and renderer-overlap files are classified;
2. the exact final built-source materialization strategy is locked;
3. all David VSG subsystems have a donor disposition;
4. all Clockwork GPU-scene subsystems have an algorithm disposition;
5. known V3.25-vs-donor semantic conflicts have explicit winners;
6. CP1 types and update ownership rules are specific enough to implement without inventing backend semantics during coding;
7. no CP1 change requires preserving hidden behavior only inside the V3 patch harness.

## 8. Immediate next audit work

- complete the verified materialized-source commit on the CP0A branch;
- classify generated V3.25 renderer/world/resource overlaps against David subsystem files;
- audit David NIF/material/actor/terrain/water/postfx subsystem boundaries against those overlaps;
- audit Clockwork record ownership, retirement, texture-budget and multi-view data contracts for the exact fields CP1 must reserve;
- then lock the CP1 implementation handoff.
