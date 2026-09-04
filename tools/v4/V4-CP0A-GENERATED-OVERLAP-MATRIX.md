# V4.0 CP0A — Generated V3.25 / Donor Overlap Matrix

Materialized V3.25 source head at time of this audit: `b024c179ed5cffb68fd2c7e6e03952c1744b2f3d`.

This document classifies the 103 generated V3.25 source files by collision risk with the V4 donor work. The purpose is to prevent a mechanically successful David/GLM/VSG port from silently erasing custom V3 behavior.

## Risk vocabulary

- **P0 / CRITICAL** — donor code and generated V3.25 change the same ownership/rendering surface. Reconcile before or during the first CP1 edit.
- **P1 / HIGH** — not necessarily backend code, but defines semantics/API/streaming/settings that the renderer boundary consumes.
- **P2 / PRESERVE** — must survive the V4 base materialization; not a CP1 architecture blocker unless a donor edit collides.
- **LEGACY_ONLY** — OSG/OpenGL mechanism remains in the comparison backend but is not copied into the Vulkan architecture unless its semantic rule is still applicable.

## A. Build/bootstrap/frame integration — P0

| V3.25 generated files | V3.25 role | David overlap | Merge rule |
| --- | --- | --- | --- |
| `CMakeLists.txt` | custom diagnostics/NIS/generated source build additions | SDL3 + optional VSG/Vulkan dependencies | preserve V3 build options while adding SDL3/VSG in isolated CP1B/CP2 commits; no wholesale donor CMake replacement |
| `apps/openmw/CMakeLists.txt` | generated renderer/Lua source list changes | David adds VSG renderer/application sources | merge source lists explicitly; retain all materialized V3 files |
| `apps/openmw/engine.cpp` | large V3 frame/diagnostic/focus/paging/preload/QoS integration | David SDL3/VSG frame loop and renderer selection | **highest-risk merge**; retain V3 frame ordering/semantics, isolate renderer backend invocation behind a neutral service; do not transplant David engine.cpp wholesale |

The materialized `engine.cpp` delta alone is roughly +528/-24 lines and touches frame timing, focus cadence, paging/preload, render/update boundaries and diagnostics. It is not a disposable harness file.

## B. Animation, actors, camera, FBFP — P0

| V3.25 generated files | Locked V3.25 semantics | David donor area | Merge rule |
| --- | --- | --- | --- |
| `mwrender/animation.cpp/.hpp` | Mode150 batched source finalization; Mode151 actor-sized controller-clone job; fail-closed serial fallback; deterministic publish; controller/source order | `animation*`, `vsganimation*`, NifVsg animation/controller/skin code | separate **game animation semantics** from **backend pose representation**; never replace materialized V3 Animation with donor implementation wholesale |
| `mwrender/npcanimation.cpp/.hpp` | FBFP owner-view behavior, actor source batching call sites, current equipment/animation assembly semantics | `npcanimation*`, `vsgnpcassembly*`, `objectsvsg*` | V3 assembly/event/body-part semantics authoritative; port VSG representation underneath |
| `mwrender/camera.cpp/.hpp` | full-body first-person camera behavior, projection offset, owner/shadow view distinctions | David transitional OSG dummy-camera/VSG bridge | do not preserve David dummy OSG camera as final architecture; CP1 neutral camera state must carry V3 semantics |

### CP1 consequence

`SkeletonHandle` and actor/instance identities must not imply that VSG owns animation semantics. Simulation/animation chooses pose/source/order; backend consumes a published pose/resource state.

## C. Objects, cells, paging, terrain, groundcover — P0

| V3.25 generated files | V3.25 custom surface | Donor overlap | Merge rule |
| --- | --- | --- | --- |
| `mwrender/objects.cpp/.hpp` | insertion/removal, V3.2 renderer-safe exterior hibernation/restore, occlusion hooks | David `ObjectsVsg`, `VsgWorldBridge`, actor/static placement | translate object lifecycle into neutral `RenderWorldUpdateBatch`; OSG objects remain legacy adapter only |
| `mwrender/objectpaging.cpp/.hpp` | exact-active paging, custom caches/frontload/occlusion/paging experiments and retained semantics | David terrain/distant statics/object bridge; Clockwork persistent chunks | preserve paging visibility/content semantics; map produced static chunks into generation-safe RenderWorld chunks rather than VSG scenegraph ownership |
| `mwrender/groundcover.cpp/.hpp` | custom groundcover/occlusion/cache behavior | David VSG groundcover; Clockwork instanced persistent scene | retain density/distance/animation/visual semantics; Vulkan submission may use donor instancing |
| `components/terrain/chunkmanager.cpp` | V3 terrain custom behavior | David VSG terrain/quadtree | preserve chunk/LOD content semantics; backend representation changes below boundary |
| `mwworld/cellpreloader.*`, `mwworld/scene.*` | custom streaming/preload/commit behavior | David cell/VSG world integration | P1 semantic producer; CP6 eventually converts live renderer reaches to generation-safe update batches |

### Clockwork translation

Clockwork `Chunk { generation, firstInstance, instanceCount, batchCounts, meshes, materials, bounds, lightBase }` confirms that a **chunk/group identity with generation** belongs in CP1. V4 must not wait until CP7 to invent this.

## D. RenderingManager ownership seam — P0

`apps/openmw/mwrender/renderingmanager.cpp/.hpp` is the largest ownership collision. Materialized V3 adds roughly +411/-19 lines in the implementation and currently exposes direct object/cell/move/rotate/scale/water/sky/camera/postprocess/paging operations while owning OSG Viewer/Group, LightManager, WorkQueue, terrain, ObjectPaging, Groundcover, Sky, Water and PostProcessor.

David solves Vulkan bring-up by adding `RenderingManagerVsg` plus a live mutable `VsgWorldBridge`. That is useful transitional code but not our final public seam.

**Locked merge rule:**

1. keep final materialized `RenderingManager` as the legacy OSG implementation/control;
2. extract a high-level, backend-neutral semantic producer/renderer service alongside it;
3. game/world code publishes neutral object/resource/environment changes;
4. legacy adapter applies those changes to current OSG objects;
5. VSG/Vulkan backend consumes the same neutral world/frame data;
6. do not make `osg::Node`, `vsg::Node`, `osgViewer::Viewer`, `osg::Camera`, or VSG viewer types part of the common renderer API.

## E. Postprocessing, render scale, NIS, water — P0

| V3.25 generated files | Meaning | Donor overlap | Rule |
| --- | --- | --- | --- |
| `postprocessor.cpp/.hpp` | API revision 5 behavior, current resources/technique handling, V3 diagnostics/changes | David VSG full-screen/composite/HBAO path | port semantic API/resources first; implementation may change to SPIR-V/Vulkan |
| `pingpongcanvas.*`, `pingpongcull.cpp` | current offscreen/ping-pong render behavior and NIS integration seams | David VSG offscreen/composite path | legacy OSG path remains exact; Vulkan gets native render graph equivalents, not OSG emulation |
| `nisscaler.*`, `v318_nis_config.hpp`, `v318_nis_shader.hpp` | custom NIS scaler/reference feature | no David equivalent | preserve as reference/comparison functionality; Vulkan implementation can be native later, not a CP2 blocker |
| `water.cpp` | custom water camera/render hooks | David VSG water/reflection/refraction/ripples | V3.25 visual behavior wins; donor water RTT visibility gate may be a switchable optimization after parity |

### Temporal consequence

The neutral frame contract must include render size, output size, projection offset/jitter and history identity from CP1 even though DLSS is later. This avoids another ownership rewrite in CP9.

## F. Resource/SceneManager/cache layer — P0/P1

Materialized generated changes overlap:

```text
components/resource/bulletshapemanager.*
components/resource/imagemanager.cpp
components/resource/multiobjectcache.*
components/resource/objectcache.hpp
components/resource/resourcemanager.hpp
components/resource/resourcesystem.*
components/resource/scenemanager.*
```

David's VSG path adds separate VSG texture/model loading and caching. A blind second cache hierarchy would make memory ownership, invalidation and the 8 GB VRAM ceiling difficult to reason about.

**Rule:** distinguish three lifetimes explicitly:

1. **content/source resource lifetime** — VFS/NIF/image/animation semantic data;
2. **RenderWorld logical resource lifetime** — stable `MeshHandle` / `TextureHandle` / `MaterialHandle` / `SkeletonHandle` identities;
3. **backend residency lifetime** — OSG/VSG/Vulkan allocations, descriptors, compiled pipelines, GPU buffers/images.

Backend eviction must not invalidate a logical RenderWorld handle. A resource can be logically live but temporarily non-resident and re-uploadable.

## G. Shadows/occlusion/lighting/work queues — mixed P0 + LEGACY_ONLY

Materialized generated files:

```text
components/sceneutil/framejobservice.hpp
components/sceneutil/mwshadowtechnique.*
components/sceneutil/occlusionculling.*
components/sceneutil/shadow.cpp
components/sceneutil/workqueue.*
```

### Frame-job service

The exact OSG-related implementation is not a Vulkan API contract, but its **scheduler rules are architectural**:

- immutable/pre-resolved worker inputs;
- worker-owned unpublished outputs;
- deterministic main publish;
- bounded immediate work;
- no frame-critical FIFO-then-wait;
- generation/stale-result checks where asynchronous lifetime crosses frames.

### Shadows

OSG `MWShadowTechnique` itself is legacy-only, but owner-head/FBFP/view semantics and visual reference are V4 contracts. David shadow code is `PORT_ADAPT`.

### Occlusion

V3.22–V3.24 OSG MSOC experiments remain historical evidence. Do not mechanically carry their OSG implementation into Vulkan. Clockwork/Hi-Z occlusion are new mechanisms and remain default-off until **zero visible false occlusion**.

### Lighting

Final materialized V3.25 sits on current clustered-lighting-capable source (`LightSettings`, classic per-object path plus clustered grid/cull path). V4 must preserve both semantic capability and settings behavior. Clockwork lighting data may inform GPU scene layout; David lighting is not authoritative where it diverges.

## H. Shader manager — P0

`components/shader/shadermanager.cpp` has generated V3 custom changes and is also the gateway to the current supported postprocess/material shader surface. David's `VsgShaderManager`, `SpvCompiler`, material defines and pipelines are implementation donors.

**Rule:** shader compatibility is split into:

- **semantic preprocessing/API compatibility** above backend;
- **GLSL/OpenGL compilation** in legacy backend;
- **GLSL→SPIR-V/Vulkan compilation and pipeline construction** in modern backend.

Do not expose SPIR-V/VSG pipeline details to Lua/mod-facing APIs.

## I. P1 semantic/API producer overlap

The following materialized V3 files are not primarily renderer implementation, but donor/GLM/SDL work can collide with them and change behavior:

```text
apps/openmw/mwlua/camerabindings.*
apps/openmw/mwlua/engineevents.cpp
apps/openmw/mwlua/globalscripts.hpp
apps/openmw/mwlua/localscripts.hpp
apps/openmw/mwlua/luamanagerimp.*
apps/openmw/mwlua/soundbindings.cpp
apps/openmw/mwmechanics/actors.cpp
apps/openmw/mwmechanics/mechanicsmanagerimp.cpp
apps/openmw/mwphysics/physicssystem.cpp
components/lua/luastate.*
components/lua/scriptscontainer.*
components/settings/categories/{camera,cells,lua,shadows,sound,video}.hpp
components/settings/ramcache.hpp
components/settings/sanitizerimpl.*
components/settings/v36profile.hpp
files/lua_api/openmw/camera.lua
files/settings-default.cfg
```

**Rule:** no whole-repository GLM or SDL3 migration should touch these mechanically without an explicit semantic diff. The CP1 GLM seam starts at renderer-facing neutral values; broad migration can proceed incrementally later.

## J. P2 preserve, non-renderer-first

Generated sound/cache diagnostics and navigator changes remain part of final V3.25 source authority. They do not block CP1 unless a donor touches them. Diagnostics can be retired or redesigned only deliberately after equivalent validation capability exists; they are not a reason to keep the V3 patch harness.

## K. CP0A merge-order conclusion

The safest and fastest convergence order is now:

1. **materialize final V3.25 generated source** — complete;
2. add neutral handle/math/update/frame-state types without changing OpenGL output;
3. adapt current `RenderingManager`/`Objects` producers into the neutral update seam while retaining the exact OSG backend;
4. SDL3 migration with OpenGL still active;
5. transplant David Vulkan infrastructure;
6. port/adapt feature systems against the semantic matrix;
7. only after parity, replace transitional VSG scene ownership with the Clockwork-derived persistent GPU scene.

This is intentionally different from both a David rebase and a scenegraph-to-scenegraph rewrite.
