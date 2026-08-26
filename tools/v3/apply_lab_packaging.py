from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

# V3.4 layer: broadened-MSOC lab, far-shadow divisor 4, persistent cell-occluder storage,
# and unified one-run telemetry. V3.5 intentionally builds on the known-good generated tree.
v34 = Path(__file__).with_name("apply_v34_occlusion_shadow_lab.py")
exec(compile(v34.read_text(encoding="utf-8"), str(v34), "exec"), {"__file__": str(v34), "__name__": "__main__"})

# Keep the cache-path rebinding call in sync with the V3.4 Objects::setOcclusionCuller signature.
v34_compile_fix = Path(__file__).with_name("apply_v34_compile_fix.py")
exec(compile(v34_compile_fix.read_text(encoding="utf-8"), str(v34_compile_fix), "exec"),
    {"__file__": str(v34_compile_fix), "__name__": "__main__"})

# V3.5 final generated layer: coarse paged/groundcover MSOC, bounded far-cascade reuse with
# dynamic casters when explicitly selected, Lua first-materialization attribution, and benchmark invariants.
v35 = Path(__file__).with_name("apply_v35_coarse_occlusion_shadow_lab.py")
exec(compile(v35.read_text(encoding="utf-8"), str(v35), "exec"),
    {"__file__": str(v35), "__name__": "__main__"})

# Final V3.5 safety pass: complete OcclusionCuller type in groundcover.cpp and clamp actor/player
# far-cascade reuse to interval 2 (at most one rendered frame of stale dynamic far-shadow data).
v35_safety = Path(__file__).with_name("apply_v35_safety.py")
exec(compile(v35_safety.read_text(encoding="utf-8"), str(v35_safety), "exec"),
    {"__file__": str(v35_safety), "__name__": "__main__"})

# Install the V3 helper launchers and exact applied-source snapshot beside the
# runtime executable. This script only runs through the V3 harness, so upstream/default builds are unaffected.
cmake = ROOT / "CMakeLists.txt"
text = cmake.read_text(encoding="utf-8")
marker = "# V3 OPTIMIZATION LAB PACKAGING"
if marker not in text:
    text += r'''

# V3 OPTIMIZATION LAB PACKAGING
if(WIN32)
    install(FILES
        "${CMAKE_SOURCE_DIR}/tools/v3/launchers/V3_Unified_Test.bat"
        "${CMAKE_SOURCE_DIR}/tools/v3/launchers/V3_City_Frametime.bat"
        "${CMAKE_SOURCE_DIR}/tools/v3/launchers/V3_Transition_Deep.bat"
        "${CMAKE_SOURCE_DIR}/tools/v3/launchers/V3_Render_Deep.bat"
        "${CMAKE_SOURCE_DIR}/tools/v3/launchers/V3_Lab.ps1"
        "${CMAKE_SOURCE_DIR}/tools/v3/v3_trace_to_chrome.py"
        "${CMAKE_SOURCE_DIR}/V3-applied-source.patch"
        "${CMAKE_SOURCE_DIR}/V3-applied-source-stat.txt"
        "${CMAKE_SOURCE_DIR}/V3-LAB-README.txt"
        DESTINATION "."
    )
endif()
'''
    cmake.write_text(text, encoding="utf-8", newline="\n")

# Validate the fully transformed source tree, then preserve exactly what the build will compile.
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)

stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

readme = r'''OpenMW Custom V3.5 - Coarse Occlusion / Shadow Cache Lab
=========================================================

This build contains independently selectable diagnostics and experiments. Normal OpenMW behavior remains unchanged unless an experiment is selected.

Known-good retained optimizations:
[Cells]
ram cache mode = overdrive
ram cache overdrive preload = balanced

[Lua]
v3.3 idle timer fast path = false

[Shadows]
v3.3 far cascade resolution divisor = 1
v3.5 allow dynamic far cascade reuse = false

[Camera]
v3.4 broaden occlusion = false
v3.5 coarse chunk occlusion = false

V3.5 experiments are disabled by default. The unified launcher adds:
- 19: coarse chunk MSOC. Uses tight AABBs to reject whole paged-object chunks and groundcover chunks while retaining the baseline 400/6144/30000 individual-occluder policy.
- 20: bounded one-frame far-cascade reuse + divisor 4. Far cascade may reuse exactly one prior rendered frame when the existing camera/texel-drift guard permits it; actor/player shadow settings stay enabled.
- 21: coarse chunk MSOC + proven divisor-4 far cascade.
- 22: coarse chunk MSOC + proven Lua idle-timer fast path.
- 23: full V3.5 combined experiment: coarse chunk MSOC + Lua fast path + divisor-4 far cascade + bounded one-frame far reuse.

V3.4 option 15 remains available for comparison but is not part of V3.5 combinations because testing showed that simply doubling individual occluder work produced little GPU benefit.

Benchmark invariant:
- The unified benchmark forces [Groundcover] density = 1.0 for the test so results remain comparable with the historical V3 baseline set.
- The launcher restores the user's original settings.cfg afterward, so a lower normal-play density is preserved outside benchmarking.

Double-click launchers:
- V3_Unified_Test.bat     : NORMAL benchmark. One run captures traversal/Lua + render/GPU/MSOC/shadow telemetry.
- V3_City_Frametime.bat   : compatibility alias for the same unified City-mode dataset.
- V3_Transition_Deep.bat  : special door/interior transition trace.
- V3_Render_Deep.bat      : special steady rendering-only diagnostic.

For a unified run: use the same outdoor save, hold the usual heavy outdoor view for about 45 seconds, then walk the same 2-3 minute exterior route across several cell boundaries and quit.

New V3.5 diagnostics:
- MSOC telemetry separates whole paged-chunk and groundcover-chunk tests/rejections from ordinary AABB tests.
- v35-lua-loads.csv records slow first-time ScriptsContainer materialization and breaks it into preparation, interface/package setup, script sandbox/body execution, init/load handlers, timer restoration, heap setup, and tracker work. This is attribution-only and does not change Lua ordering or activation semantics.
- Existing shadow telemetry reports far-cascade update/reuse counts, far-map dimensions/divisor, and drift guard behavior.

V3-applied-source.patch is the exact generated patch compiled by CI.
'''
(ROOT / "V3-LAB-README.txt").write_text(readme, encoding="utf-8", newline="\n")

print("V3.5 Lab packaging/preflight completed successfully.")
