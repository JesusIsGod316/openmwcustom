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

P3B adds a separate opt-in `[Cells] optimizedmw parallel template prefetch` path (default false, minimum-template threshold 16). It applies to compile-time distant chunks, including P2 required-readiness work, but never active-grid demand. A cheap deterministic prepass collects unique model paths; one persistent bounded helper and the existing paging worker warm the already thread-safe SceneManager template cache, then the original authoritative ObjectPaging loop performs grouping, radius decisions, analysis, merge and publication unchanged.

The helper is deliberately not a global OSG threading-mode change and does not mutate the live scene graph. Only one P3 two-way helper operation can exist engine-wide at a time; contention or worker-creation failure falls back to the original serial path rather than queueing. The helper thread is persistent after first use so large distant loads do not pay a fresh OS thread launch on every chunk.

P1/P2 ownership is preserved explicitly. `SpeculativeScope` and `PagingWorkScope` are thread-local, so P3B captures their contexts, installs child scopes on the helper, propagates cancellation/phase, and credits helper retained-byte estimates back to the parent speculative job after join. Targeted tests cover these contracts.

Promotion still requires runtime evidence: useful reduction in distant paging/template-load tails without steady-frame, memory, compatibility or accepted-P1/P2 regressions. Worker count stays at one helper until evidence justifies more.


## P3C — semantic premerge

This revisits the visually-safe V3.15 state canonicalization idea through a different combined mechanism. The current final ObjectPaging optimizer groups geometry by StateSet identity. Equivalent states that become shared only after the optimizer are already too late to unlock a merge. P3C optionally canonicalizes state immediately before the final strong-quality merge, where P3A can then also collapse mixed-width index submissions inside the newly larger geometry.

Setting: `[Cells] optimizedmw semantic premerge` (default false). The P3 path excludes P2 required-readiness work. Existing V3.15 behavior remains independent and unchanged.

## P3D — immutable distant command cache

The modified OSG optimizer historically switches successfully merged geometry to VBOs and explicitly disables display lists because VBOs consume less driver memory. That was a reasonable memory-first choice, but current accepted captures are draw-side CPU limited enough to justify retesting the opposite trade for immutable distant world geometry.

Setting: `[Cells] optimizedmw distant display lists` (default false). The experiment is limited to compile-time strong-quality distant chunks, excludes active-grid and P2 required-readiness paths, and is disabled for multiview. It changes only the submission storage/cache strategy after a successful merge; geometry, indices, state, shaders, shadows and render order are unchanged. Promotion requires a CPU-draw win large enough to justify any measured VRAM increase.

## P3E+ open lanes

P3 is not limited to prior V3 mechanisms. Higher-risk/high-upside follow-ups under active audit include: shared-context background OpenGL compile/upload, cached immutable per-template analysis, worker-local ObjectPaging construction/merge planning with deterministic publish, and capability-gated persistent multi-draw/indirect static submission. The SDL/OSG window path already understands shared OSG context IDs, but actual SDL GL object sharing must be made explicit before a background compile context can be trusted. Prior rejected instancing, spatial batching, or OSG threading experiments may be revisited through materially different implementations informed by their failure evidence.
