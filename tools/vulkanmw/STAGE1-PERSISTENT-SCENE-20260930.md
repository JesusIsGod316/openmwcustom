# VulkanMW Stage 1 persistent scene checkpoint

This checkpoint adds retained GPU population rows and committed chunk discovery to
VulkanMW. It adapts RTX stable-placement and upload-debt ownership ideas, alongside
Clockwork style packed GPU inputs, to the existing RenderWorld and VSG backend.
It does not import another renderer or duplicate the authoritative gameplay scene.

The source base is `3993b098613b131ba431d7b2f57320a2c8a2ce60`, on
`codex/vulkanmw-stage1-persistent-scene`. The full Release executable compiled and
linked locally and is installed in `C:\VulkanMW`. These changes remain uncommitted
and have not been pushed. Modded gameplay acceptance and performance promotion
are still pending. No new FPS or frame-time improvement is claimed.

## Retained population updates

Eligible compacted populations bind each unique source reference to a stable GPU
row when the resident is built. Payload reordering does not reassign rows.
Transform, bounds and LOD changes prepare only affected row uploads. Publication
waits until residency accepts the world revision and generation.

The ordinary placement-only path reuses its compiled draws, descriptors, buffers,
traversal root and inventories. It does not realize the mesh again or compile a
replacement graph. Cached model bounds supply updated conservative group bounds;
assets previously classified as fail-open remain fail-open.

Small uploads have a 16 KiB aggregate budget per population. Each changed row uses
96 bytes of packed culling data and 40 bytes for the original direct-view transform
streams. Bulk changes retain the existing staged rebuild path. Initial upload
arrays are released after staging has copied them.

## Discovery from committed changes

GpuSceneTables retains a separate population consumer queue containing complete
chunk handles, including their generations. Repeated updates to a chunk coalesce,
but an unloaded slot and a reloaded generation remain distinct. Skipped GUI frames,
failed preparations and another consumer clearing dirty ranges cannot discard
this queue. It is acknowledged only after successful residency publication.

With asset identity and options unchanged, population discovery visits only those
chunks. Existing asset bindings are reused for placement-only plans. The static
instance stamp pass no longer rediscovers population model dependencies in this
mode. Asset, settings, source-world or epoch changes retain complete discovery.

## GPU ordering and resource lifetime

The upload prelude has submit order `-10000`, before VSG shadow pre-renders and
parallel water views. Main-camera visibility still writes its own compact output;
secondary views use the updated original transform streams rather than borrowing
main-camera visibility.

Uploads embed their small data in command recording with `vkCmdUpdateBuffer`.
Multiple publications before submission merge their row debt. Debt is cleared
only after successful submission. Transfer barriers order prior reads, uploads,
compute reads and direct vertex reads. Graph and buffer replacements retain the
existing GPU-completion fence retirement; no CPU frame-number guess or new
per-frame device-idle wait is introduced. This use is bounded by the
[Vulkan small update contract](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdUpdateBuffer.html).

Stricter validation also exposed an existing offscreen-target depth clear hazard,
including on the direct control. Its external dependencies now include early and
late depth writes for every depth attachment, not only sampled depth attachments.
The GPU tests fail on validation messages and enable
[Khronos synchronization validation](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/syncval_usage.md).

## Compatibility and control path

`OPENMW_VK_PERSISTENT_POPULATIONS` is independently switchable and default-off.
It requires the existing compacted GPU-cull stack. The existing producer harness
accepts `--persistent-populations`; both arms retain P3B and differ only in this
new switch. No new stage-specific BAT is added. Normal settings and saves are not
rewritten by installation or the launchers. Ordinary gameplay still uses the
normal user-data directory and may save there. No OpenGL rendering optimization
is changed by this checkpoint.

Identity or cardinality changes, ambiguous bindings, traversal-mask changes,
changed assets, unsupported ordered populations, non-finite data and large updates
retain the rebuild or established CPU/direct fallback. Mods do not need to issue
new notifications: the backend consumes the engine's committed RenderWorld
publications. This is a retained population implementation, not a claim that all
actors, particles, controllers or OSG compatibility capture have been removed.

## Local validation

- MSVC Release native tests: 29 of 29 passed, including the strict warning-as-error
  new binding and event fixtures.
- Full production OpenMW Release compile and link passed using MSVC 19.44,
  Ninja, six compiler jobs and no LTO. Existing external OSG, conversion and
  dependency warnings remain.
- The production-linked OSG capture regression passed 51 of 51 tests. The real-NIF
  conformance tool passed all 136 hair NIFs in the winning Character Edits package
  after the import repair below. This is not exhaustive modded asset validation.
- Actual Vulkan execution on the RTX 5050 Laptop GPU: all four inventory variants
  passed exact main and secondary pixel comparisons with synchronization validation.
  Tests cover reordering, movement, changing LOD inputs, a secondary pre-render,
  repeated publications, queued frames and bounded bulk fallback.
- The event fixture uses 128 chunks and 256 groups. A one-chunk movement visits
  one chunk, creates one placement delta plan without rebuilding its asset, and
  preserves the untouched plans' placement and draw storage. It also tests stale
  transactions, ordering, unload/reload generations and GPU-fenced retirement.

The broad architectural guard has an inherited OSG dependency violation in
`fximagecapture.hpp`; that file is unchanged from the source base. The targeted
P2 and P3 guards pass. Post-processing compatibility repair is outside this slice.

## NPC hair import repair

The initial installed build aborted both a normal save load and the first P3B
control run. The native model cache returned InvalidBundle without a translation
error. The winning Character Edits assets `b_n_dark elf_m_hair_01.nif` and
`b_n_dark elf_m_hair_22.nif` contain 22 and 12 non-finite normals, respectively.
NifOsg copies these values without rejecting the model, whereas the neutral mesh
contract requires finite attributes. The control-arm failure was independent of
the new retained-population switch, not evidence of a corrupted save.

Native import now reconstructs only non-finite normals from area-weighted mesh
faces. Finite normals, positions, indices, UVs, materials and source files remain
unchanged. Degenerate or isolated vertices use the existing missing-normal
direction. The strict payload validator remains active; invalid positions,
indices, primitive ranges or attribute sizes are not silently accepted. Repair
is performed at import, not in the recurring frame loop, and emits a per-mesh
translation warning. No hair geometry is skipped and Stage 1 remains enabled.

The new native fixture passes 17 checks for finite-normal preservation, NaN/Inf
repair, winding, triangle strips, degeneracy, invalid geometry, absent normals
and large finite coordinates. Both reported assets now publish and realize with
their actual textures. All 136 hair assets in the selected package pass the
headless source-to-VSG check; 30 assets needed normal repair. The full engine
rebuilt and linked, 51 capture tests passed, and all four GPU synchronization
validation variants passed again.

The installed public automated harness completed all four runs and exited with
code zero. Original configuration hashes matched after each run. Evidence is in
`C:\VulkanMW\Benchmarks\producer-20260930-130447-040689.zip`. Run mean frame times
were 60.798695, 60.590349, 60.192383 and 61.284867 ms in control, candidate,
candidate and control order. These short runs establish loading and exit success;
they do not establish a substantial performance improvement or promote Stage 1.

The exact reported Gallus Darkwind `[zZz] Wake up` save also loaded and ran for
20 seconds before a normal scripted exit. This used a copied save, normal settings
copied into private configuration, and only the Vulkan renderer/fallback override.
The normal save, settings, content configuration, input, shader settings and Lua
storage hashes were unchanged. Its evidence is
`C:\VulkanMW\Benchmarks\repair-save-20260930-131201\repair-qa.json`.
This is a save-load regression check, not exhaustive gameplay or visual acceptance.

## Remaining integration

The next performance gate is movement and other runtime changes with nonzero
retained-update telemetry. Representative modded gameplay and matched benchmarks
must follow before enabling the engine switch by default. Further
donor integration can then address shared GPU allocation and batching, bulk delta
uploads, view-specific visibility, and other producer families without confusing
them with the separate OptimizedMW OpenGL work.

## Local installation and launchers

The installed executable SHA256 is
`0e523aae039c2f21d1726eae77838a14e08ce56b8b2938c012eb3dc71bcdfd13`.
`C:\VulkanMW\source-changes.json` records the base commit, dirty worktree status,
changed implementation file hashes, dependencies and validation scope. The
executable's printed revision is the base commit, not a new committed checkpoint.

Only two public launchers remain at the installation root:

- `START-VulkanMW-Test.bat` defaults to same-executable A/B/B/A of Stage 1 against
  P3B. Both arms use the established isolated test settings and differ only in the
  new retained-population switch. Explicit comparison options remain available.
- `START-VulkanMW-Benchmark.bat` enables Stage 1 and uses the normal configuration,
  mods, settings and saves, with only a temporary Vulkan/no-fallback renderer
  overlay. Quit normally to finish logs under `C:\VulkanMW\Benchmarks\manual-*`.
  `--p3b-control` disables only Stage 1. `--frame-profile` adds detailed timing
  explicitly; it is not enabled in the ordinary manual session.

Installed imports, executable version, both launcher interfaces and normal-profile
dry-run checks passed. The installer itself did not launch a game; subsequent
crash-repair QA exercised both public test modes and the copied save as described
above. The
packaging rules also stop installing legacy V3 OpenGL BATs into Vulkan packages;
OpenGL-only packaging keeps those entrypoints unchanged.

The previous engine files, resources, DLLs and debug symbols were removed after
staging validation. Normal configuration and the existing benchmark ZIP passed
before/after hash checks. Small prior root records were preserved under
`C:\VulkanMW\Install-Records\Previous-20260930-122529`. The old binaries and PDB
are not retained as a rollback build. Cached dependencies were reused; no new
download was needed. Source/build work remains in the managed Stage 1 worktree.

## Changed files

All paths are relative to this checkout. The generated local build directories
are ignored and are not part of the source change.

- `components/render/backend/vsg/populationbindings.hpp`
- `components/nifrender/geometrysanitization.hpp`
- `components/nifrender/niftranslator.cpp`
- `components/render/backend/vsg/gpupopulationcull.hpp`
- `components/render/backend/vsg/gpupopulationcull.cpp`
- `components/render/backend/vsg/gpuscenetables.hpp`
- `components/render/backend/vsg/staticpopulationresidency.hpp`
- `components/render/backend/vsg/staticworldsyncstate.hpp`
- `components/render/backend/vsg/staticworldplan.hpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/offscreenrendertarget.cpp`
- `tools/v4/cp3b3/gpu-population-routing-smoke.cpp`
- `tools/vulkanmw/run-producer-queues.py`
- `tools/vulkanmw/run-publication-cohort.py`
- `tools/vulkanmw/tests/CMakeLists.txt`
- `tools/vulkanmw/tests/p3-gpu/CMakeLists.txt`
- `tools/vulkanmw/tests/population-bindings-tests.cpp`
- `tools/vulkanmw/tests/geometry-sanitization-tests.cpp`
- `tools/vulkanmw/tests/population-event-tests.cpp`
- `tools/vulkanmw/tests/test-producer-launcher.py`
- `tools/vulkanmw/tests/test-normal-benchmark.py`
- `tools/vulkanmw/vulkan-play.py`
- `tools/vulkanmw/START-VulkanMW-Test.bat`
- `tools/vulkanmw/START-VulkanMW-Benchmark.bat`
- `apps/openmw/CMakeLists.txt`
- `CMakeLists.txt`
- `.github/workflows/vulkanmw-producer-queues.yml`
- `tools/vulkanmw/STAGE1-PERSISTENT-SCENE-20260930.md`
