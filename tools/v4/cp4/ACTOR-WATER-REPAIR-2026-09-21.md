# Actor planning and water repair checkpoint — 2026-09-21

## State and scope

Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Existing dirty work was retained.
No commit, push, CI run, archive mutation, gameplay launch, or save modification.
Read the Shared Project Context Archive control block and relevant history
before editing. Preserve separate same-executable controls and distinguish
source/build tests from user runtime acceptance. Previous population residency,
water uniform, shadow-slot, view-lifetime, texture sharing, and LAND fixes remain.

## Implemented

- Cache immutable actor draw plans by instance generation and resource revisions.
  Include hidden-node mesh/material/texture dependencies. Rebuild on changed
  model, skeleton, instance placement, material, texture, mesh, or options; prune
  removed actors and reset on world epoch changes. Poses/deformations still
  evaluate each frame. Invalid plans cannot be reused. No mesh payload copies,
  generic frame deferral, resource eviction, or renderer quality cuts added.
- Correct the horizontal projection mismatch introduced by reconstructing a
  right-handed reflection camera. Reverse the reflection/refraction retained
  half-spaces underwater, including interior/cave water.
- Stop alpha-compositing an already-composed refraction target over the main
  scene a second time.
- Decode `textures/omw/water_nm.png` through the winning OpenMW VFS source in
  data/normal color space. Use the native six-sample wave scales/weights and
  dielectric Fresnel calculation, world-anchored sampling, and normal-driven
  reflection/refraction distortion. Missing/failed texture decode explicitly
  warns and preserves the procedural fallback, not a warning image as normals.
- Separate diagnostic CPU elapsed time for `actor_plan`, `submit_task`, and
  `submit_present_queue`. These are NOT GPU execution times. The normal VSG
  task submission implementation and synchronization remain unchanged.

Controls (presence enables each; leave unset for repaired behavior):

- `OPENMW_V4_REBUILD_ACTOR_PLANS_CONTROL`: rebuild planning each frame.
- `OPENMW_V4_LEGACY_WATER_COMPOSITION_CONTROL`: old reflection UV and alpha blend.
- `OPENMW_V4_PROCEDURAL_WATER_CONTROL`: omit native normal-map loading.

## Exact files changed in this checkpoint

1. `apps/openmw/CMakeLists.txt` — register water-camera recovery test.
2. `apps/openmw/mwrender/v4enginerenderbridge.cpp` — VFS water normal-map decode.
3. `components/render/backend/vsg/dynamicactorplan.hpp` — revision-safe plan cache.
4. `components/render/backend/vsg/vsgruntimehost.hpp` — cache and water map option.
5. `components/render/backend/vsg/vsgruntimehost.cpp` — cache use, water map handoff.
6. `components/render/backend/vsg/vsgsubmission.hpp` — task/present timing only.
7. `components/render/backend/vsg/watersurface.hpp` — normal-map input/state.
8. `components/render/backend/vsg/watersurface.cpp` — projection/composition/waves.
9. `components/rendercore/frameproducer.hpp` — underwater water-view clip sides.
10. `tools/v4/cp3d/dynamic-actor-plan-smoke.cpp` — cache safety/control assertions.
11. `tools/v4/cp4/water-auxiliary-smoke.cpp` — camera and cave clip assertions.
12. `tools/v4/cp4/water-pixel-tests.hpp` — production-pipeline rendered checks.
13. This checkpoint record.

The repository diff also contains earlier work; it is not all attributable to
this checkpoint.

## Validation

MSVC2022 RelWithDebInfo production executable and explicit recovery targets
built successfully. Executable:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`

SHA256 `9C7AAF312BE33701A9E59B5C3FAD544B9A23CA787C68E734616CFEC46B847363`.

- 15/15 registered CTest tests passed (2.20 seconds).
- Actor tests: 100 stable frames reuse; forced control rebuild; movement,
  resource/options/removal/epoch invalidation; hidden texture/material changes;
  invalid models rejected and not retained.
- Offscreen production shader GPU tests passed with Khronos validation loaded:
  horizon/UV/depth, water disable/height/underwater uniform updates, cave coverage,
  reflection horizontal alignment, no double composition, normal-map descriptor,
  angle-dependent Fresnel, GUI ordering, LAND layers/depth/retirement, and view
  registration lifetime. No `VUID` or validation error reported in that log.
- Real packaged water normal map decoded at 128x128; 4,924 tested pixels changed
  between simulation times 0 and 5, confirming animated texture-driven output.
- Same-executable legacy composition control deliberately fails the horizontal
  reflection assertion. This is an expected regression-detection result.
- 16/16 gameplay-diagnostics Python tests and seven source contracts passed.
- `git diff --check` passed; Git emitted existing LF/CRLF conversion notices.

Build/test logs under `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/`:

- `actor-water-repair-build.log`, `actor-water-repair-final-build.log`
- `actor-water-suite-build.log`, `actor-water-pixel-build.log`
- `actor-water-ctest.log`, `actor-water-pixels.log`
- `actor-water-legacy-control.log`

Initial compilation exposed an ambiguous VSG vector initializer, corrected with
explicit vector types. Initial phase-level submission instrumentation failed
Windows linkage because `RecordedCommandBuffers` is not exported. Removed that
instrumentation; retained `task->submit()` unchanged, timing its whole call.
The camera test initially used a 0.001 world-unit absolute threshold; observed
translated-float lookAt rounding (~0.0011 at 330 units) justified a small relative
tolerance. The X/Y mapping and handedness assertions remain intact.
Existing compiler warnings remain (settings float truncation, engine conversions,
and test NDEBUG override). Do not describe this as warning-free or an ALL-target
build: the previously recorded missing googletest source remains outside scope.

## Not yet validated / remaining rendering work

No gameplay FPS or memory improvement claimed. Plan reuse removes known repeated
work but cannot by itself establish that the remaining submission, capture,
dynamic realization, and memory-pressure stalls are solved. Prior strict-QC
scene timings are not a clean comparative benchmark.

This is NOT complete native water parity: depth-dependent absorption, shoreline
distortion rejection, native collision/rain ripples, and sun/shadow response still
need implementation/validation. Native sky clouds/celestial rendering remains
incomplete. Cyan/green wood and material artifacts are NOT claimed fixed. They
need an asset-specific comparison against winning VFS textures and native
material semantics, not arbitrary tint/gamma changes.

Next gameplay acceptance uses isolated config AND user data, 1920x1080 windowed
mode 2, no border, no minimize-on-focus-loss, and New Game -> ship -> hatch ->
Seyda Neen. Do not load regular saves or substitute a direct-cell diagnostic for
that acceptance route. Runtime promotion requires user testing and analysis.

## 2026-09-22 isolated new-game runtime result

Evidence directory:
`C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260922-000035-actor-water-newgame`

The user tested the exact executable identified above through the required
1920x1080 isolated New Game route. This result is a checkpoint observation, not
runtime acceptance:

- Ship-interior performance was visibly better and reached about 27 FPS in the
  supplied capture. This is encouraging but is not a controlled benchmark.
- Seyda Neen remained unplayable at about 3-4 FPS while process RAM reached
  roughly 31 GB. Exterior transition timing recorded 44.494 seconds, including
  a 37.263-second terrain preload/wait.
- Water now had visible animated surface detail, but it remained predominantly
  white and visually incorrect. Flat pale sky and cyan/green material artifacts
  also remained.
- The character-creation appearance preview was blank.
- Entering the Excise Office terminated with:
  `evaluated non-actor object ref:0xa8000418: evaluated effect textured geometry has no matching UV stream`
- Diagnostic samples showed actor-plan work at 0.047 ms median and 0.249 ms
  maximum, supporting that the actor-plan reuse mechanism was active. This did
  not translate into acceptable whole-frame performance.
- Effect churn remained: 26 sampled frames rebuilt effects while reusing plans,
  with a median of 3 rebuilt effects per sampled frame.

Runtime status: **rejected for promotion**. Preserve this exact source/build as
a diagnostic checkpoint. The next repair must address the missing-UV effect
path and blank preview without regressing actor-plan reuse, then continue the
exterior memory, terrain preload, water, sky, and material investigations.
