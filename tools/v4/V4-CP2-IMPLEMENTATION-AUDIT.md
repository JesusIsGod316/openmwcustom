# V4.0 CP2 — VSG/Vulkan Foundation Implementation Audit

Status: **IMPLEMENTATION STARTED — WINDOWS QC QUEUED**
Branch: `v4.0-cp2-vsg-vulkan-foundation`
Exact parent: `40227915a9069d74743eb1a344e28cdeb9ee68d6` (`v4.0-cp1b-sdl3-opengl-parity`)
Checkpoint objective: **Windows VSG/Vulkan foundation + real CI validation + VRAM/resource lifetime**
Performance claim: **NONE**

## 1. Audit correction: donors are not preferred over a better local design

CP0A correctly classified David P and Clockwork, but `PRIMARY DONOR` never means `copy first`.

CP2 uses this rule:

1. final V3.25 / promoted V4 semantics and ownership invariants win;
2. choose the simplest design that satisfies the V4 contract cleanly;
3. use donor code directly only when its implementation is better than a local implementation after adaptation;
4. otherwise use donor code as evidence, test oracle, algorithm reference or failure-history reference;
5. never import donor ownership merely to save porting effort.

That changes the CP2 implementation from "port David's backend wholesale" to "build the V4 backend boundary we actually want, borrowing mature mechanisms when they are superior."

## 2. CP1 contract implementation discrepancy

The locked CP1 contract specified `updatebatch.hpp`, `framerenderstate.hpp` and a semantic renderer service in addition to handles, identities, slot tables and `RenderWorld`.

The promoted CP1B source contains the accepted compile-tested subset:

- `handles.hpp`
- `math.hpp`
- `resources.hpp`
- `slottable.hpp`
- `renderworld.hpp`

Git history on the accepted CP1A ancestry does not contain a committed `components/rendercore/framerenderstate.hpp`. Therefore CP2 does **not** claim that a specific committed FrameRenderState implementation was removed by a known compiler error. The safest interpretation is that CP1A implementation was narrowed during its build/QC cycle while the larger contract remained the architectural handoff.

CP2 restores only the neutral state needed now, in compile-isolated form:

- immutable-by-interface `FrameRenderState` snapshot;
- explicit variable `FrameView` list;
- render/output extents;
- current/previous camera state;
- history identity/validity;
- dynamic current/previous transforms;
- initial environment value state.

The full ordered `RenderWorldUpdateBatch` producer conversion remains deferred rather than being reintroduced speculatively into the proven OpenGL path. It must be completed before Vulkan becomes the full game renderer, but it is not allowed to block isolated CP2 device/present validation.

## 3. Why CP2 uses an isolated foundation target

`OMW::Engine::createWindow()` currently creates an SDL3 `SDL_WINDOW_OPENGL` window and immediately builds `SDLUtil::GraphicsWindowSDL2`, OSG camera state, realize operations and the OSG viewer.

Forcing a Vulkan branch through this function during CP2 would mix:

- platform migration;
- backend selection;
- OSG teardown assumptions;
- gameplay renderer integration;
- Vulkan device bring-up;

in one failure domain.

CP2 therefore builds a real standalone foundation executable from the same repository:

`openmw-vulkan-foundation.exe`

This is not a header-only or mock validation. It creates:

`SDL3 window -> VkSurfaceKHR -> VSG instance -> physical device -> logical device -> swapchain -> VSG viewer -> command graph -> real graphics pipeline/shaders -> submitted/presented frames`

The existing OpenMW executable remains the OSG/OpenGL control and is rebuilt/tested in the same CP2 workflow.

CP3 is the checkpoint that connects a real OpenMW interior/NIF/material/animation/NPC/GUI slice to this backend.

## 4. SDL3/VSG window ownership

A fresh `RenderVsg::SdlVulkanWindow` adapter is used instead of inheriting David's broader renderer ownership.

Rules:

- SDL3 remains native-window and event/input authority;
- VSG receives the SDL-owned window only through a narrow Vulkan surface adapter;
- VSG owns instance/device/surface/swapchain resources below the backend boundary;
- VSG resources are released before `SDL_DestroyWindow`;
- resize conservatively rebuilds the swapchain after device-idle synchronization in CP2;
- no OSG, VSG or Vulkan type enters `components/rendercore`.

David's `vsgsdlwindow` remains a useful implementation reference, but the CP2 adapter is purpose-built for the promoted CP1B SDL3 ownership model.

## 5. Dependency pin

SDL remains exactly:

- SDL3 `3.4.10`
- VC development archive SHA-256 `e2b336b10b037934af98308027410732ef7b22f2c6697d58092aa1c209fae7d7`

VSG is initially pinned through exact vcpkg baseline:

- vcpkg baseline `04a9d8e5212d01ee1dd9478eadd9caade4f8b0d4`
- VSG package `1.1.15` at that baseline

VSG 1.1.16 is newer and contains useful memory/OOM/paging fixes. It is deliberately not silently substituted during first bring-up because the current vcpkg package provides a reproducible Windows integration point. After a green CP2 foundation, 1.1.16 can be evaluated as an explicit dependency-only delta.

## 6. VRAM and residency architecture

CP2 adds `RenderVsg::ResidencyLedger` from day one.

Tracked categories:

- textures/images;
- mesh vertex/index;
- skinning;
- instance/material/light;
- render targets/history;
- shadow maps;
- water targets;
- postprocess;
- staging/temporary.

Per-category state tracks:

- logical-live bytes;
- resident bytes;
- pending-upload bytes;
- pending-retire bytes;
- pinned bytes;
- evictable bytes.

Backend retirement is separated from logical lifetime. Resident allocations can remain charged until their `lastUseFrame` has completed; collection then releases backend residency without invalidating a logically live resource.

The foundation probe also queries Vulkan physical-device memory heaps and uses `VK_EXT_memory_budget` when supported, reporting adapter heap budget/usage separately from OpenMW-owned category accounting.

The project 8 GB VRAM constraint remains a promotion gate.

## 7. Pipeline proof

The CP2 probe creates visible geometry with VSG's Builder, constructs a camera/view/command graph, and calls `viewer->compile()` before entering the frame loop.

This proves more than Vulkan loader availability:

- VSG render-graph creation;
- shader module/pipeline construction;
- vertex/index/resource compilation;
- framebuffer/swapchain compatibility;
- record/submit/present path.

OpenMW shader-mod/postprocessing shader compilation is intentionally not ported in CP2; that belongs to feature/parity checkpoints after the backend itself is stable.

## 8. Windows CI topology

`.github/workflows/v4-cp2.yml` has three gates:

### Preflight

- exact promoted CP1B ancestry;
- CP0A materialized-source provenance;
- RenderCore backend-purity scan;
- required CP2 source/pin checks;
- whitespace validation.

### FoundationWindows

- Windows Server 2022 + MSVC + Ninja;
- exact vcpkg baseline made available;
- standalone CP2 VSG/Vulkan configure and build;
- FrameRenderState + residency contract tests;
- install/stage `openmw-vulkan-foundation.exe`, SDL3 and VSG/Vulkan runtime dependencies;
- artifact and build-evidence upload.

The Vulkan window is not required to execute on the GitHub-hosted VM because availability of a suitable presentation-capable Vulkan device is not a reliable CI assumption. Runtime presentation is a user-hardware gate.

### OpenGLControlWindows

The existing full reusable Windows OpenMW build/test/install/benchmark workflow runs from the same CP2 head. This proves the CP2 foundation has not stranded the promoted SDL3 + OSG/OpenGL correctness control.

## 9. CP2 acceptance gates

1. Exact ancestry from `40227915...` — **PASS**.
2. V3.25/CP1B OSG/OpenGL control source untouched by Vulkan bootstrap — **PASS by design; CI pending**.
3. Backend-neutral FrameRenderState restored without backend leakage — **IMPLEMENTED; CI pending**.
4. Pinned reproducible VSG/Vulkan dependency path — **IMPLEMENTED; CI pending**.
5. SDL3 Vulkan surface adapter — **IMPLEMENTED; compile/runtime pending**.
6. VSG instance/device/swapchain/viewer/command graph — **IMPLEMENTED in probe; compile/runtime pending**.
7. Real shader/pipeline/renderable scene proof — **IMPLEMENTED in probe; compile/runtime pending**.
8. Resize/swapchain recreation and ordered shutdown — **IMPLEMENTED; runtime pending**.
9. Explicit VRAM categories and logical-vs-resident lifetime split — **IMPLEMENTED; tests pending**.
10. Frame-delayed backend retirement accounting — **IMPLEMENTED; tests pending**.
11. Adapter heap/budget telemetry — **IMPLEMENTED; runtime pending**.
12. Windows foundation artifact with runtime dependencies — **CI pending**.
13. Full Windows OpenMW control build/tests/benchmarks — **CI pending**.
14. User-hardware Vulkan present/resize/shutdown smoke — **PENDING after green artifact**.
15. Performance promotion claim — **FORBIDDEN at CP2**.

## 10. Explicitly not pulled into CP2

- NIF/VSG content conversion;
- NPC assembly/animation/skinning gameplay integration;
- terrain/groundcover;
- water/shadows/sky/weather;
- MyGUI/maps/previews;
- postprocessing parity;
- Clockwork indirect GPU scene;
- Hi-Z occlusion;
- Jolt;
- DLSS/Reflex/Frame Generation.

These remain later checkpoints. Their donor code can be audited/ported when each subsystem becomes the active checkpoint, and can be rewritten when a better V4-native design exists.

## 11. CP3 handoff condition

CP3 begins only after CP2 has:

- green FoundationWindows compile/tests/install;
- green full OpenGL control QC;
- a user-hardware Vulkan probe that creates/presents frames and survives resize/shutdown;
- no unresolved residency/lifetime correctness defect discovered by the probe.

Then the first real Vulkan OpenMW interior slice can be connected without changing the neutral ownership direction again.
