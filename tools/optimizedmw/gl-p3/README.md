# OptimizedMW GL-P3

Parent: `optimizedmw/gl-p1p2-repair-ffpb@41ff409f8ba8fce55f57eecb8ece45b196aa000b`.

P1A/P1B/P2 are accepted as the current baseline but remain revisitable.

## P3A — submission compaction

The existing ObjectPaging path already flattens transforms, shares state and merges compatible geometry. P3A therefore does not revive the rejected V3.19 `gl_InstanceID` instancing implementation or the rejected spatial topology.

The remaining narrow case addressed here is adjacent indexed primitive sets with identical primitive mode but different index storage widths. The old optimizer only combines equal primitive-set types, so an otherwise compatible merged geometry can retain separate UByte/UShort/UInt draws. The P3 switch promotes only the index storage width needed to combine adjacent draws, preserves index and primitive order, and leaves material/state/shader/shadow topology unchanged.

Setting: `[Cells] optimizedmw submission compaction` (default false).

The ObjectPaging integration applies P3A only to compile-time strong-quality chunks, never the P2 required-readiness weak pass, and only when the accepted world batching path is active. Existing render diagnostics report `p3_index_merges` without adding a new telemetry-only build.

## P3B — threading direction

Threading remains in scope. Prior threading rejections are not permanent bans; they reject the old mechanism.

The preferred next audit is coarse worker-local static preparation with deterministic publish, reusing the validated V3.25 ownership pattern. Do not mutate the live OSG graph from multiple workers, do not change the global OSG threading model, and do not reintroduce same-frame tiny-job fork/join. A threaded P3 candidate must preserve final render topology and demonstrate credible >1 ms or material tail headroom before promotion.
