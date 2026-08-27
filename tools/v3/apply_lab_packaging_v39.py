from pathlib import Path


# Apply the complete, already-validated V3.8 stack first, then layer V3.9 on top.
v38 = Path(__file__).with_name("apply_lab_packaging_v38.py")
exec(compile(v38.read_text(encoding="utf-8"), str(v38), "exec"),
    {"__file__": str(v38), "__name__": "__main__"})

v39 = Path(__file__).with_name("apply_v39_frontloaded_batching.py")
exec(compile(v39.read_text(encoding="utf-8"), str(v39), "exec"),
    {"__file__": str(v39), "__name__": "__main__"})

v39_priority = Path(__file__).with_name("apply_v39_preload_priority.py")
exec(compile(v39_priority.read_text(encoding="utf-8"), str(v39_priority), "exec"),
    {"__file__": str(v39_priority), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
''')
