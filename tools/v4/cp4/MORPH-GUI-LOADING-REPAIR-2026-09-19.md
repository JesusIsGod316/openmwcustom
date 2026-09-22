# CP4F omitted-morph and GUI-loading repair

## Source and acceptance state

Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Changes are uncommitted atop preserved
recovery WIP. No commit, push, CI dispatch, normal-profile edit or game launch.
Cached Shared Archive AI_CONTEXT_CONTROL_BLOCK/decision locks and CP4F events
108-111 were read, followed by current local repair records and source. The
archive cache is not a fresh remote snapshot.

Full MSVC 14.44.35207 RelWithDebInfo application build passed. Existing warnings
remain. Executable:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`

SHA256: `3bea6644feca0e8b8bd80a73d00731fd22627ba4d6d5bbe9f859dc8ace6b8ad6`.

Implementation and focused validation passed. Exterior gameplay, save-load
compatibility, visual parity, sustained FPS and CP4F acceptance remain unproven.
Broad CP6 optimization, PBR changes and upstream development-build merging were
not attempted.

## Trigger and diagnosis

Run `20260919-193520-gameplay-80104`, previous executable `4604ba2a...`, exited 1.
First frame failure was 19:39:46; later fatal-dialog dismissal at 19:42:04 is not
load time. Error: actor morph controller has no morphed mesh: Tri Shadow_Fishead.
The measured exterior transition was 101241.037 ms, including terrain preload
37266.638 ms and central Seyda Neen render/physics insertion 53243.691 ms. Within
that central insertion, canonical render was 6100.057 ms and V4 publication
1012.137 ms. Nested timings overlap and are not GPU timings or a controlled
cross-run benchmark.

The installed VFCR `meshes/r/xbabelfish.nif` reproduces that exact morph error in
the pre-repair production translation/actor-plan test. File SHA256:
`2a13625a4b5fcaa6e152159d2663692fe2a8fb84a56d8702b97d628c944521b0`.

Canonical NifOsg skips legacy shadow/tri shadow geometry before creating its
drawable/morpher. The neutral translator also omitted the mesh, but retained a
Morph controller requirement. The strict actor validator therefore demanded
deformation data for deliberately omitted geometry.

The GUI bridge previously built a GUI-shaped input but called normal semantic
renderFrame. That path flushes populations and synchronizes static/dynamic world
resources. A loading-screen progress refresh could therefore realize partial
world state during object insertion. This is a confirmed routing defect; its
share of the 101-second transition is not yet measured.

## Changes

- Omitted source geometry no longer publishes a drawable Morph requirement.
  ControllerTarget stays consistent with the remaining controller flags. Node
  identity, transforms, other controller semantics and actual visible morphs
  remain. The actor validator still rejects missing morph data on real geometry;
  diagnostics now include source model identity. No filename exception, new
  shadow rendering, asset alteration or blanket actor skipping was added.
- Added a dedicated GUI-only semantic/host route. It does not flush pending
  populations or synchronize local lights, static populations/instances or
  actors/effects. The main scene is hidden with a persistent VSG Switch, without
  discarding its resident resources. Gameplay reenables the scene and resumes
  ordinary synchronization. Swapchain, completion polling, GUI publication and
  submission tracking remain active. World residents are not marked as used by
  GUI-only submissions. World-bearing GUI frames are rejected explicitly.
- `OPENMW_V4_GUI_WORLD_CONTROL=1` selects the former world-rendering route from
  the GUI bridge for same-executable comparison. Normal gameplay is unchanged.
- The cell insertion diagnostic includes `progress_ms`, covering progress/UI
  callbacks around insertion and navigation loops. It is aggregated per cell;
  normal diagnostics-disabled execution does not take these timestamps.
- Added/extended regression gates, including a real Vulkan GUI/world test and
  a source-identity-rich negative morph test. No loading FPS cap, terrain-grid
  reduction, generic adaptive deferral or OpenGL rendering fallback was added.

## Validation

- 13/13 native/package CTest cases passed after affected binaries were rebuilt.
- 7/7 CP4 source contracts and 15/15 Python collector tests passed.
- Real Vulkan test with shadow and water resources enabled: 24 GUI presentations
  interleaved with gameplay retain residents, do not realize pending objects,
  resume world synchronization, and still reject an actor with no evaluated pose.
  Repair route exits 0. The same test executable's `--world-control` exits 1 at
  `GUI realized pending world objects`, reproducing the former defect.
  This bounded test opens its own small window, not the game or a user save.
- Actual xbabelfish now publishes, plans and evaluates both visible draws,
  including one active morph payload; Shadow_Fishead/Tri Shadow_Fishead nodes
  remain present but non-drawable. This tests bind-pose/zero-weight evaluation,
  not visual animation parity or GPU pixels for the fish.
- Actual scrib retains 43 rigid draws. Actual kwama 30-second timeline passes
  902 frames, 902 body captures and 771 particle captures.
- Existing shoulder/skin-space, resource-retirement, missing-morph rejection,
  shader-package and update-only tests remain passing. git diff --check passes.
- Development checks caught and corrected the SDL3 include/resolver fixture
  setup and the ControllerTarget invariant after clearing Morph. One MSVC test
  link reported LNK1163; a subsequent build retry succeeded without changing
  source or deleting user/generated files. No failed gate was waived.

Build-directory evidence: `morph-gui-qc-build.log`, `morph-gui-gate-build.log`,
`morph-gui-ctest.log`, `morph-gui-isolation-test.log`,
`morph-gui-isolation-control.log`, and `morph-integration-*.nif.log`.

## Preservation and next test

All five tracked normal config/storage hashes match the preceding run manifest.
Protected `_zZz__Wake_up.omwsave` SHA256 remains
`d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`.
Save schema/content, mod order, assets, shader/PBR configuration, threaded Lua,
shoulder repair and mutable particle-bounds repair are untouched.

Next manual run: ordinary GUI-only route (no GUI_WORLD_CONTROL), isolated
writable config, same content and ship-to-Seyda Neen route. Check exterior
completion and rendering, loading totals including progress_ms, first gameplay
frame cost, resident counts, FPS and subsequent errors. Less loading-screen work
may leave first-frame realization work, so do not claim a total loading speedup
or broad compatibility pass from the bounded tests. Existing-save testing remains
required independently of new-game testing.

## Exact files changed in this repair

- `apps/openmw/CMakeLists.txt`
- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `apps/openmw/mwworld/scene.cpp`
- `components/nifrender/niftranslator.cpp`
- `components/render/backend/vsg/dynamicactorplan.hpp`
- `components/render/backend/vsg/vsgsemanticsession.cpp`
- `components/render/backend/vsg/vsgsemanticsession.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `tools/v4/cp3d/dynamic-actor-plan-smoke.cpp`
- `tools/v4/cp4/effect-capture-tests.cpp`
- `tools/v4/cp4/gui-isolation-tests.cpp` (new)
- `tools/v4/cp4/runtime-blocker-contract.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- This repair record (new).
