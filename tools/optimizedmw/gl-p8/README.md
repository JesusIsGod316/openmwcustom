# OptimizedMW GL-P8 — dynamic draw synchronization and geometry ownership

Parent: optimizedmw/gl-p7-cpu-prep@5667ecf7c043509388e821fbe864625455e310fe

## Measured motivation

The P7 hardware matrix isolated the remaining rendering-traversal tail to OSG's DrawThreadPerContext dynamic-draw completion wait. P8 first attributes that wait before changing synchronization semantics.

## Benchmark-only telemetry

P8 adds three async diagnostic channels:

- p8-dynamic-draw.csv: slow dynamic draw calls (>=0.20 ms) with drawable kind/class/name, draw time, data variance, vertex/primitive counts, buffer count/bytes, dynamic-buffer bytes, and modified count.
- p8-dynamic-frame.csv: graphics-thread per-frame aggregate for instrumented dynamic draws.
- p8-deform.csv: CPU rig/morph deformation time with vertex/work-unit/buffer-byte metadata.

The existing p6-render-traversal.csv is extended with current double-buffered SceneView dynamic-object counts so dynamic-draw wait can be correlated with the actual number of dynamic RenderLeaves.

Callbacks are installed only when a P8 environment channel is enabled. Normal gameplay remains free of P8 draw callbacks and diagnostic output.

## Optimization sequence after attribution

1. Inventory the dynamic wait: determine how much of the wait is rig geometry, morph geometry, particles/effects, or other DYNAMIC drawables; separate CPU deformation time from graphics-thread VBO upload/draw time.
2. Dynamic geometry ownership: preserve OpenMW's existing double-buffering, but make buffer-slot ownership explicit. A slot may be Prepared, Submitted, InFlight, or Reusable. CPU deformation may only write a Reusable slot; draw submission publishes an immutable Prepared slot.
3. Decouple completion: replace the coarse frame-wide dependency for qualifying double-buffered OpenMW geometry with object/slot readiness. The main thread must not wait merely because a previous-frame dynamic drawable remains in flight when a different reusable slot exists.
4. Upload strategy: only after telemetry proves VBO upload is dominant, test orphan/subdata/persistent-mapped strategies on the private per-slot buffers. Do not mutate shared template buffers.
5. Expand by class: only migrate dynamic drawable classes whose ownership is explicit and whose telemetry shows material contribution. Unknown OSG dynamic drawables remain on the stock EndOfDynamicDrawBlock path.
6. Validate: compare median/p95/p99, >33/>50 ms, dynamic_draw_wait, dynamic counts, per-class draw cost, deformation CPU cost, visual/animation parity, and memory. Any slot hazard, visible animation desync, or first-use corruption rejects the experiment.

This phase deliberately does not disable EndOfDynamicDrawBlock globally.
