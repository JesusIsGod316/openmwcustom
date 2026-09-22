# Water compilation and unchanged static-scene runtime repair

## State and scope

- Checkout: `C:/Users/LSCha/Documents/ChatGPT/OpenMW custom Build-cp4f`.
- Branch: `v4.0-cp4f-exterior-closeout`.
- HEAD: `f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`.
- Existing uncommitted work preserved. No commit, push, CI run, or runtime promotion.
- Read the Shared Project Context Archive control block and relevant current records before implementation. Retained the pre-submit safety census, immutable semantic revisions, fence-based resource ownership, and independent same-executable controls. No whole-scene GPU eviction or adaptive scheduling was introduced.
- This is a targeted correctness/runtime repair, not CP6 performance optimization. Exterior visual correctness, load time, and gameplay frame rate are NOT accepted.

## Water failure and repair

The previous manual run, `20260919-214335-gameplay-84196`, stopped before submission with 239 unrealized refraction pipelines. The allocation repair from the preceding batch did not fix this separate compile-context failure.

Reflection/refraction views begin masked off. Initial VSG view discovery skips them, so the compile manager has no matching contexts. A filtered compile previously returned success even when no context matched.

- Explicitly register both offscreen water views against their framebuffers after initial viewer compilation; leave their rendering masks disabled until cell water is active.
- A view-specific compilation now requires the same view identity and view ID. Zero matched contexts are an initialization failure, not success.
- Preserve the pre-submit census. Fatal messages retain the full failure count but show at most eight pipeline details plus an omitted-detail count.
- `OPENMW_V4_UNREGISTERED_WATER_VIEWS=1` retains the broken-registration control, still failing closed before submission.
- Wet caves use the same water views, independently of exterior sky. The regression fixture covers wet interiors, underwater interiors, exterior sky/shadows, new static objects, GUI-only loading frames, and repeated transitions.

Before repair the real Vulkan fixture reproduced two unrealized active pipelines. After repair it passes. The latest same-executable unregistered-view control exits 1 with `No registered compile context for VSG view`, as expected.

## Direct-start smoke: reached exterior, but FAILED visual/performance acceptance

Evidence: `C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260919-water-repair-smoke`.

This smoke used the intermediate water-only executable with SHA256:
`09D1978B778FC09A4CB982007A5FA35A7E877EED3256DB709E2218665E45EB9C`.

Command arguments were `--config <evidence> --user-data <evidence>/userdata --skip-menu --start "Seyda Neen" --no-grab`. No savegame argument was supplied. A private configuration inherited the normal content list, copied Lua storage and other settings, and changed only window mode/border and resolution to 960x540. The private userdata directory was initially empty. The game/mod created a private autosave there.

**This was the wrong route for the user's acceptance test.** The user reported that it appeared to load their regular save rather than starting the New Game test, with white terrain, less than 1 FPS, and a distorted-looking scene. Preserve that failure report; do not classify this smoke as a successful gameplay test or dismiss the visual result because a pipeline guard passed.

Nine exterior cells loaded without the prior pipeline fatal. The process was subsequently closed normally; the log ends with `Quitting peacefully.` at 22:00:01.215 local time. Its process exit code was not captured.

The diagnostic trace contained ten sampled world frames:

| Stage | Median ms | Maximum ms |
| --- | ---: | ---: |
| static_sync | 688.1410 | 1547.7164 |
| dynamic_realization | 102.35465 | 2175.8263 |
| dynamic_capture | 82.19755 | 1721.5211 |
| pipeline_audit | 3.21115 | 3.6209 |
| vulkan_present (inclusive) | 984.6443 | 4636.3083 |

These stages overlap and must not be summed. Terrain worker time was 16182.2864 ms versus 0.0185 ms queue wait. Exterior transition time was 20864.8085 ms. This different route, window size, and cache state is NOT comparable to earlier hatch runs and proves no load-time or FPS improvement.

## Static runtime repair prompted by that trace

The host rebuilt the entire static world plan every frame, including full mesh payload validation, before residency preparation determined that there were no changes. This happened despite persistent static residency.

`StaticWorldSyncState` now compares exact stamps for the static inputs before that work:

- Instance and population membership/order, typed handle slot/generation, resource revision, world epoch, and all static planning options.
- Each referenced model and its mesh/material/texture dependencies, including hidden and inactive nodes that may affect legacy sort state.
- Actor-only placement/skeleton changes do not invalidate unrelated immutable static geometry. Classification changes back to static do invalidate it.
- No hashes of geometry, no retained payload copies, no GPU allocations, and no graphics features disabled.
- Stamps are acknowledged only after successful scene publication, or after the full planner confirms that no residency mutation is needed. Failed attempts remain dirty.
- `OPENMW_V4_REBUILD_STATIC_PLANS=1` forces the original full-planning path using the same executable.

This removes a specific repeated-work path. It does not prove a gameplay speedup, fix the terrain worker, or remove the other measured runtime costs.

## White terrain and remaining issues

`apps/openmw/mwrender/v4terrainsource.cpp` publishes LAND positions, normals, vertex colors and triangle indices, but assigns a white diffuse/ambient material. It does not publish landscape texture layers, blend weights, or texture coordinates. `TerrainChunkSource` currently has a single generic material/mesh boundary without terrain-layer publication. This is a concrete incomplete terrain rendering path, not evidence that the user's PBR mods are at fault.

No terrain texture-layer repair is included in this batch. Do not substitute an arbitrary flat texture or claim material parity. The user's other visual distortions still need exact localization/visual evidence.

The long terrain preparation also remains open. The canonical OSG terrain preload builds rendering nodes, but its paging output is still consumed by scene insertion/physics logic. Simply disabling that preload would risk missing paged objects; no such bypass was applied.

The next acceptance route must be the normal menu **New Game -> prison ship -> hatch -> Seyda Neen**. Do not silently substitute `--skip-menu --start` or an existing save. Real cave-water appearance also remains visually unverified despite the transition fixture passing.

## Files changed in this batch

- `apps/openmw/CMakeLists.txt`: water GPU fixture and registered static-sync regression target.
- `components/render/backend/vsg/vsgruntimehost.cpp`: water registration, bounded fatal details, successful-static-input change check.
- `components/render/backend/vsg/vsgruntimehost.hpp`: static synchronization state ownership.
- `components/render/backend/vsg/vsgsubmission.hpp`: reject unmatched view compilation.
- `components/render/backend/vsg/staticworldsyncstate.hpp`: new exact static-input stamp comparison.
- `tools/v4/cp4/water-transition-tests.cpp`: new real Vulkan transition regression.
- `tools/v4/cp4/static-sync-state-tests.cpp`: new change detection/invalidation regression.
- `tools/v4/cp4/runtime-blocker-contract.py`: supplemental source checks for the above safeguards.
- This repair record.

The isolated smoke directory also contains its private copied configuration, logs, userdata and report. It is retained as failed-run evidence, not installed as the normal profile.

## Validation and build identity

Build directory: `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc`.

- MSVC RelWithDebInfo compile/link of `openmw.exe` and focused targets: PASS. Existing warnings in `engine.cpp` and `settings/categories/cells.hpp` remain; this is not a warning-free build.
- Rebuilt remaining regression executables; CTest: **14/14 PASS**, including shader package validation.
- Static-sync test: stable frames, all resource revision families, hidden materials, instance addition/removal/generation reuse, population replacement/removal, dynamic/static classification, view options, epoch reset, and failed-attempt retry: PASS.
- Real Vulkan water transitions: PASS with repair; PASS with `OPENMW_V4_REBUILD_STATIC_PLANS=1` control.
- Same final water fixture with `OPENMW_V4_UNREGISTERED_WATER_VIEWS=1`: expected failure reproduced before submission.
- GUI/loading isolation Vulkan fixture: PASS.
- Python gameplay diagnostic tests: **16/16 PASS**.
- Seven CP4 source contracts: PASS.
- `git diff --check`: PASS.
- VSG emits two informational `addViewDependentState ... no framebuffer` lines in these fixtures; these are retained in logs, not removed or treated as visual acceptance.

Latest executable SHA256:
`1A94A1DD654F7F3ED52D92A7B4694029C3634FF7501FA0F6F694D83CC0C3AC44`.

Logs: `static-sync-repair-build.log`, `static-sync-gate-build.log`, `static-sync-ctest.log`, `static-sync-state-test.log`, `static-sync-water-test.log`, `static-sync-water-control.log`, `static-sync-unregistered-water-control.log`, `static-sync-gui-test.log` in the build directory. Earlier `water-transition-before.log` preserves the unfixed fixture failure.

Normal `openmw.cfg`, `settings.cfg`, `input_v3.xml`, `shaders.yaml`, `global_storage.bin`, and `player_storage.bin` hashes were rechecked against the prior capture and remain unchanged. The protected `Leon_Valerius/_zZz__Wake_up.omwsave` hash remains `D34685C838BD1B4318DB34B1E0C1F00A7A2FE9A990FD21A120973297083AF99B`. No OpenMW game process remains running.

**Implementation/build/test gates passed. The final build has not been user-tested in the actual New Game/hatch route. No FPS, load-time, visual parity, or CP4F acceptance claim is made.**
