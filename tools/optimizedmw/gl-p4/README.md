# OptimizedMW GL-P4R — scheduler repair + staged terrain GL realization

Parent: tested GL-P4 compile-scheduler build
`1487a9ab6081f82424cbe7b96103542c66c85ea3`.

## Why this repair exists

The first P4 runtime matrix proved that smoothing IncrementalCompileOperation
work can dramatically reduce common render-handoff hitches, and that P3B +
P3C can stack once GL realization is controlled. It also exposed two problems:

1. The first scheduler treated almost all aged work as forced one-op-per-frame
   work, so the compile queue could grow into the thousands.
2. Individual terrain drawable / texture GL calls could still take tens of
   milliseconds and could not be preempted once entered.

P4R repairs the queue policy and then attacks the terrain half of the second
problem without introducing a shared-context publication race.

## Scheduler repair

- Compile cost history is tracked by producer class and operation kind.
- OSG's estimate is corrected by a learned actual/estimate scale.
- EMA and a decaying high-water guard prevent a 40 ms class from immediately
  returning to a 0.1 ms prediction.
- Old work that fits the budget is ordinary budgeted work and may continue
  draining in the same frame.
- Only old work that is actually over budget receives the one-forced-op
  starvation escape.
- A tiny 0.35 ms cheap-work drain floor prevents permanent backlog when ICO
  reaches the render thread with near-zero nominal spare time.
- The object cap is 12 and the age threshold is 60 frames for the test matrix.
- P4B render-handoff feedback remains active.
- Multi-context/stereo still fails open to stock OSG ICO.

## Next bottleneck: staged terrain realization

OpenMW's TerrainDrawable previously compiled all pass StateSets and every dirty
VBO/EBO inside one Drawable compile operation. P4R can optionally split this
into individually schedulable operations:

1. unique terrain textures,
2. unique programs,
3. terrain/pass StateSet finalization,
4. one VBO/EBO upload per operation,
5. geometry/VAO finalization.

A second independent toggle splits terrain position, normal, and color arrays
onto separate VBOs. This is experimental and exists specifically to determine
whether the remaining large terrain buffer upload is itself the long pole.

Both terrain changes default OFF.

## Focused launcher matrix

1. `CONTROL-B1-C` — stock/current OSG ICO + B1 + C.
2. `P4R-B1` — repaired P4B scheduler + B1.
3. `P4R-B1-C` — repaired scheduler + B1 + C.
4. `P4R-B1-C-STAGED` — mode 3 + staged terrain compilation.
5. `P4R-B1-C-STAGED-SPLITVBO` — mode 4 + split terrain attribute VBOs.

P3A/D/E/F and the V3.21 completion governor remain off.

## Diagnostics

`p4-compile.csv` adds raw OSG estimate, learned EMA, estimate scale,
predicted cost, actual cost, fits-budget and forced flags. The launcher also
retains render-handoff, paging, OSG frame/cull/draw/GPU/Compiling, VRAM, and
process-memory data.

Deep trace/profilers remain off for performance runs.
