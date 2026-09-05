# V4.0 CP2 — Pre-CP3 Source Hardening Audit

Status: **SOURCE HARDENING PREPARED — WINDOWS QC REQUIRED — FINAL CORRECTED RESIZE RUNTIME RETEST STILL PENDING**
Audited head: `9af7acc701e9e1afa4799681b4e78e9c3a5c6c34`
Audited run: `33977577668` — Preflight, FoundationWindows and full OpenGL control all green
Performance claim: **NONE**

## 1. Runtime state carried into this audit

The user-hardware CP2 probe has already proven the core Vulkan path on the RTX 5050 Laptop GPU:

- discrete NVIDIA device selected;
- Vulkan API 1.4.351 reported;
- device-local heap/budget telemetry returned;
- VSG render graph + shader/pipeline + swapchain compile passed;
- visible geometry presented;
- swapchain survived repeated resize without a crash;
- clean shutdown message was observed.

The first probe exposed a real aspect-ratio ownership bug during extreme resize. `9af7acc...` fixes that by coalescing SDL resize events and allowing VSG's RenderGraph/WindowResizeHandler to own camera projection/viewport extent changes. CI is green on that repair, but the corrected wide/tall user-hardware retest remains a CP2 closeout gate.

## 2. Existing CP2 code that is sound and should not be churned

### SDL/VSG window ownership

`SdlVulkanWindow` keeps SDL3 as native-window authority and VSG as Vulkan-resource authority. The adapter's `releaseWindow()` behavior intentionally mirrors VSG's native window backends: it only severs the native handle; ordered teardown remains the owner's responsibility. The probe destroys the SDL window only after VSG viewer/window references are released. No speculative `releaseWindow()` rewrite is justified by this audit.

### Dependency pin

VulkanSceneGraph v1.1.16 is now an official upstream release and contains useful available-memory, paging thread-safety and out-of-GPU-memory fixes. However the current upstream vcpkg `vsg` port still resolves to 1.1.15#1. CP2 therefore keeps the already validated 1.1.15#1 pin rather than introducing an ad-hoc overlay port immediately before CP3. A 1.1.16 dependency-only experiment remains worthwhile once it can be pinned reproducibly or deliberately carried as an isolated overlay delta.

## 3. Defects found that matter before real CP3 resource churn

### ResidencyLedger mutation safety

The original CP2 ledger had two robustness gaps that the one-box probe was unlikely to exercise:

1. `queueRetire()` incremented `pendingRetireBytes` before `std::vector::push_back()`. If ticket allocation threw, accounting could claim a retirement that did not exist.
2. `collectRetired()` subtracted several counters sequentially. If an invariant mismatch was encountered after the first subtraction, the ticket could remain while accounting was partially mutated.

Hardening:

- publish the retirement ticket before changing counters;
- validate every required subtraction before mutating any counter;
- overflow-protect queued-class summation;
- saturate snapshot/released-byte aggregate telemetry rather than wrapping.

This is correctness hardening, not an eviction policy or performance feature.

### RenderWorld referential integrity

The initial logical world could retire a mesh/material/skeleton/chunk that was still referenced by a live instance, and it had no same-generation update API for resource revision changes. That becomes dangerous as soon as CP3 streams/reloads NIF resources.

Hardening:

- `SlotTable::update()` preserves slot/generation identity while replacing a live payload;
- deterministic read-only `forEachLive()` supports invariant checks without exposing mutable storage;
- versioned RenderWorld records now require strictly increasing `ResourceRevision` on update;
- instance updates revalidate mesh/material/skeleton/chunk handles;
- resource/chunk/instance retirement fails closed while live logical dependents still reference the target;
- chunk membership validation rejects stale, cross-chunk and duplicate member handles;
- `RenderWorld::valid()` provides an explicit expensive publication/checkpoint audit for resource references and bidirectional chunk membership.

The implementation deliberately does not add backend objects, pointer identity or GPU allocation state to RenderWorld.

### FrameRenderState numeric fail-closed validation

The original `lodScale <= 0` test accepts NaN because NaN comparisons are false. Real camera/transform publication in CP3 should not be allowed to put NaN/Inf into command recording or skinning data.

Hardening:

- require finite simulation time/frame delta/jitter/projection offset;
- require finite positive LOD scale;
- require finite current/previous camera position/orientation/view/projection;
- require finite current/previous dynamic transforms.

This does not add CP3 pose/morph data yet; it only hardens the existing frame contract.

## 4. Missing architecture that is intentionally CP3 work, not guessed into CP2

The locked common boundary remains:

`game/mod semantics -> ordered immutable RenderWorldUpdateBatch -> persistent RenderWorld -> immutable FrameRenderState -> semantic renderer service -> backend`

Current source still does not contain the full `RenderWorldUpdateBatch` producer/apply path or the semantic renderer service. CP2 explicitly deferred those because the Vulkan probe is isolated. They become the **first CP3 integration task** and must exist before real OpenMW game objects are allowed to reach VSG.

Do not bypass this gap by mapping `MWWorld::Ptr` directly to long-lived `vsg::Node` ownership.

Likewise, the current RenderWorld resource records are intentionally minimal. CP3 must derive the exact neutral payload from current OpenMW/V3.25 semantics plus the audited donor implementation rather than freezing a guessed GPU vertex/material ABI in CP2.

Required CP3 extensions include at least:

- mesh bounds/topology/surface descriptors and backend-neutral geometry payload ownership;
- material scalar/color/alpha/two-sided/texture-role semantics with current V3.25 PBR precedence;
- texture role/color-space/sampler semantics;
- skeleton hierarchy/bind/inverse-bind/stable bone indexing;
- instance bounds/LOD/attachment semantics;
- immutable per-frame skeleton pose and morph publication, including previous-rendered history where required.

## 5. Engine integration audit

`OMW::Engine::createWindow()` is still correctly hard-wired to `SDL_WINDOW_OPENGL`, `GraphicsWindowSDL2`, the OSG camera and OSG viewer. That is the protected OpenGL control path.

CP3 must not mutate that path into a conditional tangle. Use startup-scoped backend selection with a separate Vulkan/VSG construction path that shares SDL/input/game semantics above the renderer boundary. OpenGL remains available as the parity control until the locked V4 parity gates close.

The first Vulkan game path must consume neutral handles/update batches/frame state. It must not require OSG scene nodes to exist merely to feed VSG.

## 6. Recommended CP3 implementation order

1. Close CP2 with the corrected wide/tall resize + clean-shutdown retest on the final hardening head.
2. Branch CP3 from that exact accepted head.
3. Implement the immutable ordered `RenderWorldUpdateBatch` and minimal semantic renderer service, compile-isolated first.
4. Audit/port the first static interior NIF/material path from the David implementation while preserving final V3.25 NIF/RCN/PBR/alpha semantics.
5. Publish those resources into RenderWorld and consume them through VSG; no direct game-pointer/VSG-node persistent identity.
6. Add skeleton/animation/controller/skin/morph neutral payloads and immutable pose publication, then bring one actor/NPC path online with serial fail-closed fallback.
7. Add the minimum MyGUI bridge required for a genuinely usable interior slice.
8. Keep the OSG/OpenGL executable/path buildable and testable on every CP3 source head.

CP3 should be split into small switchable sub-checkpoints inside this vertical slice rather than attempting full renderer parity in one commit.

## 7. CP3 readiness disposition

After the hardening changes in this audit compile and pass the same CP2 FoundationWindows + full OpenGL control QC, there are no known CP2 source defects that justify delaying CP3 beyond the outstanding corrected hardware resize retest.

The absent update-batch/renderer-service and richer NIF/material/skeleton/pose records are not waived. They are the first implementation layer of CP3 itself.
