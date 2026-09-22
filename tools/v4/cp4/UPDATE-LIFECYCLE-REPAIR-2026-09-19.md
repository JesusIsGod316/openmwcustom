# CP4F CPU scene-update lifecycle repair

## Status and acceptance boundary

2026-09-19, branch `v4.0-cp4f-exterior-closeout`, base HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Uncommitted repair on preserved
recovery/instrumentation WIP. No push or CI run. CP4F is NOT accepted; CP5 stays
blocked. Shared Archive control and checkpoint 111 checked before implementation.

Existing saves, including the user's modded Leon Valerius save, are a required
acceptance case. New-game success alone is insufficient. This repair changes no
save schema, deserialization, content selection, Lua worker settings, or save
bytes. Retain the normal mod load order. Do not modify assets/saves to mask engine
failures, disable Lua threading, or substitute OpenGL rendering for Vulkan.

## Evidence and mechanism

Previous run `20260919-000224-gameplay-62756`: 789 sampled frame starts, every OSG
stamp 0, viewer done on every sampled update, no camera/skeleton callbacks. All
730 picks remained barrel_01_empty despite changed render views. Moving actor
placements had fixed local poses. These are sampled observations, not a benchmark.

OSG 3.6.5 Viewer::eventTraversal checks window status and sets done when no OSG
graphics window exists. Viewer::advance and updateTraversal then return early.
The former Vulkan loop bypassed done only in its loop condition, not in those
methods. Source: https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osgViewer/Viewer.cpp

V4UpdateOnlyViewer replaces only the Vulkan CPU viewer's event traversal. It
dispatches device/queued/frame events and scene/camera/registered callbacks but
does not inspect OSG window lifetime. Canonical advance/updateTraversal remain
inherited. SDL and the state manager own normal quit and Escape; explicit done
is still honored. Accidental OSG realize/render calls fail explicitly.

UpdateOnlyVisitor marks the CPU traversal so SemiActive skeletons do not depend
on a nonexistent OSG cull traversal. Inactive/range suppression is preserved.
The ordinary OpenGL viewer and its offscreen policy remain the control path.

## Exact files in this repair

- apps/openmw/mwrender/v4updateonlyviewer.hpp (new CPU viewer)
- components/sceneutil/updateonlyvisitor.hpp (new update policy marker)
- components/sceneutil/skeleton.cpp (scoped cull gate bypass)
- apps/openmw/engine.cpp (backend-specific viewer, honor explicit done)
- apps/openmw/CMakeLists.txt (behavioral test target)
- tools/v4/cp4/update-only-tests.cpp (new linked-OSG tests)
- tools/v4/cp4/UPDATE-LIFECYCLE-REPAIR-2026-09-19.md (this record)

## Verified checks

- MSVC RelWithDebInfo openmw production compile/link PASS; existing conversion,
  unused-variable and CMake dependency warnings remain.
- New headless suite: reproduce original no-window shutdown; 12 advancing
  simulation frames and evaluated bone poses; real projection-ray intersections
  alternate between two targets with camera callbacks; queued Escape delivered
  without shutdown; scene/camera/frame events delivered once; loading-order
  update/advance supported; explicit done respected; no graphics context;
  Inactive and OpenGL offscreen policy retained; GL presentation rejected.
- Initial test expected the Geode at the end of an intersection path; OSG also
  includes the Drawable. Corrected the fixture to inspect named target ancestry.
  All four new behavioral cases then PASS.
- Four native CTest suites PASS. Eight Python diagnostic tests PASS. Seven CP4
  source contracts PASS. git diff --check PASS (line-ending warnings only).
- Production executable SHA256:
  `a0bcdfdf0e302123ca01c1a9badfea145cb3fbf5ee779aa30d9299d044ad3f31`.
- Existing save before manual test: 9,815,914 bytes, SHA256
  `d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.
  Path: `C:\Users\LSCha\Documents\My Games\OpenMW\saves\Leon_Valerius\_zZz__Wake_up.omwsave`.

## Required manual acceptance

Use gameplay-diagnostics.py's private writable config layer with normal content
and save visibility. Load the existing save, not just New Game. Check camera and
movement, changing focus labels and activation, animated NPCs/equipment, inventory
and saved script state, and the previously missing bed/materials. Exit normally;
the collector has no forced timeout. Compare sampled frame stamps, viewer_done,
camera deltas, actor pose variation, stream discrepancies, stage times and errors.
Preserve user screenshots and the run manifest, not only the last log.

Still unproven: full canonical-to-neutral skin-space equivalence, individual
body-part placement, texture/material semantics, local-map output, shader
compatibility, intro/loading stall and runtime performance/VRAM budgets. Restoring
updates can expose further failures; neither synthetic tests nor zero validation
errors establish complete modded-save compatibility.
