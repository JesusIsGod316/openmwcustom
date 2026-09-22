# Exterior dynamic compilation and GUI ordering repair — 2026-09-20

## Scope and source state

User requested continued repair of freezing, white terrain, incorrect water,
material artifacts and GUI occlusion. This checkpoint repairs two specific
mechanisms. It does NOT establish exterior playability or terrain/water parity.

Checkout: `OpenMW custom Build-cp4f`; branch
`v4.0-cp4f-exterior-closeout`; HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`.
Existing dirty WIP and the preceding shadow-slot repair are retained.
No commit, push, normal profile change, save load or game launch in this turn.

## Findings and repairs

1. `synchronizeDynamicActors` compiled the complete next actor/effect root each
   frame, even for already-resident mutable draws. It now compiles only newly
   realized/rebuilt residents. Reuse still updates dirty arrays and placements;
   fence-owned versions and retirement remain unchanged. VSG TransferTask keeps
   dynamic buffer registrations rather than replacing them on each assignment.
   New framebuffer views separately compile their complete roots.
2. Depth-disabled GUI was recorded as an ordinary main-view child before VSG
   subsequently recorded deferred world bins. The GUI now occupies an ordered
   overlay layer in bin 11, after world bins 9/10, with MyGUI internal order
   preserved. Main-only view masking remains in place.

Same-executable causal controls (presence enables old behavior):

- `OPENMW_V4_RECOMPILE_DYNAMIC_CONTROL=1`: compile full dynamic root.
- `OPENMW_V4_EARLY_GUI_CONTROL=1`: direct/early GUI child, without overlay layer.

Neither control was set globally or persisted in user settings.

## Exact files edited in this checkpoint

- `components/render/backend/vsg/vsgruntimehost.cpp`: pending dynamic compile
  root; GUI overlay routing and final bin.
- `components/render/backend/vsg/vsgruntimehost.hpp`: last compiled root count
  for regression assertions.
- `components/render/backend/vsg/uipipeline.cpp`: production overlay helper and
  explicit early-GUI control.
- `components/render/backend/vsg/uipipeline.hpp`: helper declaration/bin ID.
- `tools/v4/cp4/texture-residency-tests.cpp`: additional 12-frame `--stable`
  resident-reuse/material-change fixture. Default 150-frame churn is retained.
- `tools/v4/cp4/water-pixel-tests.hpp`: actual production GUI pixel-order test
  against a competing world-bin draw.
- `tools/v4/cp4/water-transition-tests.cpp`: optional `--pixels-only` exit after
  offscreen checks to separate framebuffer validation from presentation.
- `tools/v4/cp4/runtime-blocker-contract.py`: source contracts for both repairs.
- This record.

This is a checkpoint-specific list, not a claim that the entire dirty checkout
contains only these edits.

## Build and validation

MSVC 2022 RelWithDebInfo executable and affected test targets built successfully.
The first attempt failed in the new test with an ambiguous braced assignment
to `vsg::ubvec4`; explicit construction fixed it. Rebuild passed. Existing
compiler conversion/unused-variable warnings remain; not a warning-free build.

Build/evidence directory:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`.

- `exterior-dynamic-gui-build.log`: initial test compilation failure.
- `exterior-dynamic-gui-build-retry.log`: successful rebuild.
- `exterior-pixel-build.log`: optional isolated-pixel mode rebuilt.
- `exterior-unit-build.log`, `exterior-ctest.log`: registered binaries rebuilt;
  14/14 registered tests passed.
- Seven CP4 Python source-contract checks passed; `git diff --check` passed.
- Python diagnostic tests: 16/16 passed.
- `dynamic-stable-fixed.log`: 12 frames; compile root counts are 96 initially,
  1 for one material change, otherwise 0 despite mutable geometry updates.
  Pool size stayed 52,325,824 bytes at the recorded warm/final checkpoints.
- `dynamic-stable-control.log`: expected exit 1 when old full-root compilation
  was enabled, with the resident-recompilation assertion.
- `dynamic-churn-fixed.log`: all 150 real Vulkan frames completed; warm/final
  pool size both 52,325,824 bytes. Residency/churn assertions passed, but the
  presentation validation errors below remain. Not a clean Vulkan pass.
- `water-gui-fixed.log`: pixel assertions and complete active-water/map/GUI
  transition assertions passed.
- `water-gui-control.log`: expected exit 1, deferred world overpainted GUI.
- `water-gui-pixels-only.log`: offscreen water/sky/GUI and view-lifetime checks
  passed with ZERO Vulkan validation errors.
- `gui-isolation-exterior-repair.log`: pending objects untouched, resident
  retention, world resumption and missing-pose rejection assertions passed;
  the same ten presentation validation errors remain.

Full presentation fixtures (including the stable fixture) still emit ten
swapchain/image-view storage-usage validation errors. Thus their behavioral
assertions pass but their full Vulkan validation result is NOT clean.
`dynamic-stable-explicit-only.log` reproduced these errors with implicit Vulkan
layers excluded. This is not proof of a specific overlay or driver cause.
No global overlay settings/processes were changed.

Executable SHA256:
`4B8807C6276BBAD268A48870287083E8B878E6D159340E281A43BBB176B61368`.

## Remaining work / acceptance limits

- White terrain has a concrete source-level cause: `v4terrainsource.cpp` supplies
  positions, normals, vertex colors and a white material but NO UVs, LAND layers,
  blend maps or terrain texture bindings. This checkpoint does not implement
  those missing parts. Geometry-only source contracts do not prove material
  parity. Do not describe this as a missing user texture file or gamma problem.
- Static exterior synchronization still performs many synchronous compilation
  calls. Existing optional batch-upload work has not been promoted by this
  checkpoint or by a representative same-executable gameplay comparison.
- Real exterior/cave water appearance and cyan/green wood material interpretation
  remain unresolved. Synthetic water pixels are not actual scene acceptance.
- Presentation validation errors need further isolation.
- No FPS improvement or freeze elimination is claimed. The controlled graphics
  fixtures are not gameplay performance benchmarks. The exact new-game/intro/
  ship-to-exterior route still needs user runtime acceptance on this hash.
