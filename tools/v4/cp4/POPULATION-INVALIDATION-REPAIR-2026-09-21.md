# Exterior population invalidation repair — 2026-09-21

## Source/build boundary

Branch `v4.0-cp4f-exterior-closeout`, HEAD
`f4a1b7ed7dc67c6e973efe49f01d2c1d52c8d75b`. Existing dirty work preserved;
no commit, push, desktop-shortcut replacement, or archive write.

Production MSVC RelWithDebInfo executable:
`C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/openmw-build-cp4f-qc/openmw.exe`

SHA256: `4216086EAE1CECF372CE6B9E58308C1AF5DF9610367E23AED286FA277A9F9971`.

## Evidence and cause

User new-game evidence:
`C:/Users/LSCha/Documents/My Games/OpenMW/runtime-qc-evidence/20260921-183039-terrain-repair-newgame`.
Frames 1904 and 1934 each compiled 224 population groups, taking 4622.949 and
4899.6228 ms respectively inside static synchronization. These are sampled,
inclusive diagnostic timings, not clean FPS benchmarks.

The cause-only executable (`EE5C950D5C6686FB309A1964992E13830455DF8168633D308A0CAFE9570CF7FF`)
added reason counts without changing invalidation. Its isolated exterior run is
`runtime-qc-evidence/20260921-population-cause` under the same OpenMW user folder.
Frames 347–349 reported 236, 244, and 259 existing groups invalidated by their
cell revisions, with zero option changes, plus actual newly created groups.
Static synchronization took 4535.8061, 3503.9353, and 4321.8564 ms.

`StaticPopulationResidency::prepare` previously treated any chunk revision as a
reason to replace every resident model group in that chunk. Source publication
can legitimately change just one group (the test also observed live grass model
insertions). Unrelated, immutable residents were consequently realized, uploaded,
and fence-retired repeatedly.

## Repair and control

- Resident reuse now checks the exact group placements and resource revisions,
  rather than rejecting solely because another group changed the cell revision.
- Comparison includes identity, translation/rotation/scale, bounds, every LOD
  field, semantic masks, and lighting. No lossy hash, tolerance, or quality cut.
- Unchanged groups retain their existing packing origin and matching root
  translation when cell bounds change. Absolute placements are unchanged;
  distance visibility still reads the current cell bounds.
- New/replaced graph commits retain strict chunk-revision validation, world
  epoch/revision checks, and existing fence-backed retirement.
- Producer equality now includes `smallFeatureEligible`, previously omitted.
- `OPENMW_V4_COARSE_POPULATION_REBUILD_CONTROL=1` restores the old whole-chunk
  invalidation policy in the same executable. It is off by default. Existing
  batch-upload control stays default-off; no extra GPU idle was introduced.
- Existing sampled diagnostics report new groups, changed chunk stamps, changed
  options, stale dependencies, and the coarse-control state. Reason counts may
  overlap; a changed chunk stamp is not itself a rebuilt group in repaired mode.

## Exact source files changed in this repair

1. `components/render/backend/vsg/staticworldplan.hpp`
2. `components/render/backend/vsg/staticpopulationresidency.hpp`
3. `components/render/backend/vsg/vsgruntimehost.cpp`
4. `components/rendercore/staticpopulationproducer.hpp`
5. `tools/v4/cp4/static-sync-state-tests.cpp`
6. `tools/v4/cp4/population-environment-contract.py`
7. This repair record.

## Verification

- Targeted MSVC production and recovery-test compilation/link: passed.
  Logs in `C:/Users/LSCha/AppData/Local/Temp/openmw-cp4-local-deps/`:
  `population-repair-build.log`, `population-repair-targeted-build.log`.
- Rebuilt registered CTest suite: **14/14 passed** (`population-repair-ctest.log`).
- Same-binary CPU fixture: a 224-group cell with one moved placement and changed
  cell bounds produces **1 upsert repaired, 224 upserts control**. Unchanged
  object identity and packing are retained. 100 stable frames before and after
  the mutation require no rebuild. All placement comparison fields, real shared
  texture/material/mesh changes, options, removal, stale commit rejection and
  fence retirement are checked.
- Seven CP4 source-contract scripts passed. The population contract now checks
  the new explicit control argument instead of the old two-argument call.
- Python diagnostic suite: **16/16 passed** when run directly. An initial
  discovery command selected zero tests and was not counted as a pass.
- `git diff --check`: passed (`population-repair-diff-check.log`).

The temporary build dependency tree was incomplete: restored the missing
SQLite C source from the exact CMake-pinned archive after SHA512 verification;
restored 19 missing OSG headers from the previously verified local dependency
package after checking all corresponding existing OSG headers matched. No
existing dependency file was overwritten. A broad all-target build also found
a missing `googletest/src/gtest-all.cc`; that unrelated all-target dependency
remains unresolved. The subsequent explicit production/recovery targets and
14 registered tests succeeded. Do not call this a successful all-target build.

## Real-scene check and limitations

Repaired run: `runtime-qc-evidence/20260921-population-repair`.
Once initial population insertion settled, frames 307/337/367 reported **293
changed chunk stamps but only 1 population upsert**. Static synchronization was
41.4743 ms at frame 337 and 40.9475 ms at frame 367. At frame 367, dynamic capture
was 86.2169 ms, dynamic realization 155.7410 ms, and submission/presentation
319.4134 ms. Thus the original recompilation fanout is corrected, but significant
remaining costs and memory pressure still make the scene unplayable.

These runs use strict QC and Vulkan validation, a direct initial-cell diagnostic
(not the intro/ship route), and evolving scripted scene state. They are not
controlled gameplay benchmarks or evidence of overall runtime acceptance.
Water/sky/material appearance and the other previously recorded Vulkan
validation/allocation issues remain open. No renderer-quality features were
disabled to obtain the reuse result.

## Test display and profile correction

The initial diagnostic profiles used 800x600 and mistakenly selected numeric
window mode 0 (exclusive fullscreen), causing the user's aspect-ratio/focus-loss
complaints. The intermediate 1280x720 image verified 16:9, but the user requested
1080p. The final temporary profile is
`runtime-qc-evidence/20260921-population-repair-1080p/settings.cfg`:

```ini
[Video]
resolution x = 1920
resolution y = 1080
window mode = 2
window border = false
minimize on focus loss = false
```

Mode 2 is windowed; a borderless 1920x1080 window preserves the requested output
and does not request exclusive-fullscreen minimization. The existing render
scale is preserved. Normal user display configuration was not edited.
The small earlier profiles remain with their evidence so recorded tests are
not retroactively mislabeled. Computer-use skill was used to inspect/activate
the diagnostic windows and close test instances.

The first direct-cell test omitted an explicit `--user-data` override and a mod
created `saves/player/_zZz__Wake_up.omwsave` at 19:14:24 (734400 bytes). Creation
and write times confirmed it was a new test file, not an existing save. It was
moved intact to `20260921-population-cause/generated-test-autosave.omwsave`.
Subsequent launches explicitly direct both config and user data into their
own evidence directories. No regular save was loaded or existing save deleted.
