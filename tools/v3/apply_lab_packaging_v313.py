from pathlib import Path
import subprocess

# Apply the complete validated V3.12 stack first, then the V3.13 deterministic
# ObjectPaging quality layer. Historical V3.12 controls remain default-off.
v312 = Path(__file__).with_name("apply_lab_packaging_v312.py")
exec(compile(v312.read_text(encoding="utf-8"), str(v312), "exec"),
    {"__file__": str(v312), "__name__": "__main__"})

v313 = Path(__file__).with_name("apply_v313_deterministic_quality.py")
exec(compile(v313.read_text(encoding="utf-8"), str(v313), "exec"),
    {"__file__": str(v313), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
    "v3.13 chunk quality mode",
    "mV313ChunkQualityMode",
    "mV313ChunkQualities",
    "mV313StrongUpgradeInFlight",
    "V3.13 Weak Cache Hit On Strong Prepare",
    "object_chunk_quality_upgrade",
    "v313-mode66-control",
    "v313-quality-repair",
    "v313-quality-lua",
    "v313-strict-spatial",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.13 exact generated-source snapshot missing marker: {marker}")

print("V3.13 exact generated-source snapshot refreshed after complete V3.13 layer.")
