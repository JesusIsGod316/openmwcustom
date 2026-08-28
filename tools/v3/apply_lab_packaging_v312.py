from pathlib import Path
import subprocess

# Apply the complete validated V3.11 stack first, then the V3.12 layers.
v311 = Path(__file__).with_name("apply_lab_packaging_v311.py")
exec(compile(v311.read_text(encoding="utf-8"), str(v311), "exec"),
    {"__file__": str(v311), "__name__": "__main__"})

v312 = Path(__file__).with_name("apply_v312_hitch_scheduler_fixed.py")
exec(compile(v312.read_text(encoding="utf-8"), str(v312), "exec"),
    {"__file__": str(v312), "__name__": "__main__"})

spatial = Path(__file__).with_name("apply_v312_spatial_batching.py")
exec(compile(spatial.read_text(encoding="utf-8"), str(spatial), "exec"),
    {"__file__": str(spatial), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
''')

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
    "v3.12 predictor mode",
    "mV312PredictorMode",
    "v3.12 lua precompile",
    "precompileConfiguredScripts",
    "V3.12 ETA Target Selected",
    "v3.12 spatial batch mode",
    "mV312SpatialBatchMode",
    "v312SpatialPrepared",
    "v312MergeGroups",
    "v312-mode66-control",
    "v312-eta-predictor",
    "v312-lua-precompile",
    "v312-combined-safe",
    "v312-spatial-batching",
    "v312-aggressive-horizon",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.12 exact generated-source snapshot missing marker: {marker}")

print("V3.12 exact generated-source snapshot refreshed after complete V3.12 layer.")
