# CP4F particle/exterior repair — 2026-09-19

## Build and scope

- Checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`
- Branch: `v4.0-cp4f-exterior-closeout`
- HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`; existing WIP preserved; no commit, push, or CI dispatch.
- Compiler: MSVC 14.44.35207, RelWithDebInfo, local Ninja build.
- Executable: `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`
- Executable SHA256: `c8226382e0964719b157c09f52203e8e44251534c82f0e0fd7162ab8c86da082`
- Final application/test build succeeded. Existing numeric-conversion/settings/dependency warnings remain; this is not a warning-free build.
- This repair is not a runtime promotion, an exterior-load benchmark, or a claim of full shader/mod compatibility. No new manual gameplay run was launched.

## Evidence used

Failed run: `C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260919-171639-gameplay-62516`.

The exterior transition completed its recorded CPU operation in approximately 105.375 seconds, with central Seyda Neen `load_cell` accounting for approximately 53.118 seconds. The following frame failed with `actor model requires particle-system playback and realization`. Its last newly translated actor model was `meshes/r/xkwama forager.nif`; the old diagnostic did not identify the exact reference, so that association remains evidence-supported rather than a proven stack trace.

The user's latest screenshots show roughly 25–26 FPS, versus roughly 13 previously. These are separate observational runs, not a controlled benchmark. Remaining present/capture costs are not declared solved here.

## Implemented repairs

### Built-in actor particles

Actors whose base model requires particle playback now use evaluated OpenMW body geometry and particles rather than the persistent pose-only actor route. The route preserves active switches, excludes update-only hidden geometry, keeps body shadow/reflection/refraction semantics, and leaves particles as effects. Attached UpdateVfx draws are captured separately exactly once. Any previous persistent body is retired to prevent duplication. Attached lights still use the common publication path.

The selector is deliberately limited to the particle requirement alone. Other unsupported requirement combinations remain explicit failures, now including actor identity, source model and requirement mask. No actors, materials, or effects are silently skipped.

OSG 3.6's [ParticleSystemUpdater](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgParticle/ParticleSystemUpdater.cpp) and [ParticleProcessor](https://github.com/openscenegraph/OpenSceneGraph/blob/OpenSceneGraph-3.6.5/src/osgParticle/ParticleProcessor.cpp) perform CPU simulation during cull-phase traversal. An ordinary update visitor therefore cannot advance them. A separate active-child particle-only pass runs after canonical update callbacks. It invokes only these particle leaf nodes with an actual isolated CullVisitor carrying the correct frame stamp and node path; it does not cull the scene, accept geometry into that visitor, create a graphics context, or draw through OpenGL. Canonical virtual simulation, ordering and once-per-frame guards remain authoritative. The optional freeze-on-cull gate is disabled only during those calls and restored afterward; explicit controller freezes and inactive skeleton policy are preserved.

Control: presence of `OPENMW_V4_REJECT_PARTICLE_ACTORS` restores the old actor rejection for same-executable diagnosis. This is a diagnostic control, not a playable fallback.

### Exterior loading

The user's settings enable V3.9 mode 2 multi-view frontloading and fresh initial object paging. Vulkan now bypasses that additional legacy future-view warming, since its terrain producer builds from LAND data independently. The current-grid legacy preparation remains intact for canonical paging/reference accounting. No changes were made to cell size, content lists, physics, navigation, Lua threading, or saves.

Control: presence of `OPENMW_V4_LEGACY_TERRAIN_FRONTLOAD` restores the previous expanded preparation. The null/default lifecycle and OpenGL path retain their existing behavior. The diagnostic runner already records inherited `OPENMW_*` controls.

Added timings distinguish `terrain_preload`, `cell_insert_objects`, `cell_render_physics`, `cell_navigation`, and `cell_render_add`. The existing coarse timings could not identify the central-cell bottleneck precisely; the new breakdown is paired with the substantive frontload change rather than being a telemetry-only repair. No particular load-time reduction is asserted before testing.

### Rigid left armor attachment

Rigid part composition now applies canonical translation-only `BoneOffset` and left-side X reflection. The inherited clockwise front-face state matches the reflection, so the reflected part is not culled inside-out. Skinned CopyRig composition remains separate and unchanged. Four native parity fixtures cover left/right with/without offset, including an offset containing rotation that must not be applied. Light attitude remains outside this repair.

## Validation

- 13/13 selected CTest cases passed, including semantic/control, publication/control, deformation, actor-plan, effect capture, update-only playback, attachment parity, diagnostics, and shader-package checks.
- 7/7 CP4 source-contract scripts passed. Two pre-existing whitespace-sensitive assertions were made formatting-insensitive after indenting the new actor branch; their semantic conditions were retained.
- 13/13 Python gameplay-diagnostic tests passed.
- Shader package verified: 82 files, manifest SHA256 `bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411`; pinned overlay SHA256 `6f42a686e2a6a9038bbd4a9e2d0d1be6d8812b9b3e681d8b569560c2fe110255`.
- `git diff --check` passed.
- The real winning loose kwama model was identified from the configured data-root order, not assumed to be the earlier MOP copy:
  `C:/OpenMWMods/i-heart-vanilla-directors-cut/TexturePacks/VFCR - Meshes-55093-1-0-1723146646/meshes/r/xkwama forager.nif`.
- Model SHA256: `da6168ceb73c4c744a1f1e0658706b1d01252c1e9e740b41b786dd1b6c0aec64`.
- Integration test parsed that actual model, verified the production particle requirement selector, loaded it through NifOsg, assigned canonical animation time, simulated its full 30-second controller timeline (902 frames), and captured 902 body draws plus 771 particle draws. Passed without emitter-resolution warnings. This is CPU playback/capture evidence, not full gameplay or GPU-image validation. The test mounts the winning mesh directory and base Morrowind archive, not the entire mod texture stack.
- Tests initially caught an invalid synthetic mesh fixture, a particle policy default assumption, the missing cull-phase simulation, and an integration timeline too short to reach emission. Those failures were corrected before this passing result. A compiler frame-stamp constness error was also corrected before the final successful build.

Local build evidence, under the build directory above:

- `particle-repair-build-final.log`
- `particle-repair-ctest.log`
- `particle-actor-integration.log`

Real-asset test invocation (with the normal dependency DLL/OSG plugin search paths):

```text
openmw-v4-effect-capture-tests.exe "C:/OpenMWMods/i-heart-vanilla-directors-cut/TexturePacks/VFCR - Meshes-55093-1-0-1723146646" "C:/Games/Steam/steamapps/common/Morrowind/Data Files/Morrowind.bsa" "meshes/r/xkwama forager.nif"
```

## Preservation and open work

Settings, input bindings, shader selection, global storage and player storage hashes match the pre-run originals recorded in run 62516. The protected save `_zZz__Wake_up.omwsave` still has SHA256 `d34685c838bd1b4318db34b1e0c1f00a7a2fe9a990fd21a120973297083af99b`. No save-format or content-list changes were made.

The installed Rafael overlay sets `SPECULAR_MAP_INTERPRETATION 2` (metallicity/roughness/AO channel data), whereas `legacymaterialshader.cpp` reads specular RGB as colored highlights and alpha as shininess. That is a concrete compatibility mismatch relevant to the colored wood/sack artifacts. It has not been disguised with gamma adjustments, disabled texture maps, or a claim that arbitrary OpenGL postprocessing now works in Vulkan. PBR/shader interpretation remains open.

Next runtime validation must use the rebuilt executable and diagnostic runner, verify the guard's left armor visually, enter Seyda Neen, confirm real particle playback and stable exterior gameplay, and examine the new load-phase timings. Remaining frame-time costs and any additional unsupported actor/effect types remain open until that run is analyzed. Broad CP6 optimization is not part of this patch.

## Exact source/test files changed in this repair

- `apps/openmw/mwrender/v4effectcapture.hpp`
- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `apps/openmw/mwrender/v4scenerenderlifecycle.cpp`
- `apps/openmw/mwrender/v4scenerenderlifecycle.hpp`
- `apps/openmw/mwrender/v4updateonlyviewer.hpp`
- `apps/openmw/mwworld/scene.cpp`
- `apps/openmw/mwworld/scenerenderlifecycle.hpp`
- `components/nifrender/actormodelcomposer.hpp`
- `components/render/backend/vsg/staticassetplan.hpp`
- `components/rendercore/records.hpp`
- `components/sceneutil/particleplayback.hpp` (new)
- `tools/v4/cp4/actor-skin-space-tests.cpp`
- `tools/v4/cp4/effect-capture-tests.cpp`
- `tools/v4/cp4/update-only-tests.cpp`
- `tools/v4/cp4/runtime-blocker-contract.py`
- This repair record.

The pre-existing `updateonlyvisitor.hpp` was inspected and temporarily modified during testing, then restored to its original marker-only implementation. All unrelated existing WIP remains owned by the user.
