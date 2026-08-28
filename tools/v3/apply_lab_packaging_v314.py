from pathlib import Path
import subprocess

# Apply the complete validated V3.13 stack first, then the V3.14 first-use/render
# preparation layer. Historical modes and the promoted V3.13 Mode75 remain intact.
v313 = Path(__file__).with_name("apply_lab_packaging_v313.py")
exec(compile(v313.read_text(encoding="utf-8"), str(v313), "exec"),
    {"__file__": str(v313), "__name__": "__main__"})

v314 = Path(__file__).with_name("apply_v314_lua_gpu_efficiency.py")
exec(compile(v314.read_text(encoding="utf-8"), str(v314), "exec"),
    {"__file__": str(v314), "__name__": "__main__"})

readme_path = Path(__file__).resolve().parents[2] / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

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
    "v3.14 lua dependency precompile mode",
    "mV314LuaPackagePrototypeReuse",
    "precompileConfiguredDependencies",
    "mV314PackagePrototype",
    "V3.14 Groundcover Compile Queued",
    "V3.14 queued active PostFX chain for ICO compile warmup",
    "v314-mode75-control",
    "v314-balanced",
    "v314-aggressive-prep",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.14 exact generated-source snapshot missing marker: {marker}")

print("V3.14 exact generated-source snapshot refreshed after complete V3.14 layer.")
