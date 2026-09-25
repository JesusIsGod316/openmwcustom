# VulkanMW Phase 0 — Foundation

Status: IN PROGRESS
Branch: vulkanmw/phase0-foundation
Base: optimizedmw/gl-p1p2-repair-ffpb@41ff409f8ba8fce55f57eecb8ece45b196aa000b
Parked V4 reference: codex/cp4f-material-frame-repair@be2869dd00a49774ff2642ec238d98f1456b2b8a

## Purpose

Phase 0 establishes the source-control and dependency boundary for VulkanMW before native rendering migration begins.

VulkanMW is not CP4G and is not another optimization pass over the retained OSG-capture Vulkan renderer. The old V4 route remains preserved for compatibility forensics and semantic reference.

## Architectural boundary

Normal VulkanMW:
OpenMW semantics -> RenderWorld -> FrameRenderState -> VSG/Vulkan.

Forbidden as the target steady-state route:
OpenMW -> OSG scene -> OSG traversal/capture -> RenderWorld -> VSG/Vulkan.

## Foundation rules

1. RenderCore stays backend neutral.
2. New native Vulkan semantic producers do not depend on the OSG scene graph.
3. The VSG backend consumes semantic data and does not mutate/query authoritative gameplay state.
4. Existing v4 OSG capture files are transitional/quarantined.
5. Do not add new normal-path responsibilities to transitional capture when a native producer is practical.
6. OpenGL remains the compatibility/control renderer during migration.
7. Saves, content mods, Lua/gameplay semantics and animation-event behavior remain compatibility gates.
8. Vulkan performance claims require matched runtime evidence; Phase 0 makes none.

## Existing reusable foundation

- components/rendercore/renderworld.hpp
- components/rendercore/framerenderstate.hpp
- components/rendercore/frameproducer.hpp
- components/rendercore/records.hpp
- components/render/backend/vsg/*
- current terrain/resource-lifetime/auxiliary-view/UI infrastructure

## Transitional debt inventory

Primary files/classes to remove from normal Vulkan runtime over later phases:
- apps/openmw/mwrender/v4updateonlyviewer.hpp
- apps/openmw/mwrender/v4persistentobject.hpp
- apps/openmw/mwrender/v4rigidactorpose.hpp
- apps/openmw/mwrender/v4effectcapture.hpp
- AnimatedObjectCaptureVisitor in v4enginerenderbridge.cpp
- MorphCollector in v4enginerenderbridge.cpp
- generic whole-object OSG capture plans
- normal CPU deformed actor MeshPayload generation

These are not deleted in Phase 0.

## Immediate Phase 0 tasks

- [x] Create VulkanMW branch from accepted OpenGL foundation.
- [x] Commit architecture plan.
- [x] Add dependency-boundary checker.
- [ ] Inventory each V4 producer as NATIVE / TRANSITIONAL / REFERENCE-ONLY.
- [ ] Add a lightweight source gate for the boundary checker after its first clean run.
- [ ] Define the direct NIF semantic compiler API and source location.
- [ ] Begin Phase 1 only after the Phase 0 inventory shows no hidden dependency that would force a live OSG scene.

## Phase 0 exit gate

Phase 0 closes when:
- the boundary checker is green,
- producer inventory is committed,
- OpenGL control still builds unchanged,
- VulkanMW source scaffolding builds,
- direct NIF compiler interface is ready for Phase 1,
- no runtime/performance acceptance is claimed.
