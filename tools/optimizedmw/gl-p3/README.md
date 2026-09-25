# OptimizedMW GL-P3

Parent: `optimizedmw/gl-p1p2-repair-ffpb@41ff409f8ba8fce55f57eecb8ece45b196aa000b`.

P1A/P1B/P2 are accepted as the current baseline but remain revisitable.

## P3A — submission compaction

The existing ObjectPaging path already flattens transforms, shares state and merges compatible geometry. P3A therefore does not revive the rejected V3.19 `gl_InstanceID` instancing implementation or the rejected spatial topology.

The remaining narrow case addressed here is adjacent indexed primitive sets with identical primitive mode but different index storage widths. The old optimizer only combines equal primitive-set types, so an otherwise compatible merged geometry can retain separate UByte/UShort/UInt draws. The P3 switch promotes only the index storage width needed to combine adjacent draws, preserves index and primitive order, and leaves material/state/shader/shadow topology unchanged.

Setting: `[Cells] optimizedmw submission compaction` (default false).

The ObjectPaging integration applies P3A only to compile-time strong-quality chunks, never the P2 required-readiness weak pass, and only when the accepted world batching path is active. Existing render diagnostics report `p3_index_merges` without adding a new telemetry-only build.

## P3B — bounded distant-template prefetch

Threading remains in scope. Prior threading rejections are not permanent bans; they reject the old mechanism.

Fresh accepted P1/P2 capture analysis found that the largest sampled distant `object_chunk_template_analysis` event was dominated by serial `scene_template_miss` work: roughly 1.864 seconds of a 2.103 second sampled chunk, across 361 unique template misses. The `AnalyzeVisitor` itself is therefore not the primary target.

P3B adds a separate opt-in `[Cells] optimizedmw parallel template prefetch` path (default false, minimum-template threshold 16). It applies only to strong compile-time distant chunks, never active-grid demand and never the P2 required-readiness weak pass. A cheap deterministic prepass collects unique model paths; one bounded helper and the existing paging worker warm the already thread-safe SceneManager template cache, then the original authoritative ObjectPaging loop performs grouping, radius decisions, analysis, merge and publication unchanged.

The helper is deliberately not a global OSG threading-mode change and does not mutate the live scene graph. Only one P3 two-way helper operation can exist engine-wide at a time; contention or thread-creation failure falls back to the original serial path rather than queueing.

P1/P2 ownership is preserved explicitly. `SpeculativeScope` and `PagingWorkScope` are thread-local, so P3B captures their contexts, installs child scopes on the helper, propagates cancellation/phase, and credits helper retained-byte estimates back to the parent speculative job after join. Targeted tests cover these contracts.

Promotion still requires runtime evidence: useful reduction in distant paging/template-load tails without steady-frame, memory, compatibility or accepted-P1/P2 regressions. Worker count stays at one helper until evidence justifies more.
