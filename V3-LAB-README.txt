OpenMW Custom V3.6 - Multipath Optimization
============================================

V3.6 makes the three validated optimizations normal custom-build behavior while retaining explicit per-feature disable switches. New runtime experiments and all deep diagnostics remain independently selectable and default off.

Normal V3.6 profile (default):
[V3]
v3.6 performance profile = true
v3.6 disable ram overdrive = false
v3.6 disable lua fast path = false
v3.6 disable coarse chunk occlusion = false

Effective normal behavior:
- RAM cache Overdrive with Balanced preload
- semantics-preserving V3.3 Lua idle-timer fast path
- visually validated V3.5 coarse paged-object/groundcover MSOC

The V3.6 profile deliberately overrides stale legacy false values. Set an individual V3.6 disable switch true to troubleshoot one proven optimization, or set the profile false to return to legacy per-setting control.

New default-off runtime experiments:
- v3.6 async gpu profiler: delayed GL timestamp queries for main world, each shadow cascade, water reflection/refraction, post-processing composite, and individual post-processing passes. It never calls glFinish and reads results only after GL_QUERY_RESULT_AVAILABLE.
- v3.6 far caster minimum pixels: OSG projected-size pruning only on the farthest shadow cascade. Near/middle cascades and ordinary rendering are unchanged.

New V3.6 attribution (environment/launcher selected, no runtime behavior change):
- v36-gpu-passes.csv: asynchronous per-pass GPU time with source/report frame and query latency.
- v36-lua-addscript.csv: sandbox, package, module, body, handler, and interface phases by container/script.
- v36-controller-build.csv: keyframe lookup, node map, controller clone/type, and source assignment by model.
- v36-source-residency.csv: largest cached source images, estimated source bytes, dimensions, mip levels, references, and cache recency. These are source-memory estimates, not exact driver allocations.
- v36-static-batching-audit.csv: repeated template groups and exact drawable/vertex topology before and after the existing ObjectPaging merge optimizer.
- Existing MSOC CSVs now include estimated paged children and groundcover instances skipped by successful coarse rejection.

Unified launcher V3.6 choices:
- 24: true custom baseline, including normal RAM cache and all V3 runtime optimizations off.
- 25: normal V3.6 performance profile.
- 26: normal profile plus asynchronous GPU pass profiler.
- 27: far-caster pruning isolated.
- 28: coarse MSOC isolated plus v2 skipped-work telemetry.
- 29: Lua/controller/residency/static-batching attribution only.
- 30: steady-state combined (normal profile + GPU profiler + far-caster pruning).
- 31: hitch combined (normal profile + deep attribution).
- 32: full V3.6 diagnostic combination.
- V3.6 choices support 4096, 6144, and 8192 shadow-distance tests. Shadow experiments support far divisors 1, 2, and 4; divisor 1 is recommended and divisor 4 remains a flicker-risk comparison only.

Safety/deferred architecture:
- Whole-far-cascade reuse remains default off and is not part of V3.6 combinations. A real static/dynamic far split needs another depth layer, RTT pass, texture bindings, and shader composition; it was not faked into this checkpoint.
- Groundcover already uses hardware instancing. ObjectPaging already groups identical templates and adaptively merges geometry. V3.6 measures that pipeline before adding a potentially conflicting general instancer.
- Foliage depth prepass is deferred because a safe alpha-test-only pass requires isolated render-bin/shader work; ordinary blended effects must not be changed.
- Lua activation/worker-barrier ordering and mutable controller instance state are unchanged.
- Residency is attribution-only; no NVML-driven eviction or destructive cache policy is present.

Benchmark invariant:
- The unified benchmark forces [Groundcover] density = 1.0 for the test so results remain comparable with the historical V3 baseline set.
- The launcher restores the user's original settings.cfg afterward, so a lower normal-play density is preserved outside benchmarking.

Double-click launchers:
- V3_Unified_Test.bat     : NORMAL benchmark. One run captures traversal/Lua + render/GPU/MSOC/shadow telemetry.
- V3_City_Frametime.bat   : compatibility alias for the same unified City-mode dataset.
- V3_Transition_Deep.bat  : special door/interior transition trace.
- V3_Render_Deep.bat      : special steady rendering-only diagnostic.

For a unified run: use the same outdoor save, hold the usual heavy outdoor view for about 45 seconds, then walk the same 2-3 minute exterior route across several cell boundaries and quit.

V3-applied-source.patch is the exact generated patch compiled by CI.


V3.7 hitch/shadow/residency delta
================================

Already promoted in the normal V3.6 performance profile on this branch:
- Visually validated 2-pixel far-cascade caster pruning, with [V3] v3.7 disable far caster pruning as a dedicated kill switch.

New V3.7 experiments (default off unless a V3.7 launcher choice selects them):
- v3.7 active event fast path: avoids empty global OnActive dispatch work while preserving event order/count and local activation semantics.
- Loaded-container empty-handler fast path: unconditional semantics-preserving shortcut; unloaded containers still materialize at the original semantic point.
- v3.7 companion keyframe preload: broadens preload-worker .kf warm-up from legacy x-prefixed NIFs to any preloaded NIF with a same-name .kf, deduplicated per preload item.
- v3.7 relaxed resource cache sweep: changes only ResourceSystem sweep cadence (default experiment value 5 seconds); cache expiry values and normal one-second behavior remain unchanged when disabled.
- Adapter-aware residency admission: combines process-local DXGI pressure with adapter-wide NVML pressure. Soft pressure caps NEW predictive outer-ring cell preloads to one per frame; hard pressure admits none. Required/current cells and already-preloaded refreshes are untouched. No GL objects are destructively evicted.
- v3.7 stabilize far shadow cascade: default-off orthographic far-cascade texel-grid snap, limited to at most half a texel per axis. It is not whole-map reuse and does not freeze actor/player shadows.

V3.7 unified launcher choices:
- 33: normal V3.7 candidate = V3.6 profile + active-event fast path + companion-keyframe preload + relaxed resource sweep + adapter-aware non-destructive preload admission.
- 34: active-event fast path isolated.
- 35: companion-keyframe preload isolated + hitch attribution.
- 36: V3.7 hitch combined + deep attribution + adapter-aware preload admission.
- 37: adapter-aware speculative preload admission isolated against the normal V3.6 profile.
- 38: far-cascade texel stabilization isolated at 6144 shadow distance.

The historical V3.6 choices 24-32 intentionally leave every new V3.7 experimental switch off so they remain valid same-executable comparison points.

Shadow architecture note:
- A true static/dynamic far-shadow split is still deferred. The current receiver shader has one sampler/transform per cascade, while the installed Rafael PBR overlay can replace compatibility shader resources after the normal resource copy. Adding a second static depth layer therefore requires an explicitly audited overlay/shader composition path; V3.7 does not fake this with stale whole-map reuse.


V3.8 traversal smoothness / GPU efficiency
==========================================

Primary objective:
- Improve normal exterior gameplay frame-time consistency and sustained FPS. Door/interior transition
  hitches are secondary unless a traversal optimization helps them incidentally.

World batching modes:
- 0: exact V3.7/upstream paging merge heuristic.
- 1 conservative: stronger merge pressure for immutable distant chunks.
- 2 moderate: much stronger distant merge pressure, repeated-template preference, post-transform vertex-cache optimization,
  and post-batch shared-state compaction through SceneManager's existing SharedStateManager.
- 3 aggressive: force every eligible distant template into OpenMW's existing merge pipeline, allow repeated active-grid
  templates to merge where the existing refnum/optimizer safety rules permit it, run both post-transform and access-order
  mesh optimization, and compact duplicate post-merge render state.

The implementation deliberately extends OpenMW's existing worker-side ObjectPaging merge optimizer. It retains the
existing eligibility filters, LOD selection, StateSet/material compatibility, alpha behavior, update-traversal exclusion,
refnum markers, and geometry merge implementation rather than bolting on a second incompatible batching system.

GPU residency modes:
- 0: V3.7 pressure admission/sweep behavior only.
- 1 conservative: under hard pressure, expire only very stale cache-only scene templates.
- 2 moderate: pressure-driven scene-template and paged-chunk reclamation.
- 3 aggressive: shorter pressure ages and cache-only groundcover reclamation too.

The residency path reuses GenericObjectCache reference-count safety. Live/external scene nodes refresh their timestamps
and cannot be removed. Long host-side NIF/image/keyframe caches remain untouched so the system continues to prefer the
32-GB host RAM budget while stale render graphs become reclaimable under pressure. V3.8 intentionally does not call
releaseGLObjects() on arbitrary expired scene graphs because their StateSets/textures may be shared with live objects.

Far-shadow modes:
- 0: proven V3.7 2-pixel far-cascade caster pruning.
- 1: 2.5-pixel conservative.
- 2: 3.5-pixel moderate.
- 3: 5-pixel aggressive.
Only the farthest cascade changes; near/mid cascades, map resolution, receiver shading and dynamic-caster semantics remain intact.

Incremental compile-pacing modes:
- 0: exact OpenMW/OSG behavior using the configured Cells target framerate.
- 1 conservative: retain target frame rate, cap compilation at 6 GL objects/frame and use a lower spare-time ratio.
- 2 balanced: compile target at most 45fps, cap 8 objects/frame, default 0.5 spare-time ratio.
- 3 aggressive preparation: compile target at most 36fps, cap 12 objects/frame, 0.6 spare-time ratio.
These modes aim to prepare newly paged VBO/state before first visibility without an unbounded compile burst.

Hardware object instancing QC note:
- Groundcover proves the renderer supports hardware instancing, but the project's Rafael PBR archive overlays the final
  object and shadow shaders during CMake. Stock-only instancing shader edits would therefore not be guaranteed to reach
  the actual runtime shader set. V3.8 does not ship unsafe partial object instancing; the stronger shader-independent
  merge path is used until the final PBR overlay can be transformed and shadow-tested as one atomic feature.

V3.8 launcher choices:
- 39 clean traversal baseline: proven V3.6/V3.7 stack, unvalidated companion keyframe preload and far stabilization off,
  all new V3.8 mechanisms off.
- 40/41/42 batching conservative/moderate/aggressive.
- 43/44/45 GPU residency conservative/moderate/aggressive.
- 46/47/48 combined conservative/moderate/aggressive (batching + residency + far-shadow + compile pacing).
- 49/50/51 isolated far-shadow conservative/moderate/aggressive.
- 52/53/54 isolated incremental compile pacing conservative/balanced/aggressive-preparation.

These are runtime modes inside one executable, not separate build cycles.


V3.9 front-loaded batching / VRAM headroom
===========================================

Priority correction from runtime validation:
- A large one-time startup/exterior-initialization hitch is acceptable if expensive preparation does not recur during
  ordinary traversal. Optimize post-start smoothness first; cold-load duration is no longer a primary acceptance metric.

Core implementation:
- Reuses CellPreloader -> Terrain::QuadTreeWorld::preload() for startup frontloading. QuadTree preload already walks every
  registered ChunkManager with compile=true, including ObjectPaging, so strong paged-object batches are created through
  an existing engine path rather than a second pager.
- Frontload 1: center + four cardinal future views.
- Frontload 2: 3x3 future-view block; intended normal candidate.
- Frontload 3: 5x5 future-view block; intentionally aggressive/slow startup.
- Frontloading runs only once per Scene lifetime. Normal cell-grid changes continue using the existing prediction path.

Preload-priority batching:
- QuadTree normal rendering asks ChunkManagers for chunks with compile=false, while explicit terrain/background preload
  uses compile=true. V3.9 uses that existing signal as a safety valve.
- Requested mode-2/3 strong batching is allowed on compile=true preload work.
- If prediction loses the race and a chunk is demanded synchronously with compile=false, V3.9 falls back to conservative
  mode-1 merge admission and merge-only cleanup for that miss. This bounds traversal-frame construction cost.
- ObjectPaging's ChunkId cache does not include the compile flag, so a strong batch successfully built by preload is reused
  unchanged by later rendering; the fallback is only for genuine on-demand misses.

Cheap strong batching:
- V3.8 mode-2/3 merge admission is retained on preload work so MERGE_GEOMETRY can preserve the measured draw/submission reduction.
- V3.9 separates expensive post-merge mesh ordering from merge admission.
- batch optimizer 0: exact V3.8 post-transform/pre-transform/shared-state behavior.
- batch optimizer 1: merge only.
- batch optimizer 2: merge + existing thread-safe SharedStateManager compaction.
- batch optimizer 3: merge + post-transform + shared-state; VERTEX_PRETRANSFORM is deliberately omitted.

Proactive residency:
- Adds earlier cache-only render-graph expiry in the adapter Soft-pressure band while retaining V3.8 hard-pressure policy.
- Live/external objects remain protected by GenericObjectCache reference counting.
- Source NIF/image/keyframe caches keep their long Overdrive lifetimes; host RAM remains the preferred backing store.
- No arbitrary releaseGLObjects() calls are introduced because paged StateSets/textures may be shared with live graphs.

V3.9 launcher choices:
- 55: V3.8-safe reference, equivalent to validated V3.8 Mode 46.
- 56: frontloaded strong batching, moderate V3.8 merge admission + 3x3 startup frontload + merge-only fast optimizer.
- 57: combined candidate, mode-2 batching + 3x3 frontload + shared-state compaction + proactive residency + visually-clean
      5px far-shadow pruning + aggressive compile preparation.
- 58: aggressive, mode-3 merge admission + 5x5 startup frontload + post-transform/shared-state + strongest residency.

Quality-control contract:
- Mode 55 must remain a rollback-equivalent reference.
- V3.9 startup work must execute only once per Scene lifetime and never become a recurring grid-change frontload.
- Any synchronous on-demand chunk miss while frontloading is enabled must use the conservative fallback rather than the
  strong forced-merge path.
- Existing ObjectPaging eligibility, animation/update-traversal exclusion, alpha/material/PBR compatibility, refnum semantics,
  LOD selection, occlusion data and shadow semantics remain authoritative.
- Do not promote hardware object instancing until the Rafael PBR main + shadow shader overlay is changed atomically and
  compiled/visually validated; V3.9 does not introduce a partial stock-shader instancing path.


V3.10 startup rebuild + preload-only post-transform promotion
=============================================================

Runtime evidence driving this build:
- V3.8 Mode 47: mode-2 strong batching + VERTEX_POSTTRANSFORM + shared state cut OSG draw traversal by about 33% and
  GPU draw by about 18%, but expensive optimization recurred during gameplay.
- V3.9 Mode 56: 3x3 startup frontloading reduced warm-route merge optimization by about 95% and preserved prepared
  chunks, but removing VERTEX_POSTTRANSFORM also removed essentially the entire large steady rendering win.
- V3.9 Mode 57: aggressive soft-pressure scene/chunk expiry discarded roughly a thousand prepared chunks and thousands
  of nodes for only about 41 MB less peak adapter use than Mode 56. V3.10 normal profiles therefore do not use that
  proactive whole-render-graph expiry strategy.
- The project accepts a substantially longer first exterior load when it reduces later traversal hitches.

Core V3.10 mechanism:
- Adds `v3.10 fresh initial object paging` and `v3.10 preload post-transform`; both default false.
- Ordering is explicit: any older TerrainPreloadItem is aborted and waitTillDone() completes first. Only after that
  quiescence point may the startup path clear the ObjectPaging CHUNK cache or enable the post-transform gate.
- Fresh startup rebuilding is independent from post-transform. This is intentional: Mode59 and Mode60 both rebuild the
  same startup ObjectPaging chunk set, while Mode60 alone enables VERTEX_POSTTRANSFORM/shared-state compaction.
- An earlier prediction can populate the same ChunkId before startup. Reusing that hit would bypass the intended startup
  work, so fresh mode clears only ObjectPaging's chunk cache. Externally referenced OSG nodes stay alive by ref counting;
  source scene-template/image/keyframe caches are not cleared.
- Post-transform implicitly forces freshness in engine logic as a safety net for manual settings, so an optimized startup
  cannot accidentally reuse an older merge-only chunk.
- The atomic optimizer gate is enabled only for the one-time synchronous initial frontload, before new preload positions
  dispatch worker tasks, and is cleared by RAII when that startup scope exits.
- `compile=true` by itself is deliberately NOT sufficient because later predictive/background preload also uses that path;
  later background chunks keep V3.9 strong merge admission but skip V3.10's expensive post-transform pass.
- V3.9's compile=false emergency fallback remains authoritative: an on-demand miss uses conservative mode-1 merge
  admission and merge-only cleanup, so the expensive locality pass cannot migrate back into traversal frames.
- V3.10 never enables VERTEX_PRETRANSFORM through this override.
- Existing eligibility, alpha/material/PBR compatibility, animation/update traversal exclusion, refnum semantics, LOD,
  occlusion and shadow behavior remain unchanged.

Lua QC decision:
- Mode56/57 exposed large first-cell Lua timer/event phases, but the obvious serialized-timer shortcut is not safe enough
  to promote yet. `processTimers()` can materialize an unloaded container, but the same active container is subsequently
  passed through local `onUpdate` handling in the same worker iteration. Pre-materializing a sandbox can also execute
  top-level/onLoad behavior. V3.10 therefore makes no Lua semantic change; the Lua path remains an audit target rather
  than risking script-order or save-state correctness.

V3.10 launcher choices:
- 56: unchanged runtime-validated V3.9 frontloaded strong-batching reference.
- 59: V3.9 Mode56 core knobs + fresh ObjectPaging startup rebuild, NO post-transform. This is the cache-rebuild control.
- 60: Mode59 + startup-frontload-only VERTEX_POSTTRANSFORM/shared-state. This is the FIRST causal A/B target against 59.
- 61: Mode60 + already visually-clean 5px far-shadow pruning; intended combined candidate after 59/60 establish causality.
- 62: Mode61 with 5x5 startup frontload instead of 3x3; intentionally expensive coverage experiment.

Acceptance/QC:
- Compare 59 vs 60 first. They must differ in the post-transform switch, not in fresh-cache behavior.
- Mode60 should recover a substantial share of Mode47's ~11-12ms draw / ~18-19ms GPU behavior while keeping later
  warm-route merge optimization near V3.9 Mode56's sub-second family rather than V3.8 Mode47's ~9.8s.
- Cold startup duration is not a rejection criterion by itself.
- Later compile=true predictive/background preload must NOT run the V3.10 post-transform pass.
- Any compile=false miss must remain conservative and must not run VERTEX_POSTTRANSFORM.
- No V3.10 normal profile enables V3.9 proactive residency expiry.
- No visual-quality setting is reduced; canonical groundcover remains 1.0 and the comparison shadow distance remains 4096.


V3.11 adjacent / rolling exact active-grid preparation
======================================================

Why this build exists
---------------------
V3.8 Mode47 proved that strongly prepared ObjectPaging chunks can reduce OSG draw
and GPU draw substantially, but paid hundreds of milliseconds of chunk construction
at ordinary cell crossings. V3.9/V3.10 correctly moved expensive work into preload
and preserved a cheap compile=false fallback, but the startup pattern used
stepCells=mHalfGridSize+1. That skips the immediate +/-1 grid-center states that
Scene::getNewGridCenter() normally enters during traversal.

V3.11 changes the target, not the safety model:
- Startup Mode2 frontload becomes current grid + eight immediate neighboring grid
  centers whenever V3.11 active-grid preparation is enabled.
- Normal Scene::preloadCells() already computes predictedPos and an exact future
  grid via getNewGridCenter(); V3.11 makes sure a newer grid-bound target is retained
  instead of silently dropped while an older TerrainPreloadItem is still running.
- The running task is not aggressively canceled. At most one newest different-grid
  target is retained and promoted after the current task completes. Predicted-pos
  jitter inside the same cell-grid bounds is ignored.
- Strong merge admission remains V3.8 batching mode2 on compile=true preload.
- V3.11 mode1 adds shared-state compaction only to compile=true activeGrid chunks.
- V3.11 mode2 uses that identical active-grid population and additionally applies
  VERTEX_POSTTRANSFORM. VERTEX_PRETRANSFORM is never promoted.
- compile=false active-grid misses remain V3.9's conservative Mode1 emergency path.
- V3.9 proactive whole-render-graph expiry remains disabled in all V3.11 profiles.

Causal runtime matrix
---------------------
56 = inherited V3.9 Mode56 reference.
59 = inherited V3.10 fresh-frontload control.
63 = V3.11 exact adjacent/rolling active-grid + shared state, NO post-transform.
     THIS IS THE FIRST V3.11 TEST.
64 = identical Mode63 target population + active-grid-only VERTEX_POSTTRANSFORM.
65 = Mode63 + already visually-clean 5px far-shadow pruning.
66 = Mode64 + already visually-clean 5px far-shadow pruning.
Do not use old 61/62 as the next test; they exist only for historical comparison.

Minimal proof counters
----------------------
ObjectPaging OSG stats:
- V3.11 Prepared Active Built
- V3.11 Prepared Active Hit
- V3.11 Prepared Active Resident
- V3.11 Demand Fallback
CellPreloader OSG stats:
- V3.11 Terrain Target Completed
- V3.11 Terrain Target Replaced
- V3.11 Terrain Target Promoted
- V3.11 Terrain Target Pending

Acceptance logic
----------------
Mode63 succeeds architecturally if prepared-active hits rise, demand fallbacks fall
sharply, crossing construction remains bounded, and steady draw/GPU approaches the
V3.8 Mode47 family without recurring Mode47-scale merge stalls. If draw/GPU does
not recover despite a high prepared-hit rate, run Mode64 before rebuilding; that is
the clean POSTTRANSFORM A/B on the correct live population.


V3.12 hitch / predictor / spatial-batching refinement
=====================================================

Control
-------
Mode67 is an exact V3.11 Mode66 behavior control. Every V3.12 mechanism defaults
off, so V3.12 can never silently revoke the validated renderer/paging foundation.

ETA/deadline predictor
----------------------
Mode68 keeps Mode66 rendering unchanged but estimates time to the exact
getNewGridCenter threshold from current velocity. If the next boundary is within
the configured lead window, that adjacent exact grid becomes the FIRST terrain
preload position. TerrainPreloadItem consumes positions in vector order, so the
most urgent strong active-grid POSTTRANSFORM work gets first claim on the worker.
Predictor mode2 can add the ordinary fixed-time predicted grid as a lower-priority
second horizon when it differs. Required demand work is never delayed and the
V3.11 Mode1 synchronous fallback remains unchanged.

Lua precompile
--------------
Mode69 populates LuaState's existing mCompiledScripts bytecode cache for all
configured top-level scripts during contentFilesLoaded(). It does not construct a
sandbox, run top-level script code, call onInit/onLoad, deserialize a container, or
change activation ordering. Errors during speculative precompile are warnings only;
normal upstream execution remains authoritative if that script is later used.
This is deliberately a semantics-safe first step; the larger ensureLoaded cost still
includes sandbox/body/onLoad/environment work that requires a stricter semantic audit.

Spatial prepared-active batching
--------------------------------
Mode71 changes only compile=true V3.11 Mode2 prepared active-grid chunks. Instead
of one chunk-wide mergeGroup, compatible mergeable instances are routed into four
2x2 chunk-local quadrants. Each quadrant receives the same merge optimizer,
VERTEX_POSTTRANSFORM, state sharing, refnum handling and compile policy. Geometry,
materials, transforms, alpha/PBR semantics and the emergency demand fallback are
unchanged. This intentionally trades a little possible draw-call consolidation for
finer child bounds/frustum culling. Setting 0 preserves exact Mode66 grouping.

Runtime matrix
--------------
67 = exact Mode66 control
68 = Mode66 + ETA/deadline exact-grid predictor
69 = Mode66 + safe Lua bytecode precompile
70 = predictor + Lua precompile combined safe candidate
71 = Mode70 + 2x2 spatial prepared-active batching
72 = Mode71 + predictor mode2 / 4-second lead second-horizon experiment

Mode72 may spend more background preparation and is intentionally aggressive. It
still performs no whole-graph or GL resource eviction and never disables the Mode1
demand fallback. It is not described as CPU-only GPU residency separation; true
CPU-prepared/GPU-resident decoupling remains later resource architecture work.


V3.13 deterministic ObjectPaging quality repair
===============================================

Why this build exists
---------------------
Repeated V3.12 runs proved a large renderer-family bifurcation with byte-identical
settings: strong runs retained roughly the V3.11 Mode66 draw/GPU family, while weak
runs fell back toward ~17 ms OSG draw / ~22 ms GPU. Source audit found a concrete
first-writer alias. ObjectPaging::ChunkId is (center,size,activeGrid), and inherited
getChunk() returned any cache hit without distinguishing a cheap compile=false
demand node from a strong compile=true active-grid node with shared-state and
VERTEX_POSTTRANSFORM. A weak node could therefore permanently satisfy later strong
preload requests for the same key.

V3.13 fixes the mechanism rather than increasing preload volume:
- Mode0 preserves inherited V3.12 first-writer behavior for controls.
- Mode1 records per-ChunkId preparation quality and lets compile=true active-grid
  preload rebuild a weak/lower-quality cached node on its existing worker thread.
- Demand calls never wait for repair. If a repair is already in flight they continue
  using the current cached node.
- GenericObjectCache already replaces entries under its mutex and stores osg::ref_ptr;
  installing a strong replacement therefore leaves existing readers safe while new
  lookups see the promoted node.
- Installation is strong-wins: a late compile=false build cannot overwrite a strong
  node that completed while demand was building.
- Mode2 additionally requires the V3.12 spatial prepared signature to match. This is
  intentionally experimental; normal V3.13 keeps spatial batching off.
- Full ObjectPaging cache clears also clear V3.13 quality/in-flight metadata.
- The validated V3.12 Lua bytecode precompile is promoted in the recommended profile.
  It still never executes script bodies, onInit/onLoad, or mutable container state.
- The V3.12 ETA predictor remains available in historical modes but is not enabled by
  V3.13 candidates because repeated testing did not reduce demand fallbacks.

Runtime matrix
--------------
73 = exact Mode66 foundation control (V3.13 quality/Lua/spatial/predictor off)
74 = deterministic ObjectPaging quality repair only
75 = quality repair + validated Lua bytecode precompile (RECOMMENDED FIRST TEST)
76 = strict quality signature + Lua precompile + spatial prepared batching experiment

Minimal proof counters
----------------------
- V3.13 Weak Cache Hit On Strong Prepare
- V3.13 Upgrade Built
- V3.13 Upgrade Installed
- V3.13 Upgrade Coalesced
- V3.13 Quality Entries
- V3.13 Upgrade In Flight
The paging CSV labels actual repair construction as object_chunk_quality_upgrade.

Acceptance
----------
The key criterion is repeatability: repeated identical Mode75 runs should remain in
roughly the 11-12 ms OSG draw / 18-19 ms GPU family instead of nondeterministically
landing near 17/22 ms. Visual/script behavior must remain clean, demand must remain
nonblocking, crossing p95/p99 must not regress, and 8 GB VRAM pressure must not rise
materially versus the Mode66/V3.12 foundation.


V3.14 Lua/GPU first-use efficiency layer
========================================

Foundation
----------
V3.14 preserves promoted V3.13 Mode75 as an exact in-binary control. It does not
change the deterministic ObjectPaging quality-repair architecture, the proven
non-spatial final batch topology, groundcover density, shadow distance, or Rafael
shader algorithms.

New mechanisms
--------------
1. Lua dependency bytecode prewarm
   V3.12 only compiled configured top-level scripts. V3.14 scans literal require()
   dependencies and compiles them into the existing bytecode cache without creating
   a sandbox or executing module/script code. Mode1 scans direct dependencies from
   configured scripts; mode2 recursively scans literal dependencies with a hard cap.
2. Lua static package prototype reuse
   ScriptsContainer API packages are read-only userdata. V3.14 can build one static
   package prototype per container and let sandbox-local module caches inherit from
   it, avoiding repeated insertion/allocation of the same immutable packages while
   preserving per-sandbox VFS module caches and dynamic common-package loader behavior.
3. Groundcover ICO preparation
   Groundcover already uses hardware instancing, but inherited getChunk ignored its
   compile argument. V3.14 feeds newly prepared groundcover VBO/state/program objects
   through OpenMW's existing IncrementalCompileOperation. Mode1 respects compile=true
   preload requests; mode2 also queues newly-created demand chunks asynchronously.
4. PostFX ICO warmup
   The active PostFX chain's pass/root state is collected into a temporary state-only
   graph and queued to the existing IncrementalCompileOperation. This changes when GL
   program/state objects are compiled, not Rafael HBAO/VAO shader math.

Runtime matrix
--------------
77 = exact promoted V3.13 Mode75 control
78 = balanced V3.14: direct dependency prewarm + package prototype reuse + preload-only
     groundcover ICO + PostFX ICO warmup (FIRST TEST)
79 = aggressive V3.14: recursive dependency prewarm + package prototype reuse + all-new
     groundcover ICO + PostFX ICO warmup

Important audit findings
------------------------
- First-activation Lua ensureLoaded necessarily executes top-level script bodies, so
  V3.14 does not move script body/onLoad/onInit execution earlier.
- Local Lua containers are already session-resident after first load; extending an
  unload timeout would not address the measured first-crossing spikes.
- ObjectPaging already sends strong prepared object state/programs to ICO; V3.14 does
  not duplicate generic world-object shader warmup.
- Groundcover already uses hardware instancing; V3.14 preserves it and closes only the
  missing background GL compile gap.
- Naive cache-template releaseGLObjects is unsafe because cached templates can share
  StateSets/textures with live clones. Resource-level VRAM demotion therefore remains
  a later ownership-tracked V3.14 mechanism rather than a blind graph release.


V3.15 render submission / traversal-tail layer
==============================================

Foundation
----------
Mode80 is an exact runtime copy of promoted V3.14 Mode79. V3.15 does not change
V3.13 strong-wins cache-quality behavior, Lua semantics, groundcover density,
shadow quality, or the external Rafael PBR shader algorithms.

New mechanisms
--------------
1. Premerge shared-state canonicalization
   Strong compile=true ObjectPaging merge groups can be passed through OpenMW's
   existing SharedStateManager before MERGE_GEOMETRY. OpenMW's merge comparator
   groups geometry by StateSet pointer identity, so canonicalizing structurally
   equal immutable StateSets before the merger exposes more render-equivalent PBR
   geometry without inventing a new material signature or weakening compatibility.
2. Temporary hierarchical packet premerge
   Strong prepared active-grid merge candidates may be distributed into 4 or 16
   spatial packets. Each packet receives only flatten/remove/merge work, then every
   result is recombined into ONE group before the existing Mode79-quality final
   optimizer and VERTEX_POSTTRANSFORM pass. No packet survives as final topology;
   this explicitly avoids the V3.12/Mode76 submission regression.
3. Adaptive ICO compile governor
   IncrementalCompileOperation's speculative per-frame budget is adjusted from the
   actual frame duration. Expensive frames collapse background GL compilation to
   one object and a conservative budget; cheap frames restore aggressive prewarm.
   Required/demand scene work and V3.13 cache-quality repair are never deferred.

Runtime matrix
--------------
80 = exact V3.14 Mode79 control
81 = Mode80 + premerge state canonicalization
82 = Mode81 + 4-packet temporary hierarchical premerge
83 = Mode81 + balanced adaptive ICO governor
84 = full balanced candidate: canonicalization + 4 packets + balanced governor
85 = aggressive candidate: canonicalization + 16 packets + aggressive governor

Deferred-but-promising upstream dev backport
--------------------------------------------
Current OpenMW master has a post-0.52 water path that removes the dedicated
refraction RTT and resolves opaque color/depth from the main render pipeline.
It is potentially a material GPU win for refraction-enabled water, but upstream
required follow-up fixes for underwater sorting and texture-unit handling. V3.15
tracks it as a separate future switch/backport rather than partially mixing it
into the first renderer-submission binary.


V3.16 general-play hitch suppression
====================================

Primary objective
-----------------
V3.16 freezes the promoted V3.15 Mode84 renderer/paging architecture and attacks
ordinary gameplay frametime spikes that occur while standing still, traversing,
or approaching actors. V3.15 logs showed repeated sound/script stalls plus
actor-local Lua bursts, smaller periodic resource cleanup pulses, and a separate
render-traversal synchronization lane.

Streamed sound head cache
-------------------------
V3.16 backports official OpenMW upstream commit
9ec49cfb4709cbfd8f14e97f5b9a558b71b8184f (sound-file head cache). Streamed
music/voice initialization can reuse the exact prefix/suffix bytes FFmpeg needed
on the first open, moving subsequent initialization reads to RAM. The feature is
configured through [Sound] 'head cache size'. V3.16 keeps the engine default at
0 MB so the control remains exact; experiment modes explicitly select 64/128 MB.
The upstream per-file safety ceiling remains 256 KiB.

Buffered SFX retention
----------------------
OpenMW Lua ambient playSound/playSoundFile calls use SoundBufferPool and
OpenALOutput::loadSound: the complete file is synchronously decoded and uploaded
the first time a nonresident SFX is requested. The stock decoded-buffer cache is
only 56/64 MB, which can churn in a large audio/mod setup and force later sounds
to repeat this work. Mode88 raises the decoded SFX cache to 256/384 MB and Mode89
to 512/768 MB. These are system/OpenAL Soft memory budgets, not texture/geometry
VRAM budgets. Mode87 deliberately leaves the user's existing buffer-cache limits
untouched so it remains a clean streamed-head-cache isolation run.

Loading-screen SFX metadata frontload
-------------------------------------
The stock SoundBufferPool lazily constructs its complete ESM sound-id/resource
maps on the first sound-id load. In a large content setup that one-time allocation
and insertion burst can therefore land on an ordinary gameplay frame. Modes88/89
opt into a default-off V3.16 frontload that performs only this immutable metadata
construction after content loading completes but before the startup loading screen
is dismissed. It does not read/decode sound files and does not create OpenAL
buffers. Modes86/87 leave the original lazy behavior untouched for clean control.

Aggressive first-use SFX predecode
----------------------------------
Mode89 additionally enables a 384 MB PCM predecode reservoir with one
idle-priority worker. While the startup loading screen is still active, the main
thread enumerates the known ESM sound resource names and hands the queue to that
worker. The worker performs only VFS/FFmpeg decode; it never calls OpenAL. This
keeps both ESM/resource enumeration and queue construction off the first ordinary
gameplay frame. OpenAL buffer creation/upload remains on the gameplay thread. If
a requested sound is already predecoded, synchronous storage and FFmpeg decode are
skipped. If it is not ready, the exact original synchronous load path runs
immediately, so playback semantics and correctness are preserved. Individual
speculative decoded entries are capped at 16 MB, the total ready reservoir is
bounded, and the worker waits for reservoir space rather than growing memory
without limit. Direct arbitrary playSoundFile paths that are not represented by
ESM sound records still use the original first-load path unless already known.

Idle-priority resource maintenance
----------------------------------
V3.7 already made ResourceSystem cache sweeps pressure-aware and moved deletion
work off the main thread, but those jobs still ran at the front of the same normal-
priority WorkQueue used by paging/preloading. Modes88/89 create one dedicated
resource-maintenance WorkQueue and permanently lower only that worker to idle
priority before ResourceSystem::updateCache. The V3.7 sweep cadence, cache expiry
rules, adapter-pressure policy, and emergency behavior are unchanged. Paging and
other gameplay-critical preload workers never inherit the lowered thread priority.

Asynchronous lab diagnostics
----------------------------
V3.16 removes synchronous CSV writes and periodic flushes from diagnostic
producer/gameplay threads. Each enabled CsvWriter queues complete rows into a
bounded in-memory queue and a dedicated writer thread performs disk output.
The queue is capped at 4096 rows; if diagnostic storage cannot keep up, V3.16
drops diagnostic rows instead of blocking gameplay. The producer uses a nonblocking
try-lock, so it drops a row rather than waiting behind the writer even on queue-lock
contention. The writer records the drop count at shutdown. This transport applies
to all V3.16 modes so A/B measurement is fair and the lab itself is less likely to
manufacture periodic frametime spikes. No writer thread exists for disabled streams.

Runtime matrix
--------------
86 = exact V3.15 Mode84 gameplay settings control, sound head cache 0 MB
87 = Mode86 + 64 MB streamed-audio head cache; existing decoded-SFX cache unchanged
88 = balanced V3.16 hitch candidate: Mode84 + audio head 64 MB + decoded SFX
     cache 256/384 MB + loading-screen SFX metadata frontload + dedicated idle
     resource maintenance + common nonblocking async diagnostics
89 = aggressive V3.16 hitch candidate: Mode84 + audio head 128 MB + decoded SFX
     cache 512/768 MB + loading-screen SFX metadata/queue frontload + 384 MB/1-worker
     first-use SFX predecode + dedicated idle resource maintenance + common async diagnostics

Development policy
------------------
Do not weaken Lua/script semantics to hide expensive mods. Do not defer required
scene construction or V3.13 strong-wins cache installation. OpenAL operations stay
on their existing thread/context. Resource expiry semantics remain unchanged.
Further render-thread changes must identify an actual synchronization point rather
than applying generic deferral. Diagnostic I/O must not become a gameplay-thread
hitch source.


V3.17 Lua/runtime hitch consolidation
=====================================

Primary objective
-----------------
V3.17 keeps the V3.16 balanced hitch architecture as its gameplay foundation and
attacks recurring Lua/runtime/event tails. The runtime matrix separates a stock-
LuaJIT control, Rubic0n runtime attribution, engine-side Lua/materialization work,
a combined candidate, and an aggressive SFX-predecode variant.

Engine Lua/event fast paths
---------------------------
Mode92+ adds conservative handler-presence checks before engine events construct
Lua wrapper arguments or resolve secondary RefNums that no callback can consume.
A loaded container whose current handler list is empty is a known negative and can
be skipped. An unloaded container is ALWAYS treated as "may have handlers" because
its top-level Lua code may legally choose a different handler table when it is
materialized again. V3.17 therefore does not persist negative handler-interest
state across unload/reload and does not suppress legitimate first materialization.
Lua-debug missing-object lookups are retained on otherwise-skipped event paths.

Lua -> sound conversion cache
-----------------------------
Mode92+ also keeps a small per-thread cache for the immutable conversions performed
at the Lua sound API boundary: textual sound IDs to ESM::RefId and file-name text
to normalized VFS paths. Each cache is capped at 4096 entries and keys larger than
512 bytes bypass it. Once full, new keys are parsed normally rather than clearing
the cache in gameplay. No SoundBuffer, OpenAL handle, decoded PCM, or mutable sound
state is cached here, and the normal play path never waits on predecode work.

Consolidated diagnostic writer
------------------------------
V3.16 correctly removed synchronous producer-thread CSV writes, but each enabled
CsvWriter still owned its own disk thread and flushed every 240 rows. V3.17 routes
all V3 CsvWriter instances through one shared bounded queue and one shared writer
thread. Producers use a nonblocking try-lock and drop diagnostic rows rather than
waiting. File open/header work is also queued to the writer so enabled diagnostics
do not open files on a gameplay thread. There are no periodic CSV flushes during
ordinary gameplay; streams drain and flush once at orderly shutdown. The shared
queue is bounded at 16384 items and dropped-row accounting remains per stream.

Runtime attribution matrix
--------------------------
90 = exact final V3.16 Mode88 gameplay settings + stock packaged LuaJIT.
91 = Mode90 + pinned sandboxed Rubic0n runtime only.
92 = Mode90 + V3.17 engine-side Lua/materialization optimizations, stock LuaJIT.
93 = Mode90 + Rubic0n + V3.17 engine-side Lua/materialization optimizations.
94 = Mode93 but inherits V3.16 Mode89 aggressive SFX retention/predecode budgets.

The launcher selects lua51.dll before OpenMW starts and restores the staged stock
runtime after the process exits. Direct/non-lab launches therefore stay stock by
default. Every profile records the selected DLL SHA256 and the packaged runtime
identity manifest.

Runtime safety policy
---------------------
The Rubic0n lane is source/revision pinned and sandboxed. Current OpenMW content.lua
is retained unless a real compatibility dependency is demonstrated. Experimental
Rubic0n allocator/finalizer behavior is not promoted merely because it exists;
unsafe semantic changes require explicit OpenMW/Sol userdata auditing first.


V3.18 renderer efficiency — internal resolution + NVIDIA Image Scaling
======================================================================

Purpose
-------
V3.18 begins the GPU-efficiency phase without disturbing the validated V3.17
runtime/audio work. The first layer separates native display/UI resolution from
the internal 3D scene resolution and provides a single bilinear presentation
stage. The second layer replaces only that final presentation stage with NVIDIA
Image Scaling (NIS) when selected.

Resolution architecture
-----------------------
The SDL/window size and main camera projection aspect remain native. The HUD camera
also stays at native output resolution. Scene color/depth/normals/distortion and
PostFX render targets use Video/render scale. PingPongCull pushes the internal
viewport only while the 3D scene is rendered. PingPongCanvas then executes the
entire PostFX chain at internal resolution and performs exactly one final native-
resolution presentation before the HUD/UI.

NIS provider
------------
NIS is pinned to NVIDIA Image Scaling SDK 1.0.3 source commit
35e13ba316c98eeecf16f37eae70ce88019911f6. The NVIDIA NVScaler algorithm and
coefficient tables are retained. NVIDIA's GLSL example uses Vulkan-style separate
texture/sampler objects; the V3.18 wrapper adapts only those texture-access macros
to ordinary OpenGL combined sampler2D objects and dispatches through OpenGL 4.3
compute/image-load-store using OSG GLExtensions.

NIS runs after the low-resolution PostFX chain and before native-resolution UI.
It writes a dedicated RGBA8 native-resolution output texture, then the existing
fullscreen presentation path samples that texture. A shader/extension/config
failure logs a warning and explicitly falls back to bilinear. NIS is never silently
emulated. Current implementation is FP32, 32x24 output blocks, 128 threads/group,
with adjustable Video/upscaler sharpness. Stereo/multiview remains native-only in
this first V3.18 implementation.

Causal modes
------------
95  = stock-Lua V3.17 control + 100% render scale.
96  = 85% internal scale + bilinear upscale.
97  = 77% internal scale + bilinear upscale.
98  = 66.7% internal scale + bilinear upscale.
99  = 85% internal scale + NIS, sharpness 0.20.
100 = 77% internal scale + NIS, sharpness 0.20. FIRST NIS TEST.
101 = 66.7% internal scale + NIS, sharpness 0.20.

Do not run the whole matrix automatically. The first useful rendering comparison
is 95 -> 97 to measure the raw resolution-dependent GPU ceiling, followed by
97 -> 100 to isolate NIS cost/quality at identical 77% internal resolution.


V3.19 CPU critical path P0 — focus temporal coherence + OSG scheduler matrix
============================================================================

Purpose
-------
V3.19 pivots back to the CPU/engine ceiling proven by V3.18. P0 preserves the
entire V3.18 renderer/NIS stack and adds two low-risk switchable CPU experiments
in one executable while PBR-aware texture-atlas/ObjectPaging integration is
prepared as the larger drawable-count optimization.

Focus temporal coherence
------------------------
OPENMW_V319_FOCUS_CADENCE remains the lab override for the per-frame GUI focus
refresh in normal gameplay. The stable gaming build exposes the same mechanism as
[V3] v3.19 focus cadence and defaults it to the promoted cadence 2 result. Cadence
1 reproduces the causal P0/V3.18 control and cadence 3 remains available for manual
experimentation. GUI mode still refreshes every frame. Activation/input object
queries are untouched, so this does not turn interaction itself into a half-rate
operation.

OSG scheduler matrix
--------------------
The launcher uses OSG's existing OSG_THREADING control rather than adding a new
engine threading system. Empty/unset is the exact automatic V3.18 control and is
the promoted stable scheduler policy. CullDrawThreadPerContext and
CullThreadPerCameraDrawThreadPerContext remain lab-only causal alternatives.

Stable gaming lineage
---------------------
This branch is generated from clean V3.19 P0 commit 8f94832770cbc97ca991e6a7f9ff83838f7afecc.
It intentionally excludes V3.19 P1/P1b static-instancing and compatibility-shader
changes. Rubic0n remains packaged for V3.17 attribution modes only; normal/direct
runtime selection remains stock LuaJIT. Existing native controls for the validated
V3.6 performance profile and V3.18 render-scale/NIS architecture are preserved
without duplicate settings.

Modes
-----
102 = V3.19 CPU control: native render, OSG automatic, focus cadence 1.
103 = focus cadence 2.
104 = focus cadence 3.
105 = CullDrawThreadPerContext, focus cadence 1.
106 = CullThreadPerCameraDrawThreadPerContext, focus cadence 1.
107 = CullDrawThreadPerContext + focus cadence 2.
108 = CullThreadPerCameraDrawThreadPerContext + focus cadence 2.

For ordinary gaming, no lab mode is required: native [V3] focus cadence 2 plus OSG
automatic is the stable policy. Lab modes remain available for causal reruns.


V3.20 CP1 — focus cadence refinement
====================================

V3.20 is layered over the exact clean V3.19 P0 stable gaming source. CP1 keeps
[V3] v3.19 focus cadence at the promoted default 2 and adds the disabled-by-
default [V3] v3.20 adaptive focus cadence option. Fixed mode is the exact P0
decision path. Adaptive mode forces a refresh when the main camera view or
projection matrix changes, while the fixed cadence remains a hard maximum
staleness bound for moving objects observed by a stationary camera. GUI mode
still refreshes every frame and activation/input queries remain untouched.

Modes 109-113 provide exact P0/off, fixed cadence2, fixed cadence3, adaptive2,
and adaptive3 causal selections. Aggregate counters are published only through
the existing resource-stat collection path; CP1 adds no per-frame file logger.

V3.20 CP2 — engine/Lua and pure sound-conversion fast paths
===========================================================

CP2 promotes the mature V3.17 handler-presence and pure ID/path-conversion
prototype into independent native settings. Handler checks remain unload-safe:
an unloaded container is always treated as a possible recipient. The sound cache
stores only deterministic ESM::RefId and normalized-path values in bounded TLS
maps; it never stores SoundBuffer, OpenAL, mutable world, or missing-resource
state. Exact P0 control modes force both mechanisms off. Aggregate checks, skips,
dispatches, hits, misses, and evictions use the existing resource-stat channel.

V3.20 CP3 — same-frame sound-query coalescing
================================================

CP3 optionally coalesces identical isSoundPlaying queries only within one engine
frame. Lua play/stop calls immediately invalidate cached query results. Listener
updates, playback mutations, one-shots, and frame-to-frame state remain normal-
rate. SceneManager already performs positive object-cache lookup before a real
scene_template_miss, so CP3 deliberately adds no redundant template cache and no
negative cache.

V3.20 Lua profiler recorder checkpoint
=======================================

The engine-side recorder is disabled until the in-game console command
`luaProfilerRecord start` is issued. `luaProfilerRecord stop` closes and flushes
the current CSV and `luaProfilerRecord status` reports its state/path. Recording
uses the existing LuaState count hook and per-script allocator attribution. It
adds an exact per-frame instruction accumulator beside the unchanged 30-frame
display average, so bursts are retained instead of averaged away.

The CSV is sparse: one `[frame_total]` row is written every frame and per-script
rows are written when raw operations are nonzero or active/inactive memory
changes. Omitted script rows therefore mean zero operations and unchanged
memory. The existing bounded nonblocking writer records dropped rows rather than
stalling gameplay. Normal V3.20 gameplay keeps the tracking allocator available
so a session can start at runtime; the instruction hook remains off unless the
stock profiler setting or the recorder enables it. Inherited and exact-P0 lab
modes force the recorder capability off, preserving stock allocator identity.

V3.20 CP6 — stock-semantic safe LuaJIT optimizer runtime
=========================================================

CP6 packages a second LuaJIT DLL built from the exact stable-P0 LuaJIT source
plus the checked-in metatable specialization, global-environment specialization,
and improved sinking series. It imports no Rubic0n allocator, GC/finalizer,
sandbox, builtin, ABI, or content.lua change. Root lua51.dll remains stock; the
lab launcher swaps the safe runtime only for modes 122 and 124 and restores
stock after exit. Modes 121/122 and 123/124 are same-settings causal pairs.


V3.21 CP1 — exterior completed-work admission pacing
=====================================================

V3.21 begins from the final normal V3.20 foundation. Mode 125 is an exact
behavioral control cloned from V3.20 Mode 123: stock LuaJIT, promoted focus
cadence 2, and the retained V3.20 gameplay stack. Mode 126 changes only the
new V3.21 fixed completed-work governor. Mode 127 retains the same fixed ICO
compile cap but adapts completed CompileSet merge admission from the previous
completed frame's wall time.

The governors do not throttle WorkQueue threads, terrain preload jobs, object
preparation, or prediction. Async producers continue to prepare useful work at
normal rate. The mechanism acts downstream at OSG's IncrementalCompileOperation:
it restores the configured target frame rate instead of V3.8's aggressive 36 Hz
compile target, caps GL compile objects per frame, and holds fully compiled
CompileSets in a FIFO before Viewer::updateTraversal().

Mode 126 admits a fixed bounded number of completed CompileSets per frame. Mode
127 changes only that merge budget. It uses the previously completed frame plus
a bounded EMA, guarantees a nonzero minimum service rate, accrues only bounded
service debt when pressure suppresses the fixed budget, and repays at most a
small configured amount on slack frames. The existing bounded-age supplement
still provides mandatory oldest-item escape. It never reacts to partial timing
from the frame currently being serviced.

Resource stats expose total completions/admissions/forced progress, deferred
depth and age, plus MODE 127 previous-frame time, EMA, adaptive merge budget,
debt, and debt repayment. This is substantive pacing plus attribution, not a
telemetry-only cycle.

Default settings keep the governor off. CP1 validation must compare Modes 125,
126, and 127 on the same save/mod/settings/route with the frame pacer off or
nonbinding. Promotion requires lower p95/p99 and fewer >33.3 ms / >50 ms frames
and render spikes without persistent deferred-queue growth, visible paging or
pop-in regression, or meaningful steady-state performance loss.


V3.21 CP2 — class-aware completion fairness/dephasing
=====================================================

Mode 129 keeps the exact Mode 125 V3.20 foundation and leaves the CP1 completion
governor OFF. It independently stages only fully completed ICO CompileSets and
services ObjectPaging, Terrain, GenericModel, and Unknown source classes through
bounded class-aware queues. WorkQueue threads, cell/terrain/model preload work,
prediction, object construction, and ICO compile production remain unthrottled.
Class identity lives only on transient derived CompileSet jobs. Mode 125 creates
ordinary OSG CompileSets through the original CP1 submission branches; neither
mode writes CP2 class userdata into cached or renderable scene objects.

When multiple source classes compete, CP2 uses bounded deficit round-robin with
nonzero quanta (ObjectPaging=2, Terrain=2, GenericModel=1, Unknown=1) and a
per-class burst cap to dephase same-class completion storms. When only one class
has completed work, that class may use the full service budget so fairness does
not become an artificial throttle. A separate global maximum-age escape admits
overdue work regardless of deficit or class burst cap, with bounded forced
service. Default settings use service=6 sets/frame, mixed-class burst=3,
max-age=6 frames, forced escape=2, and deficit cap=12.

Groundcover is intentionally not invented as an ICO class because its current
path does not submit CompileSets through ICO. Mode 128 remains unimplemented.
CP2 is orthogonal to the periodic ~24-26 ms other_ms investigation and to CP1
adaptive Mode 127. Resource stats expose class queue depth, seen/admitted totals,
per-frame service, active-class count, global deferred depth, and oldest age.



V3.21 CP3 — true full-body first person
=========================================

Mode 130 is Mode 129 plus a switchable full-body first-person view. The public
camera mode remains Camera::Mode::FirstPerson, so camera positioning, input,
saves, and existing first-person script checks retain their established
semantics. Internally, the player NpcAnimation selects VM_FirstPersonFullBody.

The full-body view uses the normal player skeleton, normal third-person body and
equipment parts, weapon controllers, the ordinary world field of view, and the
ordinary world-depth render path. It does not use Dot1st body parts, the native
first-person skeleton, the first-person-only FOV callback, the DepthClear render
bin, or the native first-person neck controller. Head, hair, and helmet parts are
suppressed in the owner view while the separate normal PRT_Neck part remains.
The camera is shifted 10 units forward relative to character yaw, placing the
viewpoint ahead of both the hidden head and the neck opening. The offset does not
follow pitch, so looking down cannot drive the camera into the torso. Switching
back to third person rebuilds the complete normal player normally.

The additive Lua API camera.isFullBodyFirstPerson() returns true only when the
public camera is FirstPerson and CP3 is active. Reanimation scripts can therefore
retain Rogue/native behavior for ordinary first person and use their third-person
animation path for the CP3 body without changing the public camera mode.

The feature defaults off in settings and Mode 129. Mode 130 enables it through a
process-local launcher environment override and retains Mode129 fairness tuning
unchanged. Use V3_Unified_Test.bat for normal CP3 validation. V3_City_Frametime.bat
remains a compatibility alias for the same City dataset.



V3.21 CP4 — full-body shadow and animation compatibility
=========================================================

Mode 130 remains the accepted CP3 owner-view control. Mode 131 is exact Mode130
plus CP4 shadow and animation compatibility. CP4 retains the real animated
head, hair and helmet parts on their normal player/update masks. A cull callback
hides marked parts only from SceneCam; shadow and water RTT cameras traverse
them normally. Gameplay intersection exclusion uses the same marker separately,
allowing a complete equipped silhouette without a second actor or skeleton.

The Lua sandbox loader records when a script requires openmw.animation. In
Mode131, camera.getMode() reports MODE.ThirdPerson during full-body first person
only to those animation-consuming sandboxes, so legacy animation frameworks
select their full-body overrides without a mod patch. Camera/UI-only scripts
continue seeing the physical FirstPerson mode. camera.getPhysicalMode() exposes
that exact physical mode explicitly, camera.isFullBodyFirstPerson() remains the
feature-state test, and camera.getAnimationMode() remains the modern explicit
animation-perspective query. Mode130 retains the prior behavior everywhere.



V3.22 CP1 — MSOC hot-path efficiency
=====================================

V3.22 starts from the runtime-accepted V3.21 CP4 head. Modes 132-134 remain
unused because those numbers were involved in canceled/audit-only V3.21 work.
Mode 135 is the final V3.21 Mode131 behavior control. Mode 136 adds only the
first low-risk V3.22 CPU render/cull optimization pack.

CP1 does not change occlusion thresholds, occluder geometry, visibility
decisions, exact-active ObjectPaging Mode2 batching/shareState/posttransform,
CP2 fairness, FBFP behavior, shadow behavior, or compile scheduling. It does
not attach any new UserDataContainer/DummyObject classification metadata.

The first mechanism caches the inverse main-camera view matrix and eye position
once when the active MSOC frame begins. Paged-object and groundcover coarse
callbacks reuse that immutable per-frame transform instead of independently
inverting the same camera matrix dozens of times per frame. If inversion fails
or CP1 is off, callbacks execute the original local-inversion path.

The second mechanism keeps each PagedOccluderCallback's resolved
PagedOccluderData after its first lookup on a given node. A node-identity guard
forces a fresh lookup if a callback is ever shared with another node, preserving
the original semantics while removing repeated UserDataContainer scans and
dynamic_casts on the steady cull path.

This checkpoint intentionally excludes unverified OSG-frustum shortcuts, MOC
buffer merging, off-thread proxy construction, new scenegraph metadata, and
occluder-aggressiveness changes. Those require separate API/lifetime proof
before they are allowed into a build.

Validation is Mode135 versus Mode136 with the frozen current mod/save/settings
cohort, native 1920x1080, AA4, groundcover 1.0, shadow distance 4096, OSG
Automatic, and Framepacer disabled/nonbinding. Preserve PBR, shadows, water,
groundcover, doors, and V3.21 FBFP compatibility. Primary metrics are OSG cull,
rendering traversal, wall p95/p99/tails, MSOC effectiveness, and VRAM.



V3.22 CP2 — ranked occluder efficiency ladder
==============================================

CP2 deliberately uses one executable for an innovation ladder with immediate
fallback. Mode 136 remains the CP1 control. Modes 137-140 all retain CP1 and the
final V3.21 CP2/CP3/CP4 foundation; only unpaged building-proxy budget policy
changes.

137: same 400-radius population, but eligible proxies consume the existing
     30000-triangle budget front-to-back instead of arbitrary cell-child order.
138: same 400-radius population, sorted by approximate projected coverage per
     raster triangle, with near-distance tie breaking.
139: Mode138 plus a clean 300-radius eligibility test. Proxy-detail scaling keeps
     400 units as its reference, so lowering eligibility does not inflate the
     triangle complexity of the previously eligible 400+ population. Distance
     stays 6144 and the global cap stays 30000.
140: Mode139 plus redundant-raster suppression. Before a ranked proxy is
     rasterized, the already-built full MSOC buffer may prove its AABB fully
     hidden by terrain or an earlier proxy. Only raster work is skipped; the
     building itself was already traversed and is never culled by this decision.

All modes preserve child traversal order, ObjectPaging Mode2/shareState/
posttransform topology, PBR/shadow/FBFP semantics, door handling, terrain-only
cell rejection, camera-inside exclusion, and the existing global triangle cap.
The rejected historical 250-radius / 8192-distance / 45000-triangle broadened
configuration is not revived.


CP2 eligibility-decoupling correction
-------------------------------------
Modes 139-140 do NOT lower the shared Camera/occlusion occluder min radius
setting. The established 400-unit boundary still decides which objects use the
large-owner path versus the normal full-buffer small-object visibility path.
CP2 separately considers 300-399-radius objects as potential occluders. Their
visibility result is recorded first against terrain + 400+ proxies, with their
own proxy absent. Visible mid-size proxies are then ranked/rasterized, but owner
traversal remains in the original Pass2 child order and consumes the recorded
visibility result without a second full-buffer test. This prevents self-
occlusion, preserves render traversal ordering, and avoids turning the 300-radius
experiment into a visibility-classification change.



V3.22 parallel architecture CP1 — immutable actor-avoidance prediction
=======================================================================

Mode 135 remains the exact final V3.21 control. Mode 141 changes only actor
collision-avoidance prediction. It snapshots positions, desired movement,
extents, speeds, target identity, and dead/alive state on the main thread;
worker jobs perform only pairwise numeric prediction; LOS, awareness checks,
steering writes, and turns remain on the main thread and commit in actor order.

The worker phase uses the existing bounded engine WorkQueue and its configured
preload thread count, plus the main thread as one lane. It activates only with
at least 12 live actors and at least one background lane. Jobs are inserted at
the front and joined before commit; there is no cross-frame state and no OSG,
physics-world, Lua, inventory, AI-sequence, or gameplay-event mutation on a
worker. If any activation precondition is absent, the exact legacy serial path
runs.

Mode 141 intentionally uses one immutable movement snapshot for the full
prediction batch. This replaces the legacy within-loop steering feedback with
a deterministic frame-consistent input set. It is therefore experimental and
must pass pathing/traffic correctness checks as well as frame-time gates before
promotion. Modes 136-140 remain dormant and Mode141 does not enable them.


V3.23 — parallel MSOC + frame-critical QoS groundwork
=====================================================

Mode 135 remains the exact final-V3.21 behavior control. V3.22 experimental
mechanisms are compiled but dormant in Modes 142-144 unless explicitly selected.

Mode 142 parallelizes the duplicated terrain raster into the full and terrain-only
MOC buffers. These are independent buffers, so the dedicated V3.23 worker never
shares a live write target with the cull thread. The worker is persistent and is
not the rendering/preload WorkQueue used by Mode141. If it cannot immediately
accept work, the terrain copy runs inline rather than queueing and blocking.

Modes 143 and 144 additionally batch exact-active PagedOccluderData meshes. The
main thread rasterizes alternating meshes into the live MOC while the dedicated
worker rasterizes the other half into a private same-resolution MOC. The worker
is joined only at this bounded private batch and the private buffer is merged
before traversal continues. Mode143 raises only paged-occluder range/budget by
1.5x; Mode144 raises them by 2x. Neither mode lowers CellOcclusion's individual
building radius or re-enables the rejected V3.22 300-radius experiment.

OFF isolation is mandatory: with OPENMW_V323_PARALLEL_MSOC_MODE unset/0, no
V3.23 worker is constructed, no worker MOC is allocated, no paged batch is
formed, and no V3.23 diagnostics are emitted. Mode144 ships compiled and default
off so aggressive coverage can be tested without contaminating the control.


V3.24 FRAME-JOB QOS / ZERO-WAIT MSOC
======================================
Mode135 remains the exact final-V3.21 behavior control. Mode145 enables only the
V3.24 QoS identity/infrastructure and is expected to be behavior-neutral. Mode146
uses the opportunistic reserved lane to rasterize the terrain-only MSOC buffer from
owned current-frame inputs. The main cull thread never waits for this job. If the
worker is busy, late, stale, or failed, terrain-only queries fail open (visible).
The existing full MSOC buffer still rasterizes synchronously and remains the
correctness floor. V3.23 1.5x/2x stronger occlusion tuning is not enabled by 145/146.

Actor/controller jobification was deliberately held out after the source audit: the
provably safe per-actor clone/remap boundary would require an immediate join for new
NPC construction and therefore cannot credibly reduce wall time. Broader actor setup
touches shared caches, world/mechanics state, animation sources, and live OSG. A
later actor build must batch multiple unpublished preparations or identify another
measured >=1 ms (preferably >=2 ms) ownership-clean kernel before worker execution.


V3.24 DEEP SELF-ACCOUNTING TELEMETRY
=====================================
The launcher now asks for deep telemetry OFF or ON for every mode. OFF and ON use
the identical binary. ON emits v324-deep-trace.csv with invasive scopes for
mechanics actor/object updates, physics preparation/simulation/commit, steady
animation, NPC rebuild/source construction, render-update work, FrameJobService
admission/execution/waits, and async MSOC copy/worker raster. Existing V3 paging,
streaming, Lua, resource, workqueue, render, postfx, MSOC-detail and OSG streams
remain available.

The deep writer self-accounts scope setup plus formatting, writer-lock wait, file
open, write, flush and bytes. Writer costs are carried on the following trace row
to avoid recursively timing the profiler. After the process exits the launcher
produces v324-profiler-overhead-summary.txt. The final writer operation is the only
direct writer cost not carried forward; the identical-mode telemetry OFF -> ON
comparison is the authoritative observer-effect bound and also captures cache,
scheduling and allocation perturbation that direct profiler self-time cannot.

Recommended diagnostic sequence: Mode135 OFF, Mode145 OFF, Mode145 ON, then
Mode146 ON. Use the same save/settings/mod cohort and route. Mode135 remains the
clean historical control; Mode145 OFF isolates V3.24 QoS infrastructure; Mode145
ON sizes broader future threading targets; Mode146 ON adds the zero-wait async
MSOC consumer and its worker-side telemetry.


V3.25 ENGINE OWNERSHIP BRIDGE - CP1
====================================
V3.25 is the final V3.x line. Mode149 is the V3.24-behavior same-binary control:
frame-job QoS infrastructure remains available but async terrain MSOC and V3.25
actor batching are OFF. Mode150 enables only batched NPC animation-source
finalization. It preserves animation-source insertion order and generic immediate
addAnimSource semantics while deferring repeated actor-wide
AssignControllerSourcesVisitor traversal until the ordered NPC source batch ends.

The V3.24 deep trace remains available for diagnostic runs, but V3.25 no longer
materializes the entire CSV with PowerShell Import-Csv after OpenMW exits. This
prevents multi-gigabyte traces from blocking automatic ZIP creation. Analyze deep
trace aggregation offline instead.

Mode151/152 are intentionally absent in CP1. The next checkpoint will only expose
parallel prepare/publish after Mode150 compiles and establishes how much safe work
remains. V4.0 renderer rearchitecture begins after V3.25 closes.


V3.25 ENGINE OWNERSHIP BRIDGE - CP2 / MODE151
===============================================
Mode151 inherits Mode150 actor-source batching, then enables only the safe NIF
keyframe-controller clone-preparation kernel on the V3.25 FrameCriticalJobGroup.
The group is queue-less, lazily creates up to two reserved workers, and makes the
caller participate through a shared coarse range cursor (16-controller threshold,
8-controller chunks). Worker results remain unpublished until the main thread
commits controller maps in deterministic source order.

The main thread still owns KeyframeManager/VFS resolution, NodeMap construction,
live node association, detectBlendMask traversal, missing-bone logging, AnimSource
publication, accumulation-root/blend-rule work and final AssignControllerSourcesVisitor.
Any OsgAnimationController or other non-NIF controller type forces that entire
source back through the historical serial osg::clone path. A worker exception also
discards partial unpublished results and retries the full clone set serially.

Mode151 writes one bounded v325-jobgroup-summary.csv at shutdown. It records group,
item, caller/worker chunk, fallback/failure and peak-worker totals without per-job
runtime file I/O. Deep telemetry remains OFF for performance A/B.
