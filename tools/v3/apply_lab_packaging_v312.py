from pathlib import Path
import subprocess

# Apply the complete validated V3.11 stack first, then V3.12.
v311 = Path(__file__).with_name("apply_lab_packaging_v311.py")
exec(compile(v311.read_text(encoding="utf-8"), str(v311), "exec"),
    {"__file__": str(v311), "__name__": "__main__"})

v312 = Path(__file__).with_name("apply_v312_hitch_scheduler.py")
exec(compile(v312.read_text(encoding="utf-8"), str(v312), "exec"),
    {"__file__": str(v312), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.12 hitch / predictor refinement — first implementation layer
==============================================================

Control
-------
Mode67 is an exact V3.11 Mode66 behavior control. V3.12 mechanisms default off.

ETA/deadline predictor
----------------------
Mode68 keeps Mode66 rendering unchanged but estimates time to the exact
getNewGridCenter threshold from current velocity. If the next boundary is within
the configured lead window, that adjacent exact grid becomes the FIRST terrain
preload position. TerrainPreloadItem consumes positions in vector order, so the
most urgent strong active-grid POSTTRANSFORM work gets first claim on the worker.
Mode2 predictor support can add the legacy fixed-time predicted grid as a lower
priority second horizon when it differs; the initial safe candidate uses mode1.
Required demand work is never deferred and V3.11 Mode1 fallback is unchanged.

Lua precompile
--------------
Mode69 populates LuaState's existing mCompiledScripts bytecode cache for all
configured top-level scripts during contentFilesLoaded(). It does not construct a
sandbox, run top-level script code, call onInit/onLoad, deserialize a container, or
change activation ordering. Errors during speculative precompile are warnings only;
normal upstream execution remains authoritative if that script is later used.
This is deliberately a semantics-safe first layer; larger ensureLoaded savings need
additional source-safe preparation beyond bytecode compilation.

Modes
-----
67 = exact Mode66 control
68 = Mode66 + ETA/deadline exact-grid predictor
69 = Mode66 + safe Lua bytecode precompile
70 = predictor + Lua precompile combined safe candidate
71/72 are reserved for later V3.12 spatial-clustering/CPU-horizon work and are not
present yet in this checkpoint.
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
    "v312-mode66-control",
    "v312-eta-predictor",
    "v312-lua-precompile",
    "v312-combined-safe",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.12 exact generated-source snapshot missing marker: {marker}")

print("V3.12 exact generated-source snapshot refreshed after first implementation layer.")
