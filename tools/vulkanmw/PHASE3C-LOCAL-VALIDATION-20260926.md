# Phase 3C local checkpoint — 2026-09-26

## Outcome

The forced-actor skeleton publication defect is repaired. Native actor pose
coverage increased and missing-boundary seed failures disappeared in the tested
Seyda Neen scene. **This is not a performance success or a finished native actor
migration.** The native arm remained approximately 14 FPS and was slower than
both same-executable legacy-capture runs. Do not promote it as faster than
OpenGL, or ask the user to repeat the old build merely to verify these numbers.

The user has authorized overlapping migration, Vulkan-specific optimization,
post-processing support, and rendering repairs. The architecture plan now records
this change: phase numbers do not postpone measured performance work, and Vulkan
need not use OpenGL's optimization machinery. Compatibility remains a requirement.

## Source and executable identity

- Baseline ancestor: `805d5eb277987c3357affdd85698ed51c781bb30`.
- Metadata repair: `a25f4f868729aa6af15b5729e7478df86cbe776f`.
- Benchmark executable source: `fee3a7523ffa1685bdc4d743a0d3a149ed11c85c`.
  This includes the forced-legacy control's unused native-seeding fix.
- Branch: `vulkanmw/phase3c-native-runtime-substitution`; existing PR #9.
- Local package: `C:/VulkanMW-Phase3C-Test`; old `C:/VulkanMW` untouched.
- Executable SHA256:
  `d61806f6790ddb69ca7880f689664d8cf2cf40b4c39183a021a103ba71efd966`.
- MSVC 19.44.35228, x64, optimized RelWithDebInfo; `--version` reports
  OpenMW 0.52.0, revision `fee3a7523f`.
- Four benchmark manifests record the same executable, shader package, benchmark
  driver, and scene-script hashes. Their `source_head` field is `unrecorded`:
  source attribution here comes from the local build/commit and packaged version,
  not a claim that the launcher automatically captured Git state.
- Driver SHA256:
  `c49d51737a3c3e3ed6a05a3fc6c1be7f8c9e2e3488b42b4c55c4925fc3c26314`.
- Scene script SHA256:
  `9f27b87c9011bf94c5047936bf8044a4f2e2a0838649a85edb463318b8085ca9`.
- Shader manifest SHA256:
  `bc4c6b6493a7f229a5ed99b109031328ba882cde8e7ca40dd252546ee903e411`.

Later documentation and Visual Studio conformance-tool output-path repairs do
not change this already-tested executable. Rebenchmark any rebuilt executable
before attaching these timings to it.

## Same-executable hardware runs

Private profiles, same normal mod/config chain, 1920x1080, Vulkan retained,
uncapped/VSync off, Seyda Neen, seed 123456, 15-second warmup and 30-second measured
interval. Runs were sequential, with no local compiler running. All exited zero;
all original configuration-chain hashes remained unchanged. No normal saves were
copied or loaded. "Legacy" below means **Vulkan with evaluated actor pose capture**,
not the OpenGL renderer.

| Run order | Mean ms | Median ms | p95 ms | Frames |
| --- | ---: | ---: | ---: | ---: |
| Native 1 | 70.373433 | 68.8713 | 84.1989 | 426 |
| Legacy 1 | 66.846936 | 66.4661 | 73.7283 | 449 |
| Legacy 2 | 55.618574 | 53.8218 | 67.2108 | 539 |
| Native 2 | 70.763530 | 68.5161 | 88.5490 | 423 |

Evidence directories beneath `C:/VulkanMW-Phase3C-Test/Benchmarks`:

- `native-1/20260926-173304-gameplay-163124`
- `legacy-1/20260926-173547-gameplay-162676`
- `legacy-2/20260926-173809-gameplay-158028`
- `native-2/20260926-174312-gameplay-136720`

The second control run had materially different observed activity: no sampled
effect rebuilds, fewer changing static placements, and different gameplay-stage
costs. A fixed random seed does not establish frame-exact equivalent animation,
physics, or effect activity with wall-time stepping. Do not pool these four runs
into a precise causal speedup/regression percentage. Nevertheless, neither native
run demonstrates an FPS benefit; this patch is a coverage repair only.

## Coverage and remaining cost

Sparse steady-sample medians (individual counters need not sum exactly):

| Counter | Native 1 | Native 2 | Legacy controls |
| --- | ---: | ---: | ---: |
| Native actor poses | 13.5 | 13 | 0 |
| Legacy actor poses | 34 | 35 | 48 |
| Native tracks sampled | 438 | 422 | 0 |
| Seed failures | 0 | 0 | 0 |

The supplied earlier baseline showed 1–2 native actors and 31 seed failures.
It used another executable, so it is coverage context, not a matched speed test.
Active smooth blends, procedural rotations, and animated collapsed-parent paths
still fall back. Those guards were not disabled to inflate coverage.

In the native runs, sampled CPU intervals included object capture ~8.2 ms,
actor capture ~2.4–2.5 ms, dynamic realization ~10.7–11.1 ms, command recording
~13 ms, and OSG update ~4.7–4.8 ms. These are inclusive/sparse intervals, not
independent additive frame costs or GPU timestamps. Actor preparation scopes are
per batch; their median is not the total actor deformation cost.

1682 persistent draws were reused. Geometry/material snapshots and effect
realization still perform work. Native visibility reported frustum rejections
but **zero occlusion rejections and zero examined occluder triangles** in these
samples. The occlusion flag being enabled is not proof of active useful occlusion.

## Compatibility limitations

All four runs contain the same 19 normalized error-level log lines, including
missing cell references, a localization-context error, an unsupported Lua
handler, and missing `ErnPerkFramework` references (plus traceback lines). The
`betterbars_bar.dds` decode warning also remains. Equality across arms does not
make these harmless or prove full mod compatibility. These errors were not
fixed or suppressed in this slice.

No new visual comparison, existing-save round trip, equipment change, first-
person transition, or cell-transition acceptance was performed. Focused CPU
tests cover metadata publication/composition/rebuild semantics; they are not
a substitute for those runtime checks.

## Validation and development workflow

- Original forced-skeleton builder reproduced the missing-boundary regression.
- Repaired builder passes 54 C++ checks under MSVC Release `/W4 /WX`.
- Five Python benchmark-control tests pass; focused CTest 2/2.
- Phase 0, 1, 2, 3A, 3B, 3C source contracts pass.
- Full local production compile/link passed; follow-up incremental build took
  50.7 seconds. Existing `cells.hpp` conversion warnings remain.
- Full Windows CI for **a25f4f8687** passed: run `36274875422`; focused CI run
  `36274875474` passed. These results do not imply CI passed for later commits.
- Packaged executable startup/version and four normal benchmark exits passed.
- Local Visual Studio conformance-tool build initially rejected a doubled
  absolute output path inherited by its deferred CMake target. The repair sets
  concrete per-configuration output paths for that tool only; Ninja and the
  production target layout remain unchanged. After repair, full conformance-tool
  compile/link and `--help` startup passed. This is a build/startup check, not a
  new exhaustive real-NIF visual corpus run.
- After the tooling/roadmap edits, focused CTest again passed 2/2, all six source
  contracts passed, and `git diff --check` passed.

The reusable local build lives in `build/phase3c-engine`, focused tests in
`build/phase3c-metadata`, and verified dependencies in `build/local-deps`.
`build/LOCAL-BUILD.md` records the commands and dependency provenance. These
ignored build artifacts are not preserved by a Git worktree archive. Keep this
checkout active for incremental development; do not repeatedly download CI
packages or rebuild unrelated targets for each small edit.

## Next performance-led migration work

1. Trace repeated static placement/effect invalidation and rebuilds to their
   authoritative producers. Fix unnecessary churn or replace that producer with
   persistent native state; do not merely cache another whole-scene capture.
2. Move hot dynamic object/controller/material and deformation output to native
   bindings where equivalence can be tested. Broaden animation support without
   leaving native evaluation running beside equivalent evaluated-OSG work.
3. Profile actual command recording and native deformation separately. Shared
   immutable inputs, balanced jobs, per-worker resource ownership and fence-safe
   reuse are prerequisites, not a ban on bringing threading/GPU work forward.
4. Validate occlusion's actual input/coverage before claiming a gain. Bounds and
   visibility must remain conservative and specific to main/shadow/water views.
5. Extend F2/.omwfx compatibility on native rendering interfaces. The current
   `nativepostprocess` Copy/EdgeAA/Depth facility is **not** a Rafael shader-pack
   implementation and does not execute the configured .omwfx chain. Retain that
   distinction in user-facing claims and benchmark settings.

Khronos' [command recording sample](https://docs.vulkan.org/samples/latest/samples/performance/command_buffer_usage/README.html)
provides a reference for separate per-frame/per-thread pools, balanced recording
work, and pool reuse. Its own measured speedups are not predictions for this
project. Use the local production source and hardware evidence to decide which
mechanism to implement first.

## Changed source files in this checkpoint

- `components/nifrender/actormodelcomposer.hpp`
- `apps/openmw/mwrender/v4enginerenderbridge.cpp`
- `tools/v4/cp4/architecture-benchmark.py`
- `tools/vulkanmw/check-phase3c-native-runtime.py`
- `tools/vulkanmw/tests/CMakeLists.txt`
- `tools/vulkanmw/tests/phase3c-metadata-tests.cpp`
- `tools/vulkanmw/tests/test-benchmark-control.py`
- `.github/workflows/vulkanmw-phase3c.yml`
- `.github/workflows/vulkanmw-phase3c-windows.yml`
- `tools/vulkanmw/PHASE3C-NATIVE-RUNTIME-SUBSTITUTION.md`
- `tools/vulkanmw/PHASE3C-SKELETON-PROVENANCE.md`
- `tools/v4/cp3b3/OpenMWRealNifTool.cmake`
- `tools/vulkanmw/VULKANMW-ARCHITECTURE-PLAN.md`
- `tools/vulkanmw/PHASE3C-LOCAL-VALIDATION-20260926.md`
