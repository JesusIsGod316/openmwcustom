# V4.0 CP2 — Pre-CP3 Source Hardening Audit

Status: **FINAL SOURCE HARDENING CORRECTION PREPARED — WINDOWS QC REQUIRED — CORRECTED RESIZE RUNTIME RETEST PENDING**
Performance claim: **NONE**

## 1. Accepted CP2 evidence entering this audit

The corrected resize-ownership source at `9af7acc701e9e1afa4799681b4e78e9c3a5c6c34` passed run `33977577668` in all three lanes: Preflight, FoundationWindows, and full OpenGLControlWindows.

User-hardware evidence from the preceding probe established the core path on the NVIDIA GeForce RTX 5050 Laptop GPU:

- Vulkan API 1.4.351;
- device-local heap/budget reporting;
- VSG render graph + shader/pipeline + swapchain compile PASS;
- visible Vulkan geometry presentation;
- swapchain survived repeated resize without a crash;
- clean shutdown message observed.

The first probe exposed double-applied aspect-ratio resize ownership. `9af7acc...` removed the probe's manual projection/viewport mutation, coalesced SDL resize notifications, and lets VSG RenderGraph/WindowResizeHandler own projection/viewport extent changes. CI is green on that repair. The user is away from the machine, so the corrected extreme-wide/extreme-tall hardware retest remains the final CP2 runtime gate.

## 2. Preliminary hardening and CI history

`1294a7d28386b36e8ebb69a6f85923f625d5f3ce` introduced the first pre-CP3 hardening pass:

- ResidencyLedger exception/invariant safety;
- same-generation SlotTable/RenderWorld updates;
- RenderWorld dependency validation and logical retirement guards;
- FrameRenderState finite-value validation;
- expanded isolated foundation tests;
- this audit document.

Run `33982424653` stopped in preflight only because this Markdown file used trailing whitespace for line breaks. No C++ compile ran. `5cff240ffe74c86ceaa0385ac4e13192e0798bbb` removed that formatting-only defect and triggered run `33982475602`.

## 3. Recovered CP1A RenderWorld consistency history

A source audit recovered the historical unpromoted branch `v4.0-cp1a-renderworld-consistency-staging`. It must not be treated as accepted lineage or blindly cherry-picked, but it contains directly relevant design/QC evidence that was missing from the structured current-state narrative.

Important evidence:

- `V4-CP1A-RENDERWORLD-INVARIANTS.md` established `InstanceRecord::chunk` as the single semantic ownership write and `ChunkRecord::members` as a derived ordered reverse index, never an independently authored relationship.
- staging `RenderWorld` implemented atomic instance publication, reparent, and retire maintenance of that derived index and fail-closed resource retirement while referenced;
- run `33886312639` at staging head `6733a1268b2f7f2ade5e7374e65aeaea85488ea5` passed the standalone RenderWorld relationship compile/runtime smoke plus neutral-boundary and whitespace checks;
- later commits carried the invariant and record-surface audit documents but their workflow runs were skipped because the staging workflow intentionally ran only when the commit message contained `[renderworld-qc]`;
- there is no evidence that the single-semantic-write relationship rule was rejected; the branch simply was not promoted and later accepted work diverged.

This recovered rule matches the locked neutral-render architecture and is therefore adapted into the current CP2 hardening rather than imported as branch ownership.

## 4. Final RenderWorld hardening rule

The preliminary hardening's manually writable chunk membership path is superseded before CP3.

Final rule:

- `InstanceRecord::chunk` is canonical semantic ownership;
- `ChunkRecord::members` is derived and cannot be supplied on chunk create;
- instance commit establishes derived membership atomically with rollback on failure/exception;
- `reparentInstance()` moves old/new membership and instance ownership in one published RenderWorld revision;
- generic instance update cannot change chunk ownership;
- generic chunk update cannot change derived members;
- instance retire removes derived membership in the same revision;
- chunk retire requires no live members/references;
- mesh/material/skeleton retirement fails closed while referenced by a live instance;
- versioned resource updates preserve handle slot/generation and require a strictly newer `ResourceRevision`;
- `RenderWorld::valid()` remains an expensive checkpoint/test audit for live dependencies and bidirectional membership.

Main `components-tests` are expanded with relationship, stale-generation, retirement, generic-update bypass, deterministic-membership, and versioned-update coverage so the OpenGL control lane also validates this neutral foundation.

## 5. ResidencyLedger hardening

The original box probe did not exercise enough churn to expose several bookkeeping failure modes that matter when CP3 begins uploading/retiring real meshes and textures.

Hardening keeps logical lifetime separate from backend residency but makes mutation fail safely:

- `queueRetire()` publishes its retirement ticket before changing counters, so `vector::push_back()` allocation failure cannot leave phantom pending bytes;
- queued class bytes are overflow-protected;
- `collectRetired()` validates every required subtraction before mutating any counter, preventing partial accounting mutation if an invariant is violated;
- aggregate telemetry saturates rather than wrapping;
- tests verify that an overcommitted retirement is rejected without changing accounting.

This is correctness hardening only, not an eviction policy or performance mechanism.

## 6. FrameRenderState hardening

The initial `lodScale <= 0` check allowed NaN through because NaN comparisons are false.

The hardened snapshot rejects non-finite:

- simulation time and frame delta;
- jitter/projection offsets;
- LOD scale;
- current/previous camera position, orientation, view and projection matrices;
- current/previous dynamic transforms;
- environment scalar/vector/color values including fog, sun and water state.

Duplicate frame-local view indices and duplicate dynamic-instance entries fail closed. This still does not add the CP3 skeleton-pose/morph payloads; those remain part of the real renderer slice.

## 7. SDL/VSG ownership and dependency audit

### Window ownership

No speculative `SdlVulkanWindow::releaseWindow()` change is justified. VSG's native window implementations likewise use `releaseWindow()` to sever the native handle, while ordered teardown is owner-managed. CP2 destroys the SDL native window only after VSG viewer/window references are released.

### VSG version

VulkanSceneGraph v1.1.16 is now an official upstream release and includes available-memory checks plus paging thread-safety/out-of-GPU-memory fixes that are attractive for CP3. Current upstream vcpkg still resolves `vsg` to 1.1.15#1, which is the version already validated by CP2.

Decision: keep reproducible VSG 1.1.15#1 for CP2. Do not smuggle an ad-hoc overlay-port dependency change into final CP2. Evaluate 1.1.16 as an explicit isolated dependency delta once it can be pinned reproducibly or deliberately carried as a reviewed overlay.

## 8. What remains CP3 work rather than CP2 hardening

Current accepted lineage still has no authoritative `RenderWorldUpdateBatch` implementation or semantic renderer service. The current persistent records are also intentionally too thin for the complete locked CP3 vocabulary.

Recovered `V4-CP1A-RECORD-SURFACE-AUDIT.md` clarifies the correct sequence: complete the neutral semantic record surface before freezing update-batch operations. Therefore CP3 should not start by inventing batch operations against under-specified records.

The first CP3 sub-checkpoints should be:

1. **CP3A — neutral semantic record surface:** source/donor-driven bounds/topology/geometry ownership metadata; texture role/color-space/sampler semantics; V3.25 material semantics and texture-role bindings; skeleton hierarchy/bind/inverse-bind/stable bone indices; instance bounds/LOD/attachment/category semantics; remaining light semantics. No OSG/VSG/Vulkan ABI fields.
2. **CP3B — publication contract:** immutable ordered `RenderWorldUpdateBatch`, epoch/sequence validation, deterministic atomic apply, read publication, and minimal semantic renderer service/backend selector. Extend referential-integrity rules to new relationships such as material→texture.
3. **CP3C — first static interior Vulkan slice:** adapt the donor NIF/material implementation through the neutral records and VSG backend while preserving final V3.25 NIF/RCN/PBR/alpha semantics. No persistent `MWWorld::Ptr -> vsg::Node` ownership shortcut.
4. **CP3D — dynamic actor slice:** skeleton/controller/animation/skin/morph neutral data plus immutable current/previous pose publication, then one NPC/actor path with serial fail-closed fallback where required.
5. **CP3E — minimum GUI/playability bridge:** enough MyGUI/game UI integration to make the interior slice genuinely usable.

Every sub-checkpoint keeps the existing OSG/OpenGL path buildable and testable as the parity control.

## 9. Engine integration disposition

`OMW::Engine::createWindow()` remains correctly hard-wired to `SDL_WINDOW_OPENGL`, `GraphicsWindowSDL2`, OSG camera and OSG viewer for the control backend. CP3 should not turn that function into a mixed OpenGL/Vulkan ownership tangle.

Use startup-scoped backend selection with a distinct VSG/Vulkan construction path sharing platform/input/game semantics above the renderer boundary. The Vulkan path must consume neutral RenderWorld/frame publication rather than requiring OSG scene objects as its source of truth.

## 10. CP3 readiness gate

Before branching CP3:

1. final RenderWorld/ResidencyLedger/FrameRenderState hardening must pass FoundationWindows and full OpenGL control QC on one exact head;
2. the user must eventually repeat the corrected extreme-wide/extreme-tall Vulkan probe resize test and clean shutdown on the final exact hardening artifact.

Once those two gates pass, no known CP2 source defect justifies delaying CP3. Missing richer records, update batches, renderer service, real NIF/material/animation/NPC/GUI integration are explicitly the first CP3 implementation layers, not waived prerequisites.
