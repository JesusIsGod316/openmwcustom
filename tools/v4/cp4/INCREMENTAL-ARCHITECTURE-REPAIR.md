# Incremental Vulkan performance repair - 2026-09-23

## Candidate identity and scope

This is the next bounded performance repair, not a completed replacement of
OpenMW's renderer-facing scene machinery. It targets repeated CPU work observed
in the Persistent test, while retaining the existing compatibility/control path.

- Branch: `codex/cp4f-material-frame-repair`.
- Base commit: `efa4f3694fad904b3546e7c846c756403950f0d2`.
- State: uncommitted local changes, including earlier repairs; not pushed or promoted.
- Candidate directory: `C:\OpenMW-Incremental-Test`.
- Executable SHA256: `7b6c2a251e4dc1d1063b503e99b467ec8554aed124e59f134e0e551d0e5603bf`.
- `persistent-build-manifest.json` records the executable, base commit, build-log
  hash, CMake-cache hash, and changed code/tool file hashes. Each capture embeds
  this manifest; the launcher rejects an executable that differs from its hash.
- The previous `C:\OpenMW-Persistent-Test` package is not overwritten. Its repair
  document, included as historical background, does not describe this executable.

The September 23 user instruction permits revising CP4/5/6 sequencing. The
compatibility foundations, bounded ownership, control paths and runtime
acceptance requirements remain. Authoritative context was read through the
previously retrieved archive text after a fresh Drive fetch failed decoding;
the actual checkout and latest local measurement report were checked directly.

## Work removed

1. **Repeated inherited-state construction.** An owning-thread, bounded cache
   reuses the merged state structure while the complete state binding/override
   lists remain equal. Mutable uniforms and attributes remain live references:
   their current values are still decoded into neutral records every frame.
   Replacement/removal, parent changes, modes, defines and render-bin changes
   invalidate the merge. Unsupported/large chains use ordinary merging.
2. **Warm geometry-stamp copying.** The persistent geometry cache compares
   source arrays/UV/index/TexMat inputs directly against its existing exact
   stamp. A hit no longer allocates/copies a fresh source stamp. Comparison is
   still byte-exact, so edits without dirty notifications are not hidden.
   Mismatches use the existing construction/validation path.
3. **Unchanged population-plan construction and deep copies.** Reconciliation
   uses persistent residents directly; only new/changed groups get new plans.
   Unchanged placement and asset-plan vector ownership moves through commit
   without copying their buffers. All transaction validation and allocation
   precede moving live residents. Submitted resources still use fence retirement.
   A verified model/group-index hint avoids repeated linear lookup; changed
   group ordering falls back to an exact model search.
4. **Duplicate immutable mesh comparison.** The immediate-effect contract can
   recognize the same privately immutable mesh owner without comparing its
   topology/index vectors again. Material, textures, identity, transforms and
   other mutable semantics are not exempted from their existing checks.

This does not cache mutable material values, remove the OSG traversal, introduce
a producer-side dirty journal, or make the whole renderer change-only. Population
group discovery and changed-chunk checks remain. Those are subsequent architectural
steps, not claims made for this package.

## Controls and memory

| Mechanism | Presence-enabled environment variable |
|---|---|
| Structural state and zero-copy warm geometry checks | `OPENMW_V4_INCREMENTAL_CAPTURE` |
| Direct population reconciliation and move commit | `OPENMW_V4_INCREMENTAL_POPULATIONS` |

Unset a variable to disable it; a string value of `0` still enables these controls.
Both are off outside opt-in launchers. The capture mechanism normally runs with
the previously introduced `OPENMW_V4_PERSISTENT_CAPTURE` enabled.

The state cache is limited to 2048 entries, at most 32 source state sets / 512
bindings per entry. Weak source ownership is pruned every 1024 lookups and on
full-cache misses; LRU bounds live entries. It temporarily retains referenced
attributes/textures until pruning/eviction. This is not a process-memory or
texture-byte cap, and long-session residency still needs measurement. The
previous geometry cache budget remains 128 MiB accounted data / 4096 entries,
with container/scratch and published-owner lifetimes additional.

Existing bounded neutral-data publication workers, static frustum culling and
conservative terrain occlusion are retained. No live OSG/Lua/gameplay state was
moved to workers. No broader building occluders, frame cap, resolution reduction,
save conversion, mod installation, or renderer fallback was added.

## Start this candidate

Use `C:\OpenMW-Incremental-Test\Start-Incremental-Vulkan.cmd` normally (no admin
needed). It enables this batch and the existing CPU/visibility mechanisms.
Choose NEW GAME, test the same exterior route and a short interior/NPC/menu
segment, then quit normally. The helper generates an evidence ZIP under this
package's `Test-Results`. Normal saves are not copied; the user-data directory is
private and normal configuration is read, not edited.

`Start-Incremental-Control.cmd` is a same-NEW-executable control that disables
only this batch's two mechanisms while retaining the earlier persistent paths.
It is available if needed; no rerun of an old rejected executable is requested.

`Start-Incremental-Profile.cmd` is optional. It uses installed Nsight and needs
an administrator launch by the user. It never elevates itself. Press F12 once
at the problem location for the bounded capture, then quit normally.

The experimental native post target is off, as in the previous measured run.
Rafael/F2/.omwfx parity, earlier multi-view evaluated-object occlusion, placement-
only GPU updates and command-recording parallelism are not implemented here.

## Validation

- Full MSVC Release Vulkan target linked: 0 errors, 67 compiler warnings.
- Full MSVC Release OpenGL control target linked: 0 errors, 45 compiler warnings.
  These are incremental-build warning counts, not unique/new warning counts.
- Rendering CTest: 15/15 passed, 3.86 seconds. Capture checks: 29/29 in each of
  ordinary, persistent and incremental modes. Includes live unmarked edits,
  source expiration, override changes and stable-buffer ownership checks.
- Population regression runs both old and incremental reconciliation: exact
  changes, resource/options invalidation, removals, failed/stale transactions,
  epoch/generation handling and fenced retirement remain covered.
- Validation-enabled Vulkan pixel matrix passed on the NVIDIA GeForce RTX 5050
  Laptop GPU in both incremental/persistent capture and ordinary-capture control
  configurations. No validation messages were emitted. Covers materials,
  water, terrain, sky, shadows, previews, GUI ordering and visibility routing.
- Gameplay launcher tests: 27 passed. Configuration/identity tests: 17 passed.
- Runtime-blocker source contract and generated-output materialization checks
  passed. No frozen generated source payload was edited. `git diff --check` passed.
- Staged shader package: 82 files verified; candidate startup/version smoke
  returned success. Runtime package before launch-preflight files: 319403799
  bytes (about 305 MiB). No downloads were needed; approximately 122 GiB free
  remained on C: after staging and preflight.
- Both candidate and same-executable control launch configurations were prepared
  without starting the game. The candidate enables both new mechanisms; the
  control disables only those two. Both retain persistent capture and terrain
  occlusion. All 84 changed code/tool source hashes are embedded in each capture
  manifest, the original configuration-chain hashes remained equal, and normal
  saves were not copied. The previous executable hash is unchanged.
- Initial test execution needed the runtime DLL PATH and explicit installed
  validation-layer path. A new microbenchmark initially used the wrong Uniform
  constructor argument type; corrected before the final fixture build. The pixel
  fixture previously inspected the emptied scratch mesh with persistent capture;
  it now checks the semantic mesh accessor and explicitly requires frozen ownership.

### Isolated CPU observations, not gameplay FPS

Single local fixture run; warm synthetic data, not a representative scene:

| Workload | Control | New path |
|---|---:|---:|
| Merge depth 8 / 65 uniforms / 2000 repetitions | 16.5409 ms | 0.8400 ms |
| Capture 12000 vertices / 200 repetitions (old persistent vs incremental) | 3.2645 ms | 1.5123 ms |
| Prepare 96 groups / 768 placements / 40 repetitions (indexed) | 6.2489 ms | 4.0231 ms |

The first row measures structural merge reuse, the second warm source-stamp
work, and the third population preparation only (not GPU upload or commit).
Do not convert these into expected FPS or add them together. The previous capture
still had substantial unaddressed whole-frame costs. No gameplay session or FPS
measurement has been made with this candidate; performance is not accepted yet.

Build logs, relative to the active source root:

- `build/cp4f-engine-vulkan/build/cp4f-engine-vulkan/incremental-architecture-build.log`
- `build/cp4f-engine-opengl/build/cp4f-engine-opengl/incremental-control-build.log`
- Final CTest details: `build/cp4f-rendering/Testing/Temporary/LastTest.log`.

## Files changed in this batch

Paths are relative to `C:\Users\LSCha\.codex\worktrees\cp4f-material-frame-repair\OpenMW custom Build`.
Other dirty files already existed and were preserved.

- `apps/openmw/mwrender/v4effectstatecache.hpp` (new)
- `apps/openmw/mwrender/v4effectcapture.hpp`
- `apps/openmw/mwrender/v4geometrysnapshotcache.hpp`
- `components/debug/gameplaydiagnostics.hpp`
- `components/render/backend/vsg/immediateeffectcontract.hpp`
- `components/render/backend/vsg/staticworldplan.hpp`
- `components/render/backend/vsg/staticpopulationresidency.hpp`
- `components/render/backend/vsg/vsgruntimehost.cpp`
- `tools/v4/cp4/rendering-capture-tests.cpp`
- `tools/v4/cp4/rendering-pixel-tests.cpp`
- `tools/v4/cp4/static-population-smoke.cpp`
- `tools/v4/cp4/static-sync-state-tests.cpp`
- `tools/v4/cp4/effect-stream-fastpath-tests.cpp`
- `tools/v4/cp4/rendering-tests/CMakeLists.txt`
- `tools/v4/cp4/gameplay-diagnostics.py`
- `tools/v4/cp4/diagnosticconfig.py`
- `tools/v4/cp4/test-gameplay-diagnostics.py`
- `tools/v4/cp4/test-diagnostic-config.py`
- `tools/v4/cp4/stage-persistent-candidate.ps1`
- `tools/v4/cp4/Start-Incremental-Vulkan.cmd` (new)
- `tools/v4/cp4/Start-Incremental-Control.cmd` (new)
- `tools/v4/cp4/Start-Incremental-Profile.cmd` (new)
- `tools/v4/cp4/INCREMENTAL-ARCHITECTURE-REPAIR.md` (new)
