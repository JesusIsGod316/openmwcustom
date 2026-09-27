# Vulkan publication repair checkpoint — 2026-09-26

## Result and limits

Three independently switchable renderer changes are implemented, built locally,
and measured in Vulkan-only off/on runs of the same executable. Mean frame time
in this fixed Seyda Neen exterior workload fell from 68.509 ms to 61.711 ms
(9.92% less time; approximately 14.60 to 16.20 FPS). This is a measured partial
repair, **not acceptable overall performance or a complete native renderer**.
The automatic workload is not evidence for all scenes, moving-world stutter,
pixel parity, arbitrary mod compatibility, or save round trips.

No graphics-quality setting was reduced between the arms. Existing OMWFX/F2
post-processing is still unavailable; DLSS, GPU skinning, and earlier actor/rock
visual defects are not implemented or repaired by this checkpoint.

## Source and build identity

- Worktree: `C:/Users/LSCha/.codex/worktrees/vulkanmw-phase3c-metadata/OpenMW custom Build`
- Branch: `vulkanmw/phase3c-native-runtime-substitution`
- Base HEAD: `336a4cf8668afa1a4f03069785421e9a5e2b74ef`
- Changes remain uncommitted, alongside preserved earlier work. No push was made.
- Full game build: `build/phase3c-engine`, MSVC 19.44, `RelWithDebInfo`, target `openmw`.
- Build log: `build/publication-v2-build.log`.
- Measured executable: `C:/VulkanMW-Publication-V2-Test/openmw.exe`
- SHA256: `fb312a5cdbed5e6a8494ab8487dc4b7bfc02d24c94dbd5acee31ee5e9bd64aa9`
- Package manifest: `C:/VulkanMW-Publication-V2-Test/source-changes.json` records
  dirty source hashes as well as HEAD. All seven runtime files touched by this
  repair still match that manifest after testing. Only tests/documentation were
  expanded after the measured build; it was not rebuilt after measurement.

The Shared Project Context Archive control/current-state/decision material was
consulted before implementation. Source and local evidence remain authoritative
for this checkpoint; the shared archive has not been edited. Earlier unsuccessful
experiments were not silently promoted into this cohort.

## Implemented mechanisms

### 1. Model-group publication

`OPENMW_VK_GROUP_PUBLICATION=1` partitions static population publication by
cell/model, instead of rebuilding a cell-wide placement payload when one object
changes. Model/transform changes dirty the old and new affected groups, while
unaffected groups retain their handles and immutable payloads. Empty groups are
removed; cell removal and epoch reset clear ownership and publish retirements.
The existing residency system remains responsible for GPU-safe retirement.

This changes the producer's unit of publication, rather than caching another
full-cell rebuild. It does **not** yet eliminate per-frame upstream object
capture, nor make every object update O(1). Updating a very populous model group
still copies that group's placements. Create/retire transitions still use the
general transaction route.

### 2. Validated update-only chunk transactions

`OPENMW_VK_CHUNK_TRANSACTIONS=1` avoids copying the entire RenderWorld when a
batch only replaces existing chunk records. It first copies and validates all
replacements, live handles, revisions, bounds, references and unchanged member
lists. Only after every operation succeeds does it move prepared records into
stable storage; those moves are statically required to be non-throwing. Sequence,
epoch and revision semantics remain intact. Invalid batches publish nothing.

Mixed operations, creation/retirement, and repeated handles retain the ordered
general publisher. `OPENMW_V4_FULL_WORLD_PUBLICATION` overrides this fast route.
The optimization assumes the existing published world is valid and proves the
local mutations preserve its invariants; it does not skip validation of changed
records or overwrite the normal save/game-state representation.

### 3. Resident pipeline inventories

`OPENMW_VK_RESOURCE_INVENTORIES=1` seals pipeline inventories at realization and
retains them by immutable resident-root identity. Membership updates discover
only new/replaced residents and adjust pipeline reference counts for departures.
The resulting inventory is composed before scene/residency publication, leaving
the previous snapshot untouched on allocation failure. Inventories retain current
resources, not an unbounded history. Normal graph traversal and per-view Vulkan
pipeline-handle checks remain in place; there is no cached universal 'QC passed'.

An initial asset-only version failed to reduce the inventory cost in a real run.
It was revised to retain resident inventories, rebuilt and remeasured before
this checkpoint. This distinction matters: per-asset caching alone still left
the expensive resident-graph walk.

All three new switches default off in the engine. They are enabled together in
the private playable profile below. Disabling them restores the comparison
path; none is a required change to mod files.

## Runtime evidence

Directory: `C:/VulkanMW-Publication-V2-Test/Benchmarks`.
Each subdirectory contains `manifest.json`, `console.log`, `gameplay.jsonl`,
`runtime.jsonl`, and `frame-profile-summary.json`. Manifests pin executable,
driver, benchmark script, configuration and source-manifest hashes.

Conditions: same binary and content load order; separate fresh private data per
run; Vulkan required with no OpenGL fallback; 1920x1080, full render scale;
existing shadows/refraction/visibility settings unchanged across arms. The
automated new-game scene uses Seyda Neen, random seed 123456, 15 seconds of
warmup and approximately 30 seconds of wall-clock sampling, then quits normally.
No compilation ran concurrently with the measured game. No OpenGL runs were made.

| Run | Mean ms | Median ms | Per-run p95 ms | Frames |
| --- | ---: | ---: | ---: | ---: |
| publication-0-control | 69.280270 | 67.9760 | 82.0964 | 433 |
| publication-1-combined | 62.403394 | 61.2909 | 72.2478 | 480 |
| publication-2-combined | 61.018032 | 60.9972 | 68.3054 | 492 |
| publication-3-control | 67.737633 | 66.4339 | 74.9018 | 443 |

Comparison above uses the arithmetic mean of the two run means per arm, not a
pooled percentile or an average of instantaneous FPS. This short cohort supports
a scene-specific gain, not a general performance guarantee or statistical
confidence interval. All four exited 0 and reported no frame-profile health
issues. Every recorded original configuration/Lua-storage hash remained unchanged.
All four generated settings files have SHA256
`974be14e6e703d08fca26344764e191d44ef7dc3f2e3c7982945a2ad86efc081`.

Sampled CPU exclusive timings (means of the two per-run means):

| Work | Control ms | Repaired ms |
| --- | ---: | ---: |
| Population publication flush | 4.612 | 0.075 |
| Static pipeline inventory construction | 2.565 | 0.152 |
| Command recording on frame thread | 12.561 | 13.206 |
| Object capture | 8.289 | 8.411 |
| OSG update | 4.758 | 4.810 |
| Actor CPU preparation | 3.027 | 3.073 |
| Actor stream update | 2.972 | 2.956 |

These scopes are sampled in 15–17 steady frames per run, not every frame.
Lua synchronized-update timing also varies materially (about 4.66–7.73 ms), so
do not attribute every change in total frame time to a single scope.

GPU main-view intervals remain approximately 33 ms, shadows 15.5 ms, and
refraction 4.5 ms. Those are separate GPU intervals; do not add them to CPU
exclusive times or infer that CPU removal alone will produce 60 FPS. Recording
is not improved by this repair and must be addressed separately.

The initial, superseded package `C:/VulkanMW-Publication-Test` used SHA256
`be7d417a06c6fcaed5d712e1c70551a9754eafc96534da1dc9e7e9eb14e941a6`.
Its four means were 67.156763 / 63.118773 / 63.069181 / 67.094567 ms in
control/combined/combined/control order. Inventory cost remained about 2.6 ms;
that prompted the resident-level correction. Keep the two binaries' results
separate. Eight automatic game runs completed across the two build iterations.

## Validation and remaining errors

- Two full game build iterations completed successfully. Existing numerical
  conversion, shadowing and unused-variable warnings remain; this is not a
  warning-free build claim.
- Expanded Release test suite: **17/17 passed**, including original population,
  static-sync and publication tests, and full-world-publication control coverage.
- Group/atomic-transaction test: **15,331 checks**, including all switch
  combinations, invalid-batch rollback, replay/stale-epoch rejection and 240
  deterministic moves/model changes/deletions/reinsertions compared with the
  baseline semantic placements.
- Resource-inventory tests cover deduplication, no rediscovery of unchanged
  resident roots, removal/reference counts, old-snapshot preservation, and
  ordinary visitor traversal.
- Python benchmark/profile tests passed; `git diff --check` passed. Git warns
  about pre-existing CRLF conversion policy. Baseline assert-based test targets
  intentionally override NDEBUG and MSVC reports D9025 for that override.
- Runtime tests are not Vulkan validation-layer or pixel-difference certification.

Both control and repaired console logs still report:

- Invalid localization context `openmw-third-person-alt-attacks`.
- `ErnPerkFramework` missing for `erncultistperk` / `ernexampleperkpack` scripts.
- `textures/betterbars_bar.dds` decode unsupported; UI placeholder used.
- Legacy content-script operator warnings and new-game Bardcraft state messages.

These are not fixed here. Normal termination must not be reported as full mod
compatibility. No existing user save was loaded or rewritten in these tests.

## Exact files changed for this repair

Runtime:

- `components/rendercore/staticpopulationproducer.hpp`
- `components/rendercore/renderworld.hpp`
- `components/rendercore/updatebatch.hpp`
- `components/render/backend/vsg/pipelineinventory.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `components/render/backend/vsg/vsgruntimehost.hpp`
- `components/render/backend/vsg/vsgsemanticsession.cpp`

Tests and tooling:

- `tools/vulkanmw/tests/CMakeLists.txt`
- `tools/vulkanmw/tests/group-publication-tests.cpp` (new)
- `tools/vulkanmw/tests/resource-inventory-tests.cpp` (new)
- `tools/vulkanmw/tests/test-publication-cohort.py` (new)
- `tools/vulkanmw/run-publication-cohort.py` (new)
- `tools/vulkanmw/PUBLICATION-REPAIR-CHECKPOINT-20260926.md` (this file)

Several listed files already contained earlier uncommitted changes. The package
source manifest captures the combined tested state; do not mistake the entire
working-tree diff for work performed in this repair.

Generated outside the repository: the two private packages and benchmark
directories, and the final `Play` profile. Its `profile.json` enables the three
switches; its README records the limits of the repaired build. No normal user
configuration, mod archive, save, global environment or driver profile was edited.

## Local handoff and next substantive work

Optional private launcher:
`C:/VulkanMW-Publication-V2-Test/Play/Start-Vulkan-Clean.cmd`.
It requires no elevation, preserves mod content order, starts the normal menu,
and uses isolated settings/save/Lua-data paths. Choose New Game; old saves are
deliberately absent. Do not launch the executable directly for this experiment.
The launcher uses the current worktree's verified profile tool; it is a local
handoff, not a portable public release.

No user retest is needed to establish the small performance gain already
measured. Visual validation, extended movement and compatibility testing are
still needed before promotion.

Next changes should target the remaining measured costs, not rebuild these
already-cheap publication stages again:

1. Replace broad per-frame object inspection with engine-owned change
   notifications and persistent bindings for covered objects. Keep narrow
   fallbacks for genuinely unsupported mutation/controller paths.
2. Reduce recording and dynamic compilation using persistent draw organization
   and safe worker-owned work; do not assume all measured submission time is
   parallelizable or traverse mutable OSG state concurrently.
3. Address the GPU path as well as CPU architecture. Local-light evaluation and
   shadow work require their own controlled experiments. Current classic light
   falloff is not radius-clipped, so truncating every light to its nominal radius
   would be a rendering change, not a free correctness-preserving optimization.
4. Continue native actor/effect producers and GPU deformation after establishing
   semantic parity. OSG has not been removed by this checkpoint. F2/OMWFX needs
   an explicit Vulkan implementation; a clean configuration does not supply it.

Retain current mod interpretation, gameplay updates and save-format boundaries.
Performance promotion follows representative runtime and visual evidence, not
just successful compilation or the roadmap phase label.
