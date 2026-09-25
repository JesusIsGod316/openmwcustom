# OptimizedMW GL-P3 integration repair

Parent tested Windows artifact: `optimizedmw/gl-p3-experimental-windows@839f29d9b826a4a34842ae09421d07ac0376945f`
(run `36109012918`, successful).

P1A/P1B/P2 remain the accepted baseline. All repaired P3 mechanisms remain opt-in and require new runtime validation.

## Runtime evidence that drives this revision

The first hardware matrix showed:

- P3A submission compaction produced zero surviving index merges because the accepted batching/optimizer path had already collapsed that primitive structure. P3A is parked and the repaired launcher forces it off.
- P3B parallel template prefetch produced the only clear positive runtime signal, reducing frame/render tails and render-handoff events, but large fixed-half jobs could still leave 100-200+ ms prefetch tails.
- P3C semantic premerge fired on the latency-sensitive active-grid strong path and correlated with severe tail regressions.
- P3D display-list caching and P3F shadow batching never received a fair runtime test: their gates required distant + compile + non-readiness work, while P2's optional strong-upgrade replay covered active-grid entries only.
- P3E's original white-fill rule targeted a vertex-color mismatch that did not occur in the tested Project Cyrodiil/PBR workload.

## Integration repair

P2 required-readiness publication remains unchanged.

The existing cancellable P2 optional optimization tail can now request an explicit P3 strong-quality variant for distant ObjectPaging chunks. ObjectPaging quality metadata includes a P3 optional-feature mask, so a cached required-readiness chunk cannot falsely satisfy a requested P3 C/D/E/F upgrade. Cache invalidation clears the corresponding quality side state.

Only enabled P3 optional features request distant replay. If C/D/E/F are all off, the accepted P2 route does not gain the extra distant rebuild work.

## P3A — parked

Setting retained for compatibility: `[Cells] optimizedmw submission compaction`.

The benchmark launcher forces it off. Source remains available for rollback/history, but this revision does not spend runtime or build budget trying to force an already-solved primitive pattern.

## P3B — dynamically balanced bounded template prefetch

Settings:

- `optimizedmw parallel template prefetch` (default false)
- `optimizedmw parallel template prefetch workers` (1-2, default 2)
- `optimizedmw parallel template prefetch min templates` (default 16)

P3B remains limited to compile-time distant template acquisition, including P2 required-readiness work. The caller participates in the work and up to two persistent helpers may participate. There is still only one P3 helper operation engine-wide at a time, no FIFO/background queue, and contention or worker creation failure falls back safely.

Large jobs use dynamic atomic work claiming instead of fixed halves. This directly targets the measured long-tail imbalance where one half could finish while the other was blocked on expensive template misses. Smaller jobs stay at one helper to avoid unnecessary contention.

Each helper inherits `SpeculativeScope` and `PagingWorkScope`, including cancellation and phase state, and retained-byte estimates are credited back to the parent job. Targeted tests exercise two-helper ownership, cancellation, one-time work distribution, and persistence.

## P3C — semantic premerge moved off the latency-sensitive route

Setting: `optimizedmw semantic premerge` (default false).

P3C no longer runs on active-grid strong preparation. It is restricted to the cancellable distant optional P3 upgrade. This preserves the original semantic-state canonicalization experiment while moving its cost away from the path that produced the hardware hitch regression.

It remains experimental; moving it does not erase the negative first-run evidence.

## P3D — immutable distant command cache, now reachable

Setting: `optimizedmw distant display lists` (default false).

P3D now executes only on the cancellable distant optional P3 variant and remains disabled for multiview. It can therefore receive a real runtime test without delaying P2 required readiness. The targeted QC includes a display-list promotion case that does not depend on P3A.

Promotion still requires a meaningful draw-CPU win and acceptable 8 GB VRAM behavior.

## P3E — material-ignored color stream normalization

Setting: `optimizedmw normalized static packets` (default false).

The first rule attempted to synthesize white per-vertex color arrays only for a narrow missing/present mismatch that never appeared at runtime. The repaired rule instead removes an existing primary color stream only when the attached `SceneUtil::Material` explicitly declares `VertexColorModes::None`. Such data is semantically unused by that material.

The change runs only on private static geometry in the cancellable distant optional path. Geometry whose material consumes vertex color is unchanged. Targeted tests require both the safe strip/merge case and the preservation case.

## P3F — distant static shadow proxy batching, now reachable

Setting: `optimizedmw shadow static batching` (default false).

P3F now runs in the same cancellable distant optional P3 variant rather than waiting for a non-existent distant strong phase. The conservative v1 eligibility rules remain intact for the first fair hardware test.

Diagnostics now preserve the distinction between:

- direct static geometry candidates,
- eligible geometry,
- state rejections,
- geometry/layout rejections,
- completed shadow batches,
- batched source indices.

A chunk with one eligible drawable but not enough to form a batch is therefore no longer reported as if no geometry qualified.

## Render-handoff attribution

`optimizedmw render handoff attribution` remains a low-overhead diagnostic. When enabled alongside the render CSV, `renderingTraversals()` stalls of at least 20 ms are emitted as `p3_render_handoff` events. The repaired benchmark launcher enables this in every mode, including Control.

## Benchmark matrix

The installed launcher exposes:

1. CONTROL
2. P3B one helper
3. P3B two helpers
4. P3C optional semantic premerge
5. P3D display-list cache
6. P3E ignored-color stripping
7. P3F shadow batching
8. core candidate: P3B two helpers + P3D + P3F
9. all repaired candidates

Use the same save, heavy fixed view, walking route, frame-cap state, mod state and quality settings for every run. The launcher backs up/restores `settings.cfg` and writes a ZIP containing the effective settings, identity, render/paging/shadow diagnostics, OSG statistics, OpenMW log and process-memory samples.
