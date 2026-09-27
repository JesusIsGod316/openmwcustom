# Native resource update checkpoint — 2026-09-26

## Scope and source state

This starts the performance-led follow-up to Phase 3C. It is not completion of
the native renderer, GPU skinning, native particles, or Rafael/.omwfx support.
The three new mechanisms are independently switchable and default off. Normal
OpenGL behavior, content interpretation and save serialization are unchanged.

Base commit: `336a4cf8668afa1a4f03069785421e9a5e2b74ef` on
`vulkanmw/phase3c-native-runtime-substitution` (existing PR #9). The changes in
this checkpoint are local and uncommitted; the version string still reports
the base commit. Do not confuse that string with the source of a clean build.

Candidate package: `C:/VulkanMW-Resource-Test`. The prior control packages
`C:/VulkanMW-Phase3C-Test` and `C:/VulkanMW` remain untouched.

Candidate executable SHA256:
`bc3a0a25daafbb02b99b869aedbef8e6dc6b572270c06b1da908eb6763c1f49c`.
`source-changes.json` in the package records the base and SHA256 of every
modified/new implementation and test file. Each benchmark manifest records
that manifest's hash, binary hash, effective controls, shader/driver/script
hashes and original configuration-chain hashes.

## Mechanisms

### Placement-only population changes

`OPENMW_VK_POPULATION_DELTAS=1`:

- Retains the existing placement-free asset mechanism.
- Matches each placement by stable reference identity plus the complete
  placement contract, not its index in a vector. Empty/ambiguous identities,
  changed assets, dependencies, epoch, bounds, lighting, LOD or semantic flags
  rebuild conservatively.
- Reuses immutable placement nodes and their vertex-derived bounds. In the
  synthetic regression fixture, a group with 191 placements and one moving
  reference rebuilds one placement instead
  of reconstructing 191 nodes and walking all their vertices.
- Still reconstructs the group child list and aggregates placement bounds;
  this is not an O(1) group update or a new producer.
- Avoids both global SharedObjects prune scans on placement-only mutations;
  resource-changing mutations still prune. Existing GPU-safe resident
  retirement remains in control. No indefinite cache or new retention budget.

### Actor deformation streams

`OPENMW_VK_ACTOR_STREAMS=1`:

- Preserves the existing CPU deformation and parallel preparation paths.
- Warm updates retain only deformed positions/normals/tangents/bitangents;
  unchanged indices, UVs, colors and surface arrays are no longer copied into
  every prepared actor payload each frame.
- Cold realization lazily reconstructs the full authored payload per node.
  Sparse containers never enter the full-mesh realization path.
- Does not remove OSG animation, provide GPU skinning, or claim full native
  actor coverage.

### Depth-writing terrain occluders

`OPENMW_VK_LAND_DEPTH_OCCLUDERS=1`:

- Fixes the blanket alpha-blend exclusion that rejected native LAND base
  layers. Their source-alpha/zero color-weight equation does not punch holes
  in the depth-writing base surface.
- Requires the exact native terrain-base equation and ordinary full-depth
  state. Cutouts, overlays, alpha fades, depth-disabled/write-disabled,
  stencil, decals, wireframe, refraction and displaced/effect surfaces remain
  excluded.
- Uses existing bounded current-camera software occlusion and fail-open
  bounds. It does not reuse main-camera results for other views or stop
  invisible gameplay/animation updates.

Primary reference: Vulkan's [depth attachment writes specification](https://docs.vulkan.org/spec/latest/chapters/fragops.html)
separates depth-write eligibility from color blending. Eligibility is still
verified against this fork's material producer and shader, not assumed for
arbitrary translucent mod assets.

## Validation

- Full local `openmw` x64 optimized RelWithDebInfo build passed using MSVC
  19.44.35228. Log: `build/phase3c-engine/resource-delta-build.log`.
- Focused Release CTest: 6/6 passed, including 220 population/material checks,
  serial/parallel actor stream parity, cold payload attributes and both real
  LAND occlusion/control routes.
- The visibility suite tests an actual published LAND material, then verifies
  that an object behind it is culled only in the enabled arm. Cutout LAND
  leaves the object visible. Existing holes, winding, near-plane crossings,
  removed terrain and auxiliary-view checks remain covered.
- Six architecture/phase source contract checks passed during implementation.
- Runtime results are recorded below after the same-executable comparison.
  Compilation and unit tests do not establish performance or complete mod
  compatibility.

## Runtime comparison

**Not a performance promotion.** Four sequential runs used the same executable,
mod/config chain, shader package, script, driver and source-change manifest.
Vulkan retained path, 1920x1080, uncapped/VSync off, Seyda Neen, seed 123456,
15-second warmup and 30-second wall-clock sample. No compiler or another game
was running during sampling. MSBuild's two idle reusable worker processes
remained idle (their accumulated CPU time did not increase).

| Run order | Controls | Mean ms | Median ms | p95 ms | Frames |
| --- | --- | ---: | ---: | ---: | ---: |
| Control 1 | Three new flags off | 67.178781 | 66.656500 | 74.082000 | 446 |
| Changes 1 | Three new flags on | 67.455327 | 67.123700 | 73.825900 | 444 |
| Changes 2 | Three new flags on | 68.989011 | 68.595500 | 76.749600 | 435 |
| Control 2 | Three new flags off | 66.983925 | 65.921200 | 77.616100 | 447 |

Neither enabled run improves mean frame time. These are Vulkan/Vulkan controls,
not a new OpenGL comparison. Keep all three flags default-off. No request for
another user acceptance test is warranted on the basis of this result.

Evidence beneath `C:/VulkanMW-Resource-Test/Benchmarks`:

- `control-1/20260926-183841-gameplay-160776`
- `changes-1/20260926-184121-gameplay-160704`
- `changes-2/20260926-184325-gameplay-85128`
- `control-2/20260926-184546-gameplay-152932`

Source-change manifest SHA256:
`bcde0fdd1e4ad2a8a811afe47c4f37d236747d3a709758bcd97f1006f8b5c1fb`.
Driver SHA256:
`0f6761cfbd58e72ba9a9e0389c3a21ed8425590f600f34c32f4ac796332d1285`.
Scene-script SHA256:
`9f27b87c9011bf94c5047936bf8044a4f2e2a0838649a85edb463318b8085ca9`.
Shader-manifest SHA256:
`bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411`.

All four runs exited zero; every original configuration-chain hash remained
unchanged. Each run contained the same 20 error-level console lines after
timestamp normalization (including mod-script/configuration failures). Normal
exit is not full mod/save compatibility acceptance. No ordinary saves were
copied, loaded or overwritten. No new Vulkan visual-parity claim is made from
these timing captures.

### What the runtime actually established

Sparse CPU-stage medians are inclusive, not GPU times or additive totals:

| Stage/counter | Control 1 | Changes 1 | Changes 2 | Control 2 |
| --- | ---: | ---: | ---: | ---: |
| Static synchronization, ms | 4.2835 | 4.5522 | 4.6142 | 4.2581 |
| Actor preparation per sampled batch, ms | 0.4577 | 0.4118 | 0.4220 | 0.4703 |
| Dynamic capture, ms | 11.2660 | 11.2867 | 11.3069 | 11.6509 |
| Dynamic realization, ms | 10.3273 | 10.3567 | 10.8912 | 11.0214 |
| Visibility, ms | 0.2018 | 1.0519 | 1.0709 | 0.2071 |
| Command recording, ms | 12.7410 | 12.4519 | 13.0218 | 12.6023 |
| Occluded static candidates | 0 | 16 | 16 | 0 |

The terrain repair is active: 8,711 triangles contributed depth from the bounded
32,768-triangle examination budget. It culls 16 additional candidates but costs
approximately 0.85 ms more CPU visibility time in this view. This is functional
coverage, not a demonstrated speedup; batching/budget/placement of culling still
needs work before enabling it by default.

Important correction to the early hypothesis: the repeatedly moving bottle's
changed group rebuilds **one** placement, not 191. The separate `placements=191`
counter was not that group's size. Runtime `nodes_built=1`, `nodes_reused=0`
confirms that the multi-placement reuse fixture is not a hotspot exercised by
this scene. Avoiding prune calls also did not reduce measured static sync time.
Do not claim the 190-node unit-test saving as an observed gameplay saving.

All runs sampled 48 actors and 2,707 objects. Native pose coverage varied from
13 to 15 with zero seed failures; effects rebuilt varied from 3 to 5 per sampled
frame. A fixed random seed with wall-time simulation does not make moving
effects and animation frame-identical, so do not infer an exact regression
percentage or sum the sparse medians. The overall lack of benefit is clear.

### Next substantial work

The retained-placement mechanism is now covered; further polish there is not
the primary path to acceptable FPS in this scene. Keep the native migration
focused on removing the roughly 11 ms capture path and the expensive dynamic
realization/deformation/recording paths. In particular, repeated effect-resource
birth/rebuilds still exist and the OSG update-only world still runs. Those have
not been fixed by this checkpoint. Separate CPU preparation, compilation,
upload/wait and recording costs before choosing the next native replacement;
do not assume an inclusive stage is entirely parallelizable.

F2/Rafael support and complete removal of OSG remain unfinished. Preserve narrow
compatibility fallbacks and existing OpenGL behavior while implementing native
producers, immutable per-frame inputs and GPU-safe resource reuse.

## Exact changed files

- `components/render/backend/vsg/populationplacementreuse.hpp` (new)
- `components/render/backend/vsg/landdepthoccluder.hpp` (new)
- `components/render/backend/vsg/dynamicactorpreparation.hpp`
- `components/render/backend/vsg/nativevisibility.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `tools/v4/cp4/actor-preparation-tests.cpp`
- `tools/v4/cp4/architecture-benchmark.py`
- `tools/v4/cp4/native-visibility-tests.cpp`
- `tools/vulkanmw/tests/CMakeLists.txt`
- `tools/vulkanmw/tests/population-delta-tests.cpp` (new)
- `tools/vulkanmw/tests/test-benchmark-control.py`
- This checkpoint report (new).

## Disk cleanup status

Read-only inventory identified 4,556,002,695 bytes (4.24 GiB) of disposable
`.obj`/`.pch` files in the old CP4F worktree's two compiler output directories:

- `C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/build/cp4f-engine-opengl`
  — 906 object files and 6 precompiled headers; 1,879,582,682 bytes.
- `C:/Users/LSCha/.codex/worktrees/cp4f-material-frame-repair/OpenMW custom Build/build/cp4f-engine-vulkan`
  — 942 object files and 8 precompiled headers; 2,676,420,013 bytes.

These exact generated file classes were checked as ignored, with no reparse
points or active process using those trees. Deletion was nevertheless rejected
by the execution policy before the command ran. **No files were deleted and no
space recovery is claimed.** Do not substitute recursive worktree deletion or
discard captures to bypass the restriction.

Preserve source/checkpoints, tests/logs, packages, executable/PDB/library files,
current incremental build outputs, verified dependencies, normal mods/configs
and saves. Initial free disk space was approximately 86.4 GiB; the final check
reported 99,341,656,064 bytes (92.5 GiB) free. The increase was not caused by a
successful cleanup action in this task and is not attributed to this work. The candidate
uses existing verified local dependencies; no downloads were needed.
