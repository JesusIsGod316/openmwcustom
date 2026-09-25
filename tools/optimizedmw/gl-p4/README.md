# OptimizedMW GL-P4 compile scheduler

Parent: repaired GL-P3 integration build e62a7bd38c76cbb3d9bc226424134d146f816bcd.

This build targets the recurring IncrementalCompileOperation / renderingTraversals handoff hitch path.

Modes:
1. CONTROL-B1 — original OSG ICO, validated P3B one-helper.
2. CONTROL-B1-C — original ICO with B1+C.
3. P4A-B1 — cost-aware per-operation compile admission.
4. P4B-B1 — cost-aware admission plus rendering-handoff feedback and compile credit.
5. P4B-B1-C — primary stacking test.
6. P4B-B1-C-D — tests whether D can stack once GL realization is smoothed.
7. P4B-B1-C-COMPLETION — adds the existing V3.21 completed-set admission governor.
8. P4A-B1-C — separates cost-aware scheduling from handoff feedback.

P4 modes are startup settings. Mode 0 constructs the original OSG IncrementalCompileOperation.

The scheduler does not preempt OpenGL calls. Instead it measures drawable/texture/program costs, predicts the next operation cost, avoids launching operations that do not fit the current budget, class-prioritizes ObjectPaging/Terrain work, forces aged work to avoid indefinite starvation, and gives GL deletion its own small budget.

Diagnostics:
- p4-compile.csv: per-operation and per-frame scheduler evidence
- v3-render.csv: render-handoff events
- v3-osg-stats.log: frame/cull/draw/GPU/resource/Compiling statistics
- paging/resource/streaming/batching/shadow/GPU-memory logs
- process-memory.csv

Deep trace/profilers remain off because they would contaminate performance comparisons.
