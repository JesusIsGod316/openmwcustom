# OptimizedMW GL-P4R / P5 compile pipeline

Parent: GL-P4 compile-scheduler build 1487a9ab6081f82424cbe7b96103542c66c85ea3.

The original P4R runtime at af120839420f20423a1f376c5785de4155a2ace3 was rejected after hardware testing exposed a scheduler hot-loop: full-queue rescans repeatedly re-ran OSG cost estimators on queues approaching 3,000 compile sets. This repair keeps the cheap-drain policy but caches the static OSG estimate for each compile set's current front operation and instruments selector overhead directly.

The P5 heavy/terrain mechanisms remain experimental and are not eligible for promotion until the common P4R scheduler path is clean.

## P4R scheduler repair
- Cheap operations continue draining inside the per-frame time budget even when they are old.
- Only a genuinely over-budget operation may use the one-per-frame starvation escape hatch.
- Maximum ordinary operations per frame is raised to 12 while the total time budget remains bounded.
- Cost history is tracked per producer class + GL operation type rather than globally by type.
- A decaying risk estimate catches repeated under-predicted operations without permanently quarantining cheap work.
- Queue-age default is 120 frames instead of 12.
- The OSG estimate for an unchanged front compile operation is reused across queue rescans and frames; EMA/risk remains live and uncached.
- p4-compile summary rows report candidate_build_ms, selection_ms, scheduler_total_ms, selection_passes, candidate_builds, candidates_scanned, estimate_calls, and estimate_cache_hits so selector overhead cannot hide behind GL-call timing.
- Scheduler repair stage 2 builds map/RTTI/OSG-estimator candidate descriptors once per frame, reuses the flat descriptors across cheap-drain passes, and refreshes only a selected CompileSet whose front operation advances. This preserves dynamic EMA/risk admission while removing repeated candidate reconstruction from the O(kN) drain loop.

## P5 heavy GL lane
Optional. Known/predicted heavy operations are admitted only after sustained smooth headroom. It does not preempt a driver call; it controls when the call begins. The cold terrain prior retires after four observations.

## P5 phased terrain compile
Optional. TerrainDrawable's compile work is split into individual pass StateAttribute operations plus a geometry/VBO operation. This preserves the original pass-before-geometry order but gives the scheduler interruption points between work units. Diagnostics tag:
- p5_terrain_pass_attribute
- p5_terrain_geometry_vbo

## Launcher modes
1. CONTROL-B1
2. CONTROL-B1-C
3. P4R-B1
4. P4R-B1-C
5. P4R-B1-C-HEAVY
6. P4R-B1-C-TERRAIN
7. P4R-B1-C-HEAVY-TERRAIN

The focused diagnostics remain nonblocking. p4-compile.csv records queue depth/age, budget, credit, prediction, actual cost, headroom, producer class, and admission reason.
