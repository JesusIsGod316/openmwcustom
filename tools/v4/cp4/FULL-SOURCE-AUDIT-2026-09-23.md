# Vulkan build source audit — 2026-09-23

## Verdict

The tested Vulkan build has substantial **CPU-side architectural bottlenecks**, not just expensive graphics settings. Its bridge converts a broad set of live OSG objects into immediate, copied geometry every frame. The backend then performs expensive reuse checks, repeated static planning, and synchronous realization/compilation on the presenting thread. GPU residency reuse exists and works; the missing piece is sufficiently persistent, incremental CPU-side representation.

The current occlusion mechanism cannot recover time already spent on capture, copying, planning, or compilation. Nor can adding bloom, clouds, or F2 compatibility fix those costs. Complete OpenMW shader/mod/settings parity is also not implemented. Those compatibility features remain required, but should not obscure the performance repair.

This is an architecture-wide audit of the Vulkan integration and its relevant engine boundaries. It is **not a line-by-line certification of all 4,398 tracked repository files**, a security audit, or proof that arbitrary OpenMW mods/saves work. Broad source inventory/search was followed by deep tracing of the relevant frame, resource, capture, visibility, UI, and compatibility paths.

No implementation was changed for this audit. The only new project artifact is this report.

## Exact audited state

- Actual source root: `C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build`.
- Branch: `codex/cp4f-material-frame-repair`.
- Base HEAD: `efa4f3694fad904b3546e7c846c756403950f0d2`.
- Pre-existing work: 48 modified tracked files and 24 untracked files. All retained.
- All **72/72** hashes in the package's `changed_source_files` manifest matched the source tree during this audit.
- Package: [native-build-manifest.json](<C:/OpenMW-Native-Test/native-build-manifest.json>).
- Executable SHA-256: `77e9775c7a61351933418d45df66cbb2dea09e3091661a13200159c81f3a8260`.
- The initially opened project at `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build` is a different, older checkout: `codex/cp4e-water-recovery`, HEAD `073a00dd00ca8026ca3a4734120aad09d42ef677`. It was not mistaken for the tested source.
- The Visual Studio build is multi-configuration; the cache's `CMAKE_BUILD_TYPE=RelWithDebInfo` alone does not identify the selected configuration. Release flags are optimized (`/O2 /Ob2 /DNDEBUG`); RelWithDebInfo is also optimized. No evidence here establishes an accidental Debug build as the explanation.
- No new compiler/build invocation, commit, push, package replacement, mod installation, or game launch occurred. Existing fixture binaries were rerun.

The Shared Project Context Archive's control block, relevant V4 history, and the repository compatibility contract informed the audit. Historical constraints retained: V3.25 compatibility foundation; no hybrid OpenGL/Vulkan presentation; no unsafe live OSG/Lua access on workers; no broad resurrection of rejected building-occluder/aggressive parallel-MSOC approaches; no whole-cascade shadow-reuse shortcut; preserve useful residency with pressure escape.

## Evidence from the rejected test

Capture: [20260923-191206-gameplay-129592.zip](<C:/OpenMW-Native-Test/Test-Results/20260923-191206-gameplay-129592.zip>). Raw logs and reports are in the [matching capture directory](<C:/OpenMW-Native-Test/Test-Results/20260923-191206-gameplay-129592>).

This run exited normally. Its controls enabled static frustum rejection, terrain occlusion, parallel effect publication, validated effect reuse, and bulk stream updates. Native postprocessing was unset/off. Therefore this capture is not evidence that bloom or Rafael's postprocessing is causing the current collapse.

The exterior slice is after frame 3671 and before frame 3964. There are seven sampled phase records and six complete frame-end records in this slice. Median complete sampled frame time is **183.299 ms**, range **176.542–200.812 ms**. About 5.5 FPS is the inverse of that median budget, not a full-run average or an OpenGL comparison benchmark.

| Phase | Median ms | Interpretation |
|---|---:|---|
| Vulkan capture | 64.388 | Includes dynamic capture; not additive to it |
| Dynamic capture | 62.812 | Main-thread object/actor capture |
| Effect snapshot publication | 6.242 | Neutral owned copy/publication, three workers active |
| Vulkan present | 109.516 | Inclusive backend scope; not GPU execution time |
| Dynamic realization | 41.849 | Includes dynamic compilation |
| Dynamic compile | 22.118 | Compile-manager path for new/rebuilt residents |
| Static sync | 19.208 | Includes static plan/compile work |
| Static plan | 10.514 | Whole-static-world planning/checking |
| Static compile | 5.291 | Re-realization uploads/compilation |
| Submit task | 29.291 | Recording/submission and possible waits; not a GPU timestamp |
| Pipeline audit | 3.784 | Per-view graph validation |
| OSG update | 4.110 | Headless update traversal |
| Native visibility | 0.341 | Main-view static visibility only |
| Mechanics | 0.978 | Not the dominant sampled scope |
| Physics | 0.540 | Not the dominant sampled scope |
| Lua wait | 0.001 | Does not establish total Lua work, only the wait |

Do not sum nested scopes or subtract medians to invent exclusive costs. These are sampled instrumented observations, not promised future speedups. The capture has dropped runtime events, and no fresh Nsight/GPU timestamp trace was taken during this audit.

Representative frame 3781:

- 2,716 objects scanned; 609 classified `useAnim`.
- Object capture: 60.331 ms; actor capture: 1.745 ms.
- 1,810 ordinary geometry visits, two rig evaluations, 12 morph evaluations.
- 2,058 effect draws; 36,467,268 geometry bytes in the immutable handoff.
- 2,053 effect residents reused, five rebuilt; all 48 actors reused.
- 647 population groups; 646 plans reused, one changed.
- Changed group: `meshes/m/misc_com_bottle_14.nif`, exterior `-2,-9`, cause `placement_fields`.
- 672 visibility candidates; 212 frustum rejections; **zero occlusion rejections**.

All seven exterior samples have zero occlusion rejections. Frustum rejection ranges from 173 to 278. Terrain occluder triangles are zero in the first two samples and 931–2,205 in the later samples. This does not prove occlusion is universally broken; it does prove there is no demonstrated occlusion benefit in this failing slice.

## Prioritized findings

### A01 — P1: Broad evaluated-object capture defeats persistent rendering

**Confirmed source behavior, strong measured connection.**

[apps/openmw/mwrender/v4scenerenderlifecycle.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4scenerenderlifecycle.cpp:358>) routes objects reporting `useAnim()` away from static residency. [apps/openmw/mwrender/v4enginerenderbridge.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4enginerenderbridge.cpp:638>) scans authoritative animations and captures their evaluated subtrees. The classification means “requires animation-capable handling,” not “all geometry bytes changed this frame.”

[apps/openmw/mwrender/v4effectcapture.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4effectcapture.hpp:758>) reconstructs transforms, vertex arrays, material/texture state, UVs, primitive indices and bounds, then validates the result. [components/rendercore/effectframe.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/rendercore/effectframe.hpp:85>) creates another owned neutral snapshot. Backend reuse comparisons still inspect substantial material/topology/stream state.

The measured 60 ms object capture and thousands of reused downstream residents are the key mismatch. It is incorrect to say the whole GPU scene is recreated every frame: residency reuse is already present. The CPU keeps rediscovering and copying data before it can benefit from that reuse.

**Repair:** persistent evaluated-draw identities with shared immutable mesh/topology/material ownership; independently version transform, UV/controller output, deformation, material and topology changes. Publish changed streams, not the entire geometry payload. Unknown/custom mutation paths require a conservative invalidation fallback. Keep required gameplay/controller evaluation; do not freeze unseen actors or skip scripts.

**Gate:** unchanged-scene capture work must scale with changed objects/streams rather than all animation-capable geometry. Test NiSwitch/visibility, texture flips, UV animation, morph/rig deformation, equipment changes, attached effects, first-person body, and the already-corrected NPC picking.

### A02 — P1: Transient effects can repeatedly rebuild shader/pipeline families

**Confirmed cache boundary and compile path; exact driver/shader split remains unmeasured.**

[components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:873>) creates a new `SharedObjects` cache for each dynamic generation. This was a deliberate repair: putting configurators containing mutable frame arrays into the global cache retained old frames. Simply restoring global configurator caching would resurrect that problem.

However, immutable pipeline/shader sharing is also bounded by this generation. [components/render/backend/vsg/staticassetrealizer.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/staticassetrealizer.cpp:432>) constructs a new compatibility ShaderSet for a new realization; [components/render/backend/vsg/legacymaterialshader.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/legacymaterialshader.cpp:682>) creates a new fragment ShaderModule and intentionally does not copy stock variants. Configurator/pipeline interning at lines 960–984 of the realizer uses the supplied cache.

[apps/openmw/mwrender/v4effectcapture.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4effectcapture.hpp:958>) emits individual live-particle draws. The immediate-effect realizer uses a temporary neutral world/static-asset realization path for newly required draws. Disappearing completed identities are collected from the resident pool, so later births can take the cold path.

Only pending new/rebuilt residents are compiled at [components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:1125>), not every reused resident. Yet the capture still spends a median 22.118 ms in that scope.

**Repair:** separate bounded immutable shader/pipeline/layout caches from per-frame mutable configurators/arrays. Give emitters persistent fence-safe stream storage, batching only where topology, transparency order, material and controller semantics permit. Handle view/render-pass changes with exact keys. Measure cold and warm behavior separately.

**Gate:** stable effects must stop generating new shader/pipeline work; changing/birthing particles must not retain dead generations indefinitely. Test transparent sorting, view creation/destruction, reflection/refraction and shadows. Do not label all measured compile time “GLSL compilation” without a finer trace.

### A03 — P1: A small static change triggers scene-wide checking and plan copies

**Confirmed source behavior and sampled repeated trigger.**

[components/render/backend/vsg/staticworldsyncstate.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/staticworldsyncstate.hpp:19>) builds/checks stamps by walking static instances, chunks and dependencies. On change, [components/render/backend/vsg/staticworldplan.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/staticworldplan.hpp:240>) visits population groups and their dependencies. Reused plans are copied with `result.populations.push_back(*prior)` at line 280.

The new indexed residency lookup fixes a lookup problem, but it does not make synchronization incremental. In the exterior slice, one bottle placement invalidates a group while 646 other plans are reused; whole-static bookkeeping still costs about 19 ms including about 5 ms of static compilation.

**Repair:** explicit static-world generation/change journal; dirty group/instance sets; immutable shared plans for unchanged groups. Split placement updates from mesh/material realization so moving a bottle or physical projectile does not require rebuilding its geometry/pipeline. Preserve transactional publication and dependency retirement.

**Gate:** zero-change frame takes a cheap early-out; a one-placement change scales with the changed group. Exercise removal, cell unload, replacement texture/material revision, world reset, multiple placements and rollback. Never use the global world revision alone if unrelated dynamic work increments it.

### A04 — P1: Visibility has narrow coverage and runs too late to avoid major CPU costs

**Confirmed design limitation; occlusion benefit absent in this sample.**

[components/render/backend/vsg/nativevisibility.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/nativevisibility.hpp>) updates static visibility registrations. [components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:2170>) invokes it after static/dynamic realization. Actor/evaluated-effect geometry does not enter the same static visibility registration set.

The regular opaque graph produced by [components/render/backend/vsg/staticassetrealizer.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/staticassetrealizer.cpp:994>) is a draw under a transform; it has no general CullNode/CullGroup wrapper. Sorted transparent draws have a separate bounded DepthSorted node. Do not generalize that exception into culling coverage for every dynamic draw.

Terrain-only occluders, fail-open near-plane/bounds handling, coarse population bounds and main-view-only rejection are conservative. They also limit what can be rejected. Zero rejection in this exterior is an effectiveness result, not proof of a false-cull correctness bug.

**Repair:** persistent per-view conservative bounds and visibility before optional render extraction/realization; include evaluated opaque actors/objects with correct current bounds. Maintain distinct visibility for main camera, reflection, refraction, local maps, previews and shadow casters. Required gameplay/controller work stays independent. Improve coarse terrain/chunk occluder selection and coverage before considering more aggressive occluders.

**Gate:** golden visible-pixel and camera-motion tests, including near-plane crossings, fast turns, moving/deforming actors, alpha assets and an occluded object that casts a visible shadow. Reject a change that merely reports more occlusion without a net frame-time gain or that reintroduces historical rejected approaches.

### A05 — P2: UI recreates immutable texture images and draw graphs each frame

**Confirmed code defect in reuse; current frame-time share is not isolated.**

[components/vsgmygui/rendermanager.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/vsgmygui/rendermanager.cpp:40>) calls `ImageInfo::create(sampler, data)` for ordinary data-backed textures. Each batch in `buildOverlayNode()` calls this again. The installed VSG 1.1.15 `ImageInfo.h` convenience constructor explicitly creates a new Image and ImageView from the same data. The UI path does not use the world's [components/render/backend/vsg/livetextureimages.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/livetextureimages.hpp:14>) sharing mechanism.

[components/vsgmygui/rendermanager.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/vsgmygui/rendermanager.cpp:300>) always builds a fresh immutable overlay; [components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:1595>) compiles that graph each preparation. [components/render/backend/vsg/uipipeline.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/uipipeline.cpp:184>) creates fresh descriptors without interning the image. External image-view-backed map/preview textures are a different case and do not recreate the underlying target through this overload.

This is more than “Vulkan API overhead”: unchanged font/UI texture data is being wrapped as new upload/image resources, potentially multiple times per frame. The immutable overlay strategy itself protected against a documented lifetime crash; removing it blindly is unsafe.

**Repair:** texture identity/revision-backed immutable image and descriptor reuse, independent of overlay generations; then a completed-frame UI vertex-buffer ring. Keep exact image ownership in every published overlay and preserve empty-overlay transitions, premultiplication, flip-Y and resize behavior.

**Gate:** a stationary HUD should perform no repeated unchanged image uploads, while movie/manual texture updates and texture replacement remain correct. Test loading-screen reentrancy and inventory/menu churn. Quantify before claiming this explains the outdoor collapse.

### A06 — P2: Destroyed UI CPU vertex buffers are retained for the entire session

**Confirmed bounded-lifetime workaround with unbounded session growth potential.**

[components/vsgmygui/rendermanager.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/vsgmygui/rendermanager.cpp:107>) appends every new CPU staging vertex buffer to an owning vector. `destroyVertexBuffer()` at line 115 verifies ownership but never releases it; `shutdown()` explicitly retains the vector. Each buffer owns its last byte-vector allocation.

This avoids stale raw MyGUI references during transitional loading-screen rendering, but destroyed widgets can accumulate CPU storage until RenderManager destruction. It is not evidence that all 23 GiB of process memory comes from UI or that GPU buffers leak.

**Repair:** explicit CPU traversal generation/reentrancy ownership, then retire buffers only after no MyGUI snapshot can reference them. GPU completion alone does not prove raw CPU references are gone.

**Gate:** repeated create/destroy of inventories, books, dialogs and loading transitions plateaus in CPU allocations and does not reintroduce use-after-free.

### A07 — P2: Vulkan graph recording remains a synchronous, single-task main-thread path

**Confirmed architecture; speedup from parallelization is not yet measured.**

[components/render/backend/vsg/vsgsubmission.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgsubmission.hpp:178>) deliberately uses a checked single-threaded submission helper and rejects multiple tasks/presentations. No runtime `setupThreading()` path is enabled. Engine preparation stays on the main thread before the Lua update window, and presentation is synchronous afterward. The dedicated effect-publication workers parallelize neutral copies, not the 60 ms live object capture or general command recording.

The user's six-core/twelve-thread CPU is not equivalent to twelve safe renderer owners. The capture's submit-task timer includes recording, transfer and possible synchronization, so its 29 ms cannot be assigned entirely to one component without profiling.

**Repair:** first eliminate redundant work; then partition immutable render preparation and command recording using dedicated worker contexts/pools, bounded jobs and explicit per-task error/completion results. Keep Vulkan queue submission and shared caches properly owned. A simple “enable VSG threading” switch is not a safe implementation.

**Gate:** serial/parallel output and lifecycle equivalence, fault propagation, minimize/restore, resize, device failure and long-running memory stability. No concurrent access to live OSG/Lua/world objects after handoff.

### A08 — P2: Every frame recursively audits pipelines across active views

**Confirmed code and measured cost.**

[components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:1686>) builds an active-view list, walks graphics bindings and repairs missing view-specific pipeline implementations. It is called every rendered frame, including when no relevant pipeline/view changed. Median sampled cost is 3.784 ms.

The audit is guarding a real earlier crash class, including hidden shadow views. It cannot simply be deleted or gated only on a raw view ID.

**Repair:** validate once per graph publication and view/render-pass generation, with explicit invalidation for pipeline changes and view-ID reuse. Keep an optional full per-frame audit control.

**Gate:** cached view-ID reuse, shadow context creation, auxiliary teardown and graph insertion tests must still catch missing/stale pipelines before record traversal.

### A09 — P1 parity gap: F2 effects, Rafael material interpretation and shadow replacements are not integrated

**Confirmed missing implementation; not the cause of enabled postprocessing overhead in this capture.**

[apps/openmw/mwrender/postprocessor.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/postprocessor.cpp:198>) disables the legacy GL postprocessor without an OpenGL context. The action/HUD path reports it unavailable. [components/render/backend/vsg/nativepostprocess.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/nativepostprocess.cpp:19>) implements only scene copy, a named edge-AA filter, or depth visualization. It is not a .omwfx executor, bloom/cloud pipeline, or Rafael-compatible effect host.

[components/render/backend/vsg/staticassetrealizer.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/staticassetrealizer.cpp:512>) rejects a modern PBR family without an explicit contract. Existing normal mapping, legacy specular, LAND blending, basic parallax and authored environment support are real features, but not a complete substitute for Rafael's packed-map/BRDF conventions. [components/render/backend/vsg/legacymaterialshader.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/legacymaterialshader.cpp:699>) excludes soft/PCSS shadow variants; the host selects HardShadows.

The prior [tools/v4/cp4/RAFAEL-REFERENCE-INTAKE.md](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/tools/v4/cp4/RAFAEL-REFERENCE-INTAKE.md>) records separate .omwfx, material-shader, parallax, shadow and asset packages. Treat that as reference intake, not proof of the winning assets in this user's VFS load order. Packed PBR maps interpreted as legacy RGB specular are a plausible cause of colored/glossy artifacts, but every screenshot's cause is not proven.

**Repair:** preserve the public effect/settings contract with a Vulkan executor; establish explicit legacy versus packed-PBR interpretation with winning VFS provenance. Translate includes, uniforms, depth/color conventions, auxiliary textures and pass dependencies. Preserve hard-shadow/legacy controls and source licensing.

**Gate:** real shader/mod corpus, not stock-only screenshots. Never infer PBR encoding solely from a filename, mass-convert user textures, or rename a basic filter as Rafael compatibility. Moving an effect “into the engine” helps only if it removes measured duplicate work or improves scheduling; it is still rendering work.

### A10 — P2 parity gap: Other public features remain incomplete or combined

Examples directly visible in the audited paths:

- Save-game screenshots use a placeholder: [apps/openmw/mwrender/screenshotmanager.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/screenshotmanager.cpp:113>). This is a thumbnail gap, not evidence of corrupted save serialization.
- Native sky occlusion-query/glare passes are deferred: [apps/openmw/mwrender/v4skycapture.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4skycapture.cpp:90>) and the explicit warning in the preview bridge.
- Soft-particle depth and distortion semantics fall back to ordinary transparent rendering in [apps/openmw/mwrender/v4effectcapture.hpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4effectcapture.hpp:317>). The new sampled-depth target does not itself implement those effects.
- Authored polygon-offset and some state/topology combinations remain rejected by effect capture.
- Dynamic physical-projectile model requirements can be rejected by [apps/openmw/mwrender/v4engineframecoordinator.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4engineframecoordinator.cpp:100>).
- Dynamic-material packets, clustered lighting, unequal render/output extents and temporal jitter are rejected at [components/render/backend/vsg/vsgruntimehost.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/vsgruntimehost.cpp:1948>).
- Actor/player shadow settings are merged into one actor-caster switch at [apps/openmw/mwrender/v4runtimeoptions.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4runtimeoptions.cpp:42>). Independent choices cannot be expressed by that field.
- MyGUI `registerShader`/`setShader` and texture `saveToFile` log unimplemented behavior.

These must be a tracked parity matrix, not obscured by “Vulkan works.” Explicit unsupported-feature failures are safer than silent corruption, but still prevent a full compatibility claim. Keep the separate OpenGL control executable/path until gates pass; do not introduce mixed-API presentation.

### A11 — P2: Host-memory headroom is poor; duplicate representations need ownership-led repair

**Pressure is observed; causal paging/leak attribution remains unproven.**

The run's memory report records peak private memory about 23.284 GiB and peak working set about 19.791 GiB, with legacy image payload about 10,589.5 MiB at its reported peak. These peaks need not be simultaneous. In the sampled exterior time window, available physical memory reaches about 1.81 GiB. System RAM displayed by an overlay is not the process's allocation count.

The Vulkan texture resolver and the legacy OSG resource route can retain separate decoded/mesh representations. Native live texture/data caches already use weak entries and shared immutable images; don't incorrectly label all native texture ownership a strong-cache leak. Effect snapshots/resident stream contracts add other CPU copies. Pressure trimming is active, not absent.

**Repair:** account by owner and unique allocation; share canonical immutable decode/mesh data where lifetimes permit, retire completed owners, and admit optional preloading using actual pressure. Keep budgets on old/new in-flight overlap and useful warm residency.

**Gate:** long traversal, cell return, menu churn and save/load memory plateaus. Correlate paging with frame stalls before blaming RAM pressure for all frame time. Do not globally disable caches or required loading.

### A12 — P2: Map readback is a synchronous global GPU synchronization point

**Confirmed source path; not established as a repeated exterior-frame bottleneck.**

[components/render/backend/vsg/auxiliaryreadback.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/components/render/backend/vsg/auxiliaryreadback.cpp:115>) batches requested map surfaces, but first calls `vkDeviceWaitIdle`, then submits and waits for staging copies. [apps/openmw/mwrender/v4localmapbridge.cpp](<C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/apps/openmw/mwrender/v4localmapbridge.cpp:362>) invokes it after map rendering. It is not an unconditional wait in every normal frame.

**Repair:** queue copies in an ordered frame graph, retain staging slots and publish CPU results after their completion fence; save operations may explicitly drain only required results. Preserve render-target lifetime and layout transitions.

**Gate:** map reveals, global map/save payload consistency, rapid cell transitions and shutdown. Attribute before prioritizing it over A01–A04.

### A13 — P2: Regression gates do not yet validate the new architecture or release state

**Confirmed current test limitations/failures, not a compiler failure.**

- Rendering CTest has ten tests; pixel execution is deliberately separate. A green CTest alone says nothing about whether Vulkan pixels were run.
- `runtime-blocker-contract.py` currently fails its exact source-string check for persistent effect updates. It expects `updateImmediateEffectRealization(effect, resident.mutableDraws)`; the implementation now calls the helper through a lambda with `streams`, an optional validated owner, and bulk mode. Behavioral fixtures pass; the string gate is stale, not proof that reuse vanished.
- The generated-output verifier fails on Windows checkout bytes for `V3.16-HITCH-LAYER.txt`. Git reports index LF/worktree CRLF. Normalizing just that observed file to LF **in memory** reproduces the expected SHA-256 exactly. No file was modified. This identifies the first failure as line-ending sensitivity; the verifier exits there, so later payloads are not certified by that run.
- Existing CP4F workflows include historical branch filters and exact-parent checkpoint checks. The audited candidate is dirty/uncommitted on a different branch; no GitHub CI result can be claimed for these 72 changes from this audit.
- Current small fixtures do not assert “unchanged modded scene performs no geometry recapture, pipeline/image churn, or global static-plan rebuild.”

**Repair:** behavioral workload-sized regression tests with operation/allocation counters and generous scaling thresholds; preserve strict identity checks but make canonical generated-byte policy explicit. Update source guards deliberately without removing their lifetime intent. Create a reproducible source/build manifest for an eventual committed candidate.

**Gate:** source checks, both backend build configurations, CPU ownership tests, Vulkan pixels/validation and a representative mod corpus, followed by user runtime acceptance. Compiler success and runtime promotion remain separate.

## Ownership and safety mechanisms worth preserving

The audit did not establish a specific new data race or universal per-frame device-idle bug. Important safeguards already present:

- Immutable backend-neutral frame ownership before the Lua update window; worker copy jobs join before publication.
- FrameResourcePool permits writes only to versions whose exact GPU use has completed; it is not based on an arbitrary “two frames later” assumption.
- Completion polling tracks submitted fences before VSG ring reuse/reset and handles hidden/minimized windows.
- View pipeline identity includes render-pass/traversal information and view lifetime, not merely an integer view ID.
- Compile contexts keep owning Views alive and check stale raw view-dependent-state pointers.
- Native main-view rejection does not suppress shadow/water/auxiliary view traversal; the GPU fixture verifies routing.
- Terrain preparation has a dedicated bounded worker, superseded-generation cancellation, and main-thread publication. BufferCache protects shared index/UV caches with mutexes. This is not proof of every upstream terrain/resource thread path.
- Legacy terrain preloading is gated on the Vulkan route; required producers remain enabled. Optional host-pressure admission is independently controlled.
- NPC picking explicitly evaluates rig/morph geometry for marked synchronous intersections when there is no OSG cull traversal. Preserve this user-confirmed improvement.

## Audit coverage and limits

| Area | Review performed | Remaining validation |
|---|---|---|
| Build identity/source chain | Correct worktree/branch/HEAD, manifest hashes, dirty changes, runtime source lists, test wiring, relevant historical workflows, generated-output gate | No fresh clean rebuild or CI run |
| Engine/main-frame bridge | Lifecycle classification, frame ordering, immutable handoff, evaluated objects/actors/projectiles, GUI preparation | Large controller/mod mutation corpus |
| RenderCore | Frame ownership, publication worker, record/revision interactions, terrain preparation, fence-aware residency | Sanitizer stress and full upstream subsystem audit |
| Native backend | Static/dynamic realization, pipeline/view compilation, uniforms/material families, submission/completion/retirement | Fine CPU call-stack/GPU timestamps for remaining submit/compile split |
| Visibility/terrain/groundcover | Main-view routing, conservative bounds, terrain occluder scope, population and terrain publication | Effective exterior occlusion and no popping/shadow omissions |
| Appearance | Legacy/material shader boundaries, normal/parallax/LAND fixtures, native sky/water/shadow paths, supplied-reference intake | Rafael parity, exact winning asset attribution, all weather/underwater/cascade transitions |
| UI/auxiliary | Texture/vertex ownership, overlay construction, native preview/local-map publication/readback, video interface | Long-session UI churn and full menu corpus |
| Memory/streaming | Cache ownership mechanisms, pressure admission, preloader gates, terrain jobs, log memory observations | Unique-allocation accounting, long-session plateau/paging correlation |
| Gameplay/mod/save boundary | Picking fix, unsupported features, public shader contract, native screenshot/save interaction | No blanket validation of Lua mods or saved games; unchanged audio/navigation/combat/serialization internals were not line-audited |

The inventory/search footprint includes 423 files across mwrender, render, rendercore, nifrender and tools/v4, with targeted adjacent engine/GUI/resource/sceneutil code. Inventory count is not a claim that every one of those files was read line by line.

## Checks executed during this audit

| Check | Result |
|---|---|
| Existing Release rendering CTest | **10/10 passed**, 1.76 seconds |
| Existing `rendering-pixels.exe all` with `VK_LAYER_KHRONOS_validation` | **Passed** on NVIDIA GeForce RTX 5050 Laptop GPU; no validation messages in returned output |
| Diagnostic configuration tests | **16 passed** |
| Gameplay diagnostics tests | **26 passed** |
| Runtime diagnostics tests | **17 passed, 1 skipped** |
| Shader-resource tests | **10 passed** |
| Application route contract | **Passed** |
| Terrain streaming contract | **Passed** |
| Population/environment contract | **Passed** |
| Water/environment contract | **Passed** |
| Video/UI contract | **Passed** |
| Host-memory integration guards | **Passed** |
| Real Nsight argument-transport check + packaged OpenMW `--version` | **Passed**; no game/config loaded, no GPU/CPU tracing |
| Runtime blocker source contract | **Failed: stale call-spelling assertion**, A13 |
| Materialized generated-output verifier | **Failed: first payload CRLF/LF byte mismatch**, A13 |
| `git diff --check` | **Passed**; Git emitted line-ending conversion warnings |

The first Nsight test invocation omitted its mandatory path arguments and stopped at argument parsing; the subsequent fully specified integration check passed. That was not a game startup failure. No compiler ran in this audit, and fixture success does not imply improved FPS or complete compatibility.

Pixel fixtures exercised native visibility routing; copy/AA/depth post target; water input readback; authored sphere/bump/enchanted material ordering; water/sky orientation; GUI ordering; LAND layers/depth/retirement; native sky; isolated previews; unshadowed lighting; independent sun-specular updates; shadow view routing; depth-based water optics; and normal-map reconstruction/UV basis cases.

## Recommended implementation order

1. **Repair persistent capture and incremental publication (A01).** Preserve controller correctness; reduce work at its origin.
2. **Separate immutable pipeline/image ownership from frame data (A02, A05)** and fix small-change static updates (A03). These can be developed as independent mechanisms, not checkpoint-number dependencies.
3. **Move conservative per-view render visibility earlier and cover evaluated opaque draws (A04).** Keep the established terrain/chunk occlusion direction; measure whether it actually rejects useful work.
4. **Remove recurring traversal/allocation overhead (A08, A06), then parallelize proven immutable work (A07).** Multithreading is allowed, but it should not merely distribute redundant copying.
5. **Complete shader/effect/settings parity (A09–A10)** using explicit public contracts and source provenance. F2/.omwfx compatibility and correct material encoding remain required, but do not need to be mixed into the first performance experiment.
6. **Finish asynchronous readback and memory ownership work (A11–A12)** with long-session tests.
7. **Repair and expand validation gates (A13)** alongside each mechanism, not after a large untestable rewrite.

Each mechanism needs an independent same-executable control and conservative fallback. Preserve the rejected capture as the baseline; the user explicitly declined rerunning the old build. No additional old-build run is requested by this audit. A later candidate can be compared against this captured evidence, with same-candidate controls available if a focused question remains.

Promotion requires substantially lower whole-frame time in the user's actual scene, bounded memory, correct NPC interaction, and no mod/visual regressions. No numeric FPS improvement is promised from source inspection alone.
