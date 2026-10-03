# OptimizedMW: RAM and renderer architecture candidates

These are independently selectable implementation candidates for the OpenGL OptimizedMW track. Source compilation and focused fixtures establish particular correctness properties; user runs establish performance and visual acceptance. No candidate in this document has been promoted on the basis of its implementation alone.

The work began from `codex/optimizedmw-phase9-audited` at `716b5c4549cc49ca08326f8509c39d442ea6687c`. The changes described below are independently switchable candidates. Consult the installed package manifest for the actual executable, shader hashes, commit and source digest used by a run. This guide does not identify an executable by itself.

## Numeric launcher

Start `C:\OptimizedMW_Test\START-OptimizedMW-Test.bat` after the updated package has been installed. Every interactive choice uses numbers. In this document, **11 → 2 → 1** means category 11, candidate 2, then Normal performance. These numbers are navigation choices, not engine bit masks.

For each candidate, the final run-type choices are:

- **1: Normal performance** for performance comparisons.
- **2: Deep engine diagnostics**, followed by **1: Engine only** or **2: Engine + Windows system trace** for attribution.
- **0: Back** at either step.

Comparison labels record the intended save, route and frame-cap state. They do not load a save or set a frame cap. Confirm those conditions in the game. The run receipt records the effective RAM and renderer masks and the candidate dependencies.

| Category 10: RAM retention and compression | Normal-run path | Mechanism |
| --- | --- | --- |
| Retained RAM control | 10 → 1 → 1 | Current retained foundation; all five new RAM candidates off |
| Selective cold preload owner retirement | 10 → 2 → 1 | C1 |
| Shorter cold parsed NIF retention | 10 → 3 → 1 | C2 |
| Bounded idle collision shapes | 10 → 4 → 1 | C3 |
| Earlier headroom reclamation | 10 → 5 → 1 | C4 |
| Compressed cold image cache | 10 → 6 → 1 | C5 |
| All retention changes, compression off | 10 → 7 → 1 | C1–C4 |
| All RAM candidates including compression | 10 → 8 → 1 | C1–C5 |

| Category 11: Renderer architecture experiments | Normal-run path | Mechanism |
| --- | --- | --- |
| Retained renderer control | 11 → 1 → 1 | Current retained foundation; all seven new renderer candidates off |
| Exact shader variant readiness | 11 → 2 → 1 | G1 |
| Exact paging optimizer work reduction | 11 → 3 → 1 | G2 |
| Persistent opaque draw packets | 11 → 4 → 1 | G4 |
| Bounded terrain mip upload staging | 11 → 5 → 1 | G8; supplied CPU mipmaps enabled |
| Opaque multi-draw indirect + bounded command pipeline | 11 → 6 → 1 | G4 + G16 |
| Conservative GPU visibility + indirect pipeline | 11 → 7 → 1 | G4 + G16 + G32 |
| Frozen parallel actor deformation | 11 → 8 → 1 | G64 |
| Renderer candidates combined | 11 → 9 → 1 | G1–G64; supplied CPU mipmaps enabled |

Both retained controls preserve the current animation preparation/retention foundation. They do not disable every historical optimization or all ordinary RAM caching. Category **1 → 2** provides the separate original-feature control; changing to it changes the baseline and must be reported as a separate comparison.

For custom combinations, choose **8 → 1 → 19**, then **1: RAM** or **2: Renderer**. Toggle the displayed candidate numbers, return to the combination menu, and choose **16: Continue**. Choose **1: Original file I/O** for the first architecture comparisons, then choose Normal or Deep. The menu prints the selected combination before launch.

The menu resolves real dependencies: indirect drawing requires persistent packets; GPU visibility requires both; upload staging requires supplied CPU mipmaps. Disabling a prerequisite disables its dependants. This avoids a receipt claiming that an impossible combination was tested. Combined presets are available for later interaction tests; start with individual mechanisms so a gain or regression can be assigned to a cause.

## RAM mechanisms and the work they change

### C1: selective cold preload owners

The cell preloader indexes accepted request times and retires completed cold owners in bounded batches. Current demand, the animation ticket, near-future identities and a small recent-return set are protected. The protected sets retain identities rather than another resource graph. An indexing allocation failure disables this policy and preserves the original retention behavior.

This reduces unused retained ownership so useful preparation can fit within the existing pressure policy. It does not make a reconstructed controller or render graph free. The test must include a return along the same route: lower RAM is insufficient if return hitches become worse. Look for actual cold-owner retirement, lower retained ownership and fewer pressure-denied useful preparations. If nothing is retired, that run has not exercised the mechanism.

### C2: shorter cold parsed NIF retention

This shortens retention for cache-only parsed NIF resources rather than discarding live scene instances. It trades cold parsed-source residency for later parse work. Test a repeated route and actor/object revisits, and compare parse/cache-miss work as well as RAM. A smaller footprint with more visible parsing and worse return tails is a failed tradeoff.

### C3: bounded idle collision shapes

This bounds cache-only idle collision shapes while preserving externally held and recently demanded owners. It targets an independently retained resource family; it does not remove collision geometry needed by current physics. Test movement, revisits and collisions alongside memory. A later shape rebuild on the critical path can erase the benefit of reclaiming an idle shape.

### C4: earlier headroom reclamation

This coordinates retention contraction with the existing coherent physical/commit pressure sample. Contraction persists through recovery hysteresis. Invalid or degraded measurements do not grant speculative work permission. The existing byte-admission thresholds, hardware reserve floors and required-work fallback remain authoritative.

The hypothesis is that reclaiming cold owners before the critical floor is reached allows useful preparation to complete. Check retained bytes, available physical/commit headroom, pressure state, admissions and visible demand fallback together. A healthy-looking pool limit is a ceiling, not evidence that the pool is populated or that usable headroom increased. Repeated contraction/rebuild cycling or increasing return hitches falsifies the proposed benefit.

### C5: compressed cold images

This is an LZ4 tier for eligible static contiguous image buffers held exclusively by the cold image cache. Hot or externally held images remain decoded. It does not serialize arbitrary OSG graphs, pointers, controllers or driver texture objects.

Initial bounds are 256 MiB retained compressed storage, 16 MiB transient reservation, image size 64 KiB–4 MiB, at least 4 KiB of actual allocated saving, and at least 30 seconds of cold age. Poor compression is refused. Source-revision checks and winner publication prevent a restored stale image from replacing a newer source.

Compressed bytes, decoded restoration output and provisional/scratch peaks participate in the common accounting. Restoration reserves its output before publication. A busy codec uses ordinary loading instead of waiting behind another decoder. **A demand restore can still perform bounded decompression on the requesting loader**; this is not a promise that all restores occur ahead of demand or cost no frame time.

Check actual source/compressed bytes, compression and restore counts, poor-ratio refusals, temporary peaks, load/decode time and revisit hitches. Capacity that remains unused is not a RAM saving. Lower resident bytes accompanied by new demand decode spikes is not an acceptable promotion result.

## Renderer mechanisms and their boundaries

### G1: exact shader variant readiness and lifetime

Eligible producer seams capture the real canonical Program, immutable source/link recipe and effective imported define values. Candidate program-cache lookup separates incompatible attribute, fragment, uniform-block and sampler recipes. Compile jobs run at the existing GL owner and are independent of texture/geometry preparation.

Object coverage is the existing compile-enabled SceneManager producer path, including known Distortion startup defines. Terrain uses actual material-pass state; water requests its canonical program before normal-image work and cancels stale requests; composite preparation uses its currently pushed pass. Unknown custom imports, camera subgraphs and unknown/multiple contexts preserve normal demand fallback.

A compile request is not readiness. Readiness requires the linked canonical per-context program, real handle and no pending relink. Its first-consumption receipt must match the actual context object and ID, exact effective defines, program/source/release generation, attribute aliases and built-in uniform configuration. A receipt never substitutes a different program. Global define changes and GL release invalidate the relevant lifetime.

The intended change is fewer redundant or late visible-first-use links for eligible exact variants. Check requests, completed preparation, qualified first consumption, stale/mismatch/fallback counts and the visible program-apply/link span. If qualified preparation is absent or the hitch simply moves into a required frame's preparation phase, the intended benefit has not been established.

The global queue is bounded to 512 requests, 8 MiB of copied shader text and 4 MiB of copied metadata. Each recipe is bounded to 2 MiB of shader text, with at most eight receipts per program. These are not total shader RAM or driver VRAM bounds. Compilation/link initiation, allocation, sampler application and first actual GL use/draw may still stall. The implementation neither enables an asynchronous production shared context nor guarantees the absence of future shader stalls.

### G2: exact paging optimizer work reduction

The paging vertex-cache optimizer skips redundant score/heap work when its actual tuple has not changed. It preserves output triangle order, cancellation checkpoints and unsupported-primitive fallback. This removes repeated CPU bookkeeping in paging work; it does not eliminate all parsing, merging, uploads or loading work.

Check optimizer score reuses, unchanged heap updates, heap comparisons and optimizer duration for comparable geometry. A numerical operation reduction without shorter relevant work, or a scheduling displacement that worsens visible demand, is not yet a performance improvement.

### G4: persistent opaque draw packets

Object paging retains eligible static base-Geometry rows, their original owners, immutable bounds, material/draw identity and producer generations. Per-view input remains current. Supported rows avoid repeated visitor/state work while preserving the original OSG RenderLeaf and draw owner. No arbitrary transparency order is changed.

Rig/morph geometry, terrain, billboards, custom callbacks and changed producers use ordinary traversal. This is a narrow retained scene representation, not a complete persistent GPU renderer. Check built/eligible/submitted rows, avoided state pushes, changed-producer fallbacks and cull/traversal duration. Zero eligible submissions means a run cannot test its performance hypothesis. Visual acceptance must include water/reflection, shadow and alternate-view behavior.

### G8: bounded persistent upload staging for supplied terrain mips

The candidate stages eligible initial static alpha mip chains through persistently mapped PBO slots. Slot reuse requires its previous use to be complete. Admission uses the common byte ledger and a 24 MiB staging ceiling; each slot reserves its fixed capacity. Contention, unsupported storage, unavailable capacity or an incomplete fence preserve ordinary texture upload.

The scope is supplied CPU mipmaps, at most 2 MiB per staged chain, on the existing current context. This does not move general texture uploads to another GL thread. Existing allocated texture objects, image-unref behavior, ReadPBuffer images and unsupported layouts retain the ordinary OSG path. Every supplied mip must qualify before the candidate is accepted; failure returns to ordinary loading/subloading.

For an isolated staging comparison use **2 → 9 → 1** (supplied mips only) as A, and **11 → 5 → 1** as B. Comparing the retained no-mips control directly to 11 → 5 measures both mip supply and staging, so it cannot attribute a gain to staging alone.

Check actual staged chains/bytes, reuse, admission/busy/fallback counts, supplied-mip use and upload/first-draw spans. The 24 MiB figure is allocated staging capacity, not all texture residency or physical driver VRAM. Timeout-zero fence polls and CPU try-locks avoid those waits; GL allocation/map/upload initiation can still block. A candidate that engages only during loading cannot by itself explain an ordinary walking FPS change.

### G16: opaque multi-draw indirect and bounded command ownership

Eligible static opaque indexed triangles use a bounded indirect command representation while ordinary OSG material/state ownership remains in place. Long supported primitives can be divided into fixed-size command clusters without changing their index order. The applied program must carry the exact shipped-source certificate, or use fixed function; custom lookalikes and side-effect programs are not accepted through a substring heuristic. Command slots are reused after actual completion; unknown programs, changed sources, unsupported GL support or busy capacity fall back.

Every draw rechecks the current original material and OSG's already applied blend, alpha-test and stencil-test modes, including inherited override/protected resolution. In-place transparency/sorted material changes or an enabled unsupported mode use original primitive boundaries. These three const cache lookups add no driver queries or allocations; they rely on the existing OSG state owner and do not certify external code that changes raw GL state without updating OSG. The native fixture requires exact pixel parity and positive original fallback for these mutations, then proves indirect drawing resumes after restoring opaque state.

The command pipeline is bounded ownership inside the existing context. It is not a whole-frame producer/consumer renderer and does not remove viewer barriers. Common pressure/byte admission applies before new candidate backing is retained, and pending retirement continues to consume capacity. A released context cannot create replacement storage with the same ticket while its former backing is still pending; exact buffer retirement or context discard must clear that generation first. Logical accounting bounds are not proof of the driver's physical memory footprint.

Use **11 → 4** as A and **11 → 6** as B to isolate indirect drawing from its persistent-packet prerequisite. Inspect actual indirect calls, command counts, fallback/slot pressure, CPU submission and GPU duration. A previously merged single draw can remain one indirect draw with extra setup; fewer CPU calls are not guaranteed.

### G32: conservative GPU visibility

Compute work can suppress indirect command instances for clusters wholly outside conservative X/Y clip planes. It does not perform occlusion culling, Z rejection or a quality/LOD change. The effective projection and the exact shipped shader-template/include closure must satisfy the projection certificate. Changed/custom shader sources and invalid certificates fail open. Required command/storage visibility barriers and original GL state restoration remain in place.

The private compute program is still linked on its first eligible use, and private buffer storage is allocated at its existing GL owner. Those driver calls may stall even though slot/fence readiness polling is nonwaiting. G1's exact application-program preparation does not prepare this internal compute program or guarantee a stall-free first visibility dispatch.

Use **11 → 6** as A and **11 → 7** as B. Check visibility dispatches, visible/total commands, shader-certificate fallback and GPU/CPU frame cost. A view with no newly rejected work can only measure the overhead. Visually inspect screen edges, reflections and shadows; missing geometry is a correctness failure regardless of FPS.

### G64: frozen parallel actor deformation

This freezes numeric matrices and source ranges, proves disjoint writes to the private CPU geometry slot, then splits sufficient work across the existing frame-critical worker group. It joins before dirtying/publishing the pose. Every view consumes completed output. Unsupported, overlapping or malformed ranges use the original path; a partially failed group is fully recomputed before publication.

This is **CPU parallel deformation**. It is not GPU skinning, additional Lua VMs or permission to remove a render ownership barrier. Small actors may not qualify, and the snapshot/dispatch/join cost can exceed the saved arithmetic.

Check actual eligible publications, deformed vertices, worker groups, fallback counts and rig/deformation duration. Test actor-dense scenes and all relevant views, as well as an empty/light control scene. An improved worker span with a larger frame join or changed pose is a failed result.

## Private shared-context prerequisite: fixture only

`p9-shared-context-preparation` creates two real native OSG GL contexts with explicit sharing. A dedicated producer owner uploads a bounded static RGBA8 texture, flushes its completion fence, and the consumer polls with zero timeout. A consumer rebind establishes shared-object visibility; fixture-only readback checks every byte. Final consumer-use fences retain ownership through retirement. Shutdown stops/joins the producer before final retirement.

The fixture checks exact shader readiness, another effective define variant, stale release generation, actual sharing-context identity and context closure while another member of the share group survives. Its logical payload bound is not a physical VRAM measurement. Native context closure is not simulated graphics-device removal or proof of recovery from a driver reset.

There is **no private-context game launcher switch**. This establishes a prerequisite for a future eligible resource queue; it is not that queue's production integration. General asynchronous resource creation, off-thread shader compilation and a complete frame pipeline remain future work requiring independent lifetime, compatibility and performance validation.

## Audit resolutions and verification evidence

The implementation preserves control and demand fallback instead of treating queued preparation as completed work.

- Shader recipes use actual imported defines and link inputs, and qualify the consuming context and generation. A request, pointer retention or equal OSG context ID alone is insufficient.
- Indirect splitting and GPU projection certification pin the shipped raw template and complete include closure, retain consumed source/identity and reject modified lookalikes. Names and substring resemblance cannot establish projection equivalence or side-effect safety.
- Upload release is deferred when its actual context is not current. A context-owned retirement manager keeps GL names, fences and accounting through completion or actual context discard.
- Each upload allocation has a unique retained identity, including while its former slot allocation is pending. Reusing a slot cannot alias its earlier byte charge. The frame retirement flush uses try-lock and timeout-zero fences.
- Indirect storage retains its capacity through raw-buffer deletion and prohibits reallocation for a context with pending backing. Repeated release/reinitialize cycles cannot manufacture replacement capacity or fill retirement records by reusing one charged ticket.
- Recycled OSG textures with allocated immutable storage follow ordinary subloading. A later-mip GL failure cannot be accepted merely because the base level has the expected dimensions. Image-unref and ReadPBuffer semantics preserve fallback.
- Cold compression requires exclusive immutable payload ownership, source-revision validation and restoration peak admission. Moving a payload between tiers does not manufacture spare memory.
- Parallel deformation freezes inputs and joins before publication; indirect commands retain completion/retirement ownership. Neither mechanism treats barrier removal as proof of safety.

Confirmed results reported by the integrating worker on 2026-10-02: the full MSVC game build **PASS**; shader production translation-unit compilation **PASS**; `p9-shared-context-preparation` native MSVC build and real GL execution **PASS**; repaired `p9-upload-pool` MSVC build and extended real GL execution **PASS**; final GPU pending-storage/certificate fixture **PASS**. The upload fixture exercises a reused immutable texture, an injected later-mip failure, non-current deferred release, pending same-slot replacement, and staging/common-ledger limits. The GPU fixture covers held-pending release/control fallback, actual retirement and reinitialization across repeated cycles, as well as exact source certification.

The final **12 focused ProgramVariant GTests PASS**, with exact-variant and GPU flags set to 1 in a fresh process. They cover define resolution/restoration, unknown custom imports, source/link generation and binary/size rejection, factory recipe separation, global queue quota, real ShaderManager baseline certification and consumed/template-include mutation rejection. The real GL cases also cover same-PCP relinking with sampler recovery and bounded readiness slots. Consult the final package validation receipt for the exact invocation and remaining check results. A successful full game build and focused fixtures do not establish runtime performance, all driver/hardware paths, or in-game visual acceptance.

## Performance and visual test order

1. Verify the installed package and run receipt; keep the same assets, save, view, route and high-enough frame-cap state. Avoid a cap that conceals movement stutter. Keep a cold initial run separate from the warm comparison.
2. Test one mechanism in **Normal A → B → B → A** order: A is its matching prerequisite control; B adds the candidate. Use the same approximately 45-second heavy fixed view and the same 2–3-minute walking route. For RAM policies, continue with the same reverse/return route so cold-cache rebuilding is measured.
3. Compare the complete comparable gameplay body, loading separately, frame median and tails, local/relative spikes and frame-to-frame variation. A single absolute-millisecond threshold or average FPS can miss poor pacing. Verify capture validity and mechanism engagement before interpreting any difference.
4. Run a matching Deep engine trace when the candidate produces a repeatable change or an unexplained regression. Deep has diagnostic overhead and query behavior; compare its attributed work, not its FPS against Normal. Add Windows tracing when runnable/waiting time, I/O, scheduling or driver boundaries remain unresolved.
5. Combine only individually understood candidates, starting with pairs and their matching controls. Re-test Normal ABBA after composition; lower memory, earlier preparation, command generation and worker concurrency can interact negatively. The all-candidates preset is an interaction experiment, not a recommended default.

For G8, G16 and G32 use the prerequisite controls specified above. For G1, G2, G4 and G64, 11 → 1 is the matching retained renderer control. For individual C1–C5, 10 → 1 is the matching retained RAM control. Keep original file I/O selected until an explicit I/O interaction is being tested.

Record visual or crash failures before any promotion. No observed FPS gain justifies missing geometry, a stale texture, an incomplete pose, corrupted output, or an invalid capture. A no-effect result with zero accepted work means the relevant eligibility/admission seam needs investigation; it is not evidence of successful optimization.

## Historical boundaries

The authority order remains Shared Project Context Archive for decisions/history, Benchmark & Log Ledger for exact run metrics, and this Git source for implementation. The latest explicit corrections supersede older hypotheses.

The cached control/index supports the trajectory from V2 and early V3 cache foundations through later CPU, retained-resource and rendering experiments. The full `07 LEGACY_ARCHIVE`/V2 body and complete raw benchmark ledger were not available in the bounded material used for this implementation. This is an **indexed V2 continuity audit**, not a claim that every V2 event or exact historical benchmark was reread. No new benchmark values are inferred from the index.

Past aggressive RAM/Overdrive retention improved reuse in some conditions but did not eliminate controller reconstruction, renderer work or all return hitches. Overdrive-off in the enabled V3.6 profile selects Normal RAM caching; it does not turn off all caching. These candidates revisit typed cold ownership and headroom under the current measured workload, without silently restoring rejected whole-graph VRAM eviction.

Event 159's broad static prewarm was quarantined after an NVIDIA access violation. G1 changes the relevant conditions: exact immutable program-only jobs, existing current GL owner, no broad graph cull/camera/texture/FBO application, and exact consumption guards. It does not restore that broad prewarm. Resource preparation and shader linking remain distinct from guaranteeing later GL calls will not stall.

Generic adaptive scheduling that starved required work is not restored. Temporal snapshots do not permit removing an ownership barrier without a producer/consumer proof. The VulkanMW track remains separate; these candidates make no claim of implementing its architecture or GPU deformation.

Historical read sources for this implementation were the control block cached under `Validation/NextMove_4cdba7b86e_20261002`, `ContextArchive-00.txt` and `ContextArchive-01.txt` cached under `Validation/Repairs_ea40c58d_20261002`, and the earlier architecture/RAM history audit reports. These local evidence copies are not edits to the authoritative archive.

## Implementation references

The source defines the actual shipped mechanism and limits. Useful primary references for auditing its underlying contracts are:

- [OSG 3.6.5 Program implementation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osg/Program.cpp): per-context programs, define variants and link status.
- [OSG 3.6.5 IncrementalCompileOperation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osgUtil/IncrementalCompileOperation.cpp): the existing compile owner and ambient program work.
- [OSG 3.6.5 State implementation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osg/State.cpp): inherited define and state resolution.
- [OSG 3.6.5 Texture implementation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osg/Texture.cpp) and [Texture2D implementation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osg/Texture2D.cpp): allocated texture-object reuse and ordinary subloading.
- [OSG 3.6.5 GraphicsContext implementation](https://raw.githubusercontent.com/openscenegraph/OpenSceneGraph/OpenSceneGraph-3.6.5/src/osg/GraphicsContext.cpp): context release and native closure.
- [OpenGL 4.6 Core specification](https://registry.khronos.org/OpenGL/specs/gl/glspec46.core.pdf): shared-object visibility, synchronization, indirect commands and storage lifetimes.
- [LZ4 public API](https://github.com/lz4/lz4/blob/dev/lib/lz4.h): the cold buffer codec; package/build dependency versions remain those of this project.
