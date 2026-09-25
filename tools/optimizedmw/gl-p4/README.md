# OptimizedMW GL-P4R / P5 compile pipeline

Parent: GL-P4 compile-scheduler build 1487a9ab6081f82424cbe7b96103542c66c85ea3.

This branch repairs the P4 starvation/cheap-drain defect and adds two independent mechanisms for the next remaining GL hitch class.

## P4R scheduler repair
- Cheap operations continue draining inside the per-frame time budget even when they are old.
- Only a genuinely over-budget operation may use the one-per-frame starvation escape hatch.
- Maximum ordinary operations per frame is raised to 12 while the total time budget remains bounded.
- Cost history is tracked per producer class + GL operation type rather than globally by type.
- A decaying risk estimate catches repeated under-predicted operations without permanently quarantining cheap work.
- Queue-age default is 120 frames instead of 12.

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
