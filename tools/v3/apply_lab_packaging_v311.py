from pathlib import Path
import subprocess


# Preserve the complete validated V3.10 stack, then add the narrow V3.11 layer.
v310 = Path(__file__).with_name("apply_lab_packaging_v310.py")
exec(compile(v310.read_text(encoding="utf-8"), str(v310), "exec"),
    {"__file__": str(v310), "__name__": "__main__"})

v311 = Path(__file__).with_name("apply_v311_adjacent_active_grid.py")
exec(compile(v311.read_text(encoding="utf-8"), str(v311), "exec"),
    {"__file__": str(v311), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
''')

# Refresh the exact generated-source snapshot after the final V3.11 layer.
ROOT = Path(__file__).resolve().parents[2]
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
for marker in (
    "v3.11 active grid prepare mode",
    "mV311ActiveGridPrepareMode",
    "v311ExactActiveGrid",
    "mV311PendingTerrainPreloadPositions",
    "V3.11 Prepared Active Built",
    "V3.11 Demand Fallback",
    "v311PreparedActive",
    "v311-exact-active-shared",
    "v311-exact-active-posttransform",
    "v311-combined-shared",
    "v311-combined-posttransform",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.11 exact generated-source snapshot missing marker: {marker}")

print("V3.11 exact generated-source snapshot refreshed after final layer.")
