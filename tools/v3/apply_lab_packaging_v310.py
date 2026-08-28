from pathlib import Path
import subprocess


# Apply the complete, already-runtime-validated V3.9 stack first, then layer the
# narrow V3.10 startup rebuild/post-transform promotion on top. Keeping V3.10 as a
# small layer makes rollback and generated-diff review straightforward.
v39 = Path(__file__).with_name("apply_lab_packaging_v39.py")
exec(compile(v39.read_text(encoding="utf-8"), str(v39), "exec"),
    {"__file__": str(v39), "__name__": "__main__"})

v310 = Path(__file__).with_name("apply_v310_posttransform_preload.py")
exec(compile(v310.read_text(encoding="utf-8"), str(v310), "exec"),
    {"__file__": str(v310), "__name__": "__main__"})

# QC correction: compile=true also covers later predictive/background preload.
# Gate the expensive locality pass to V3.9's one-time synchronous startup
# frontload and keep fresh-cache rebuilding separately switchable for a clean A/B.
v310_gate = Path(__file__).with_name("apply_v310_initial_frontload_gate.py")
exec(compile(v310_gate.read_text(encoding="utf-8"), str(v310_gate), "exec"),
    {"__file__": str(v310_gate), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
''')

# The inherited V3.6 packager captures V3-applied-source.patch before the V3.9
# and V3.10 layers are applied. Refresh the snapshot here, after the FINAL layer,
# so the artifact installed beside the executable and uploaded by preflight is an
# exact representation of the source tree that CI actually compiles.
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

# Self-check the exact-source artifact rather than trusting the live generated
# tree alone. This catches ordering regressions where a future layering change
# accidentally snapshots V3.8/V3.9 before V3.10 is applied.
patch_text = patch.decode("utf-8", errors="replace")
for marker in (
    "v3.10 fresh initial object paging",
    "v3.10 preload post-transform",
    "mV310FreshInitialObjectPaging",
    "v310DoFreshFrontload",
    "v310PreloadPostTransform",
    "mV310InitialFrontloadActive",
    "V310InitialFrontloadScope",
    "clearV310InitialObjectPagingCache",
    "v310InitialFrontloadScope.activate",
    "v310-fresh-frontload-control",
    "v310-posttransform-3x3",
    "v310-combined-candidate",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.10 exact generated-source snapshot missing marker: {marker}")

print("V3.10 exact generated-source snapshot refreshed after final layer.")
