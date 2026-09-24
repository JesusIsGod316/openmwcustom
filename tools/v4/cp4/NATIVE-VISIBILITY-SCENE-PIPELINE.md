# Native visibility and scene pipeline candidate

Local, uncommitted work on `codex/cp4f-material-frame-repair`, based on
`efa4f3694fad904b3546e7c846c756403950f0d2`. This includes earlier uncommitted
repairs; the package manifest fingerprints the full dirty source set.
Build/test success is not gameplay performance promotion or full mod acceptance.

## Latest user test and findings

Input: `C:/OpenMW-CPU-Test/Test-Results/20260923-182306-gameplay-131492.zip`.
Tested executable SHA256:
`3683fcc1af38e8ae8fc65baa6c506b7a9c8128324a80f1be9b823581e0cfa1a0`.
The capture ended normally with exit 0 and unchanged original configuration
hashes. The user reports that directly aiming at NPCs now works. This is positive
interaction evidence, not exhaustive NPC/mod/save validation. No old-build rerun
is required or requested.

All 185 sampled effect handoffs reported zero publication workers despite the
parallel flag. The real gameplay/local-map entry point duplicated a serial
handoff instead of calling the optimized ordinary-main-frame route. Both now
call the same publication helper. Up to three dedicated workers plus the caller
copy/validate immutable CPU draw snapshots; small batches stay inline. No live
OSG, Lua or world mutation is moved to those workers.

Eleven sparse exterior samples per stage, after excluding transition frames and
the first ten seconds after transitions, have medians of 62.72 ms for evaluated
geometry capture, 35.42 ms for dynamic realization, 18.50 ms for snapshot
publication and 27.72 ms for submit-task work. These are inclusive diagnostic
envelopes: do not sum them, infer exclusive costs, calculate FPS, or call them a
controlled before/after benchmark. Static visibility will not eliminate all
evaluated-geometry capture work. The new worker routing needs a new runtime
capture before claiming a gain.

## Implemented mechanisms and controls

All new paths are off when their environment variables are unset.

| Control | Mechanism | Limits |
| --- | --- | --- |
| `OPENMW_V4_STATIC_FRUSTUM=1` | Conservative bounds reject native static instances/population groups outside the current main camera. | Unsafe, deformed or unknown bounds remain visible. |
| `OPENMW_V4_TERRAIN_OCCLUSION=1` | Enables frustum rejection and bounded, same-frame masked software occlusion using exact active opaque LAND geometry. | Main view only; 384x216 raster, at most 32,768 triangles examined; no building occluders, old-camera history or asynchronous stale results. |
| `OPENMW_V4_POSTPROCESS=copy` | Persistent linear RGBA16F scene color plus sampled D32 depth, then an identity fullscreen pass and GUI afterward. | Experimental foundation, not an optimization by itself; no bloom/clouds or configured `.omwfx` execution. |
| `OPENMW_V4_POSTPROCESS=edge-aa` | Optional small edge-adaptive filter on that scene target. | Not SMAA, FXAA, or a replacement for Rafael's chain. |
| `OPENMW_V4_POSTPROCESS=depth` | Raw reversed-depth diagnostic display with GUI afterward. | Diagnostic only. |

For the two visibility flags, presence enables them: remove the variable to
disable it; setting it to `0` does not disable it. Invalid postprocess mode names
are rejected. Existing CPU switches remain independent and default-off; all
package launchers select `--cpu-fastpaths all`.

Main-view rejection does not remove world objects or affect picking, physics,
scripts, reflection/refraction, shadow or map draw visibility. Near-plane,
camera-inside and unsupported cases fail open. Current-frame terrain depth is
cleared/rebuilt, with conservative projection/depth margins. No terrain is
queried against itself. Candidate lists are retained between static mutations.

The Shared Project Context Archive influenced these boundaries: preserve the
coarse terrain/population foundation; do not revive rejected broad individual
building occluders or aggressive parallel MSOC budgets without new evidence.

Scene color/depth consumes approximately 23.7 MiB at 1920x1080, excluding the
existing swapchain/water resources. Resize briefly needs both old/new targets
and waits for idle only during replacement, not every ordinary frame. This
experimental scene target is single-sample; MSAA scene resolve is not integrated
yet. Full-game resize/GUI/cell-transition validation is still needed.

## Native bloom and clouds: architectural direction, not completed features

**Latest user direction:** defer custom native bloom/cloud implementations and
prioritize the existing F2 postprocessing interface and `.omwfx` compatibility
on Vulkan. The scene target remains supporting infrastructure, not a substitute
shader system. F2 compatibility is NOT implemented in this candidate.

The audited source currently disables `mUsePostProcessing` and sets
`mGLSLVersion=0` when the OSG viewer has no graphics context in
`apps/openmw/mwrender/postprocessor.cpp`. The F2 HUD then refuses to open through
`WindowManager::togglePostProcessorHud`. Merely re-enabling this boolean would
not make `PingPongCanvas` OpenGL passes execute on the native Vulkan route.

The saved user chain inspected this turn is:
`HBAO,VAIO,godrays,DIVE,wetworld,tonemap,SMAA,SMB,SSGI_Main_Exterior,ScreenSpaceShadows_Distant`.
Its settings were read only, not modified. This is more than the supplied Rafael
pack alone; preserve the active VFS winners instead of installing a different
archive over them.

Concrete implementation sequence for the remaining F2 work:

1. Reuse `Fx::Technique`, shader settings and the current F2 list/widgets as the
   single source of chain order, enablement and parameters. Separate the CPU-side
   controller from the GL-only `PingPongCanvas`; preserve OpenGL behavior.
2. Publish immutable, backend-neutral technique/pass/target/texture/uniform
   definitions at the main-thread boundary before the Lua worker runs. Version
   structural changes separately from per-frame weather/camera/uniform values.
3. Implement Vulkan shader compilation/bindings and the full `omw_*` API input
   contract, including depth/projection conventions, normals, HDR eye adaptation,
   point lights and distortion. Unsupported requirements need an explicit error,
   not plausible-looking fake inputs or a silent effect drop.
4. Implement ordered multipass execution with last-shader versus last-pass
   semantics, named/scaled targets, sampler formats/wrap/filtering, mip generation,
   clear/persistence and blend state. Reuse allocations; do not read scene color
   back through the CPU or bridge through an OpenGL rendering context.
5. Publish F2 toggles/reordering/parameter edits and reloads transactionally after
   successful compilation; retain the last valid chain with actionable errors.
   Preserve GUI-after-effects and safe resource lifetime across in-flight frames.
6. Validate real winning shader files and synthetic fixtures: exact chain order,
   live values, depth/world reconstruction, named targets, HDR, texture inputs,
   interior/underwater flags, resizing, camera cuts and cell transitions. Only
   then enable the F2 path and claim the tested capabilities. Full arbitrary-mod
   compatibility is broader than one working effect.

Engine-owned does not mean shader-free or pass-free. Bloom remains a GPU
post-render effect: reduced-resolution downsample/upsample levels, shared HDR
exposure and a combined bloom/tone-map composite can reduce redundant work.
Merely moving the same full-resolution shader into C++ changes little.

Engine-owned volumetric clouds can integrate with the sky/weather/sun system,
trace at reduced resolution, reuse stable history and skip interiors. They still
need shaders and usually multiple stages. Motion history, camera cuts, depth,
lighting and VRAM budgets must be designed explicitly. They add GPU work relative
to having no volumetric clouds; there is no promised FPS gain over Rafael's
implementation without matched-quality measurements.

The native scene plumbing here supports these future systems. It does not port
Rafael's effects or establish arbitrary `.omwfx` compatibility. Compatibility
must remain a separate, explicit path, with unsupported effects reported rather
than silently called equivalent. Remaining material, water and full mod/save
compatibility work is not closed by this batch.

## Package and next test

New separate directory: `C:/OpenMW-Native-Test`. The old CPU package and its
captures are untouched. No reference shader archives are installed by this work.

- `Start-Native-Visibility.cmd`: CPU fixes plus native frustum/terrain occlusion;
  scene processing off. This is the next practical test.
- `Start-Native-Scene-Pipeline.cmd`: same, plus the identity scene color/depth
  path. This is an optional plumbing test, not a bloom/cloud preview.
- `Start-Native-Control.cmd`: same new executable and CPU fixes with only the
  new visibility/scene paths disabled. Available for isolation if needed; no
  old executable rerun is requested.

Use New Game in the isolated profile. Normal saves are not copied. No admin or
profiler is required. Double-clicked launchers retain a visible command window
and pause at exit. Logs and the resulting ZIP go under
`C:/OpenMW-Native-Test/Test-Results/<timestamp>-gameplay-<pid>/` (ZIP beside it).
Effective switches, executable hash and original config checks are recorded.

For the visibility candidate, exercise the ship, Seyda Neen exterior, Office,
cell revisits, direct NPC interaction, looking around hills, near-wall views,
water reflections and shadows. Watch for disappearing geometry or new stalls.
The optional scene pipeline additionally needs inventory/map/preview, resizing
and return-to-game checks. A new capture will show actual worker counts,
visibility rejection counts and CPU stage times. No request to rerun the old
build is implied.

## Validation

- Windows x64 MSVC Release Vulkan production target: passed, exit 0.
- OpenGL-only Release production target: passed, exit 0.
- Rendering CTest: 10/10 passed.
- Vulkan pixel suite: all groups passed on RTX 5050 Laptop with Khronos
  validation enabled; no validation messages observed. New tests cover actual
  main/auxiliary GPU visibility routing, color transfer/orientation, repeated
  scene-target use, sampled depth and GUI-after-processing.
- CPU visibility tests cover holes, near plane, backfaces, stale terrain reset,
  disabled controls and conservative rejection.
- Python diagnostics: gameplay 26 passed; config 16 passed; runtime 17 passed
  with one unavailable-native-fixture skip.
- Staged executable version smoke passed; its matching PDB was verified by
  `dumpbin /PDBPATH:VERBOSE`. Three prepare-only launches verified independent
  visibility, copy and control settings without starting the game.
- One repeat CTest invocation initially omitted the runtime DLL search path and
  failed process loading (0xc0000135). With the staged DLL directory on PATH,
  all 10 tests passed; this was a test environment error, not an assertion failure.
- Existing compiler conversion warnings remain. Source diff check passed with
  line-ending normalization warnings.
- Gameplay performance, new-path runtime acceptance and complete mod/save/postfx
  compatibility: NOT established. No commit or push in this batch.

## Exact files changed in this batch

Other dirty files predate this batch and are preserved.

- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `apps/openmw/mwrender/v4enginerenderbridge.hpp`
- `apps/openmw/mwrender/v4localmapbridge.cpp`
- `components/render/backend/vsg/nativevisibility.hpp` (new)
- `components/render/backend/vsg/nativepostprocess.cpp` (new)
- `components/render/backend/vsg/nativepostprocess.hpp` (new)
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `components/render/backend/vsg/runtime-sources.cmake`
- `tools/v4/cp4/native-visibility-tests.cpp` (new)
- `tools/v4/cp4/rendering-pixel-tests.cpp`
- `tools/v4/cp4/rendering-tests/CMakeLists.txt`
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- `tools/v4/cp4/Start-Native-Visibility.cmd` (new)
- `tools/v4/cp4/Start-Native-Scene-Pipeline.cmd` (new)
- `tools/v4/cp4/Start-Native-Control.cmd` (new)
- this note (new).
