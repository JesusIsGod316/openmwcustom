from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

# V3.4 is the final generated layer: broadened MSOC, aggressive far-shadow option,
# persistent cell-occluder storage wiring, and unified one-run telemetry.
v34 = Path(__file__).with_name("apply_v34_occlusion_shadow_lab.py")
exec(compile(v34.read_text(encoding="utf-8"), str(v34), "exec"), {"__file__": str(v34), "__name__": "__main__"})

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

readme = r'''OpenMW Custom V3.4 - Unified Performance Lab
==========================================

This build contains opt-in diagnostics and experiments. Normal OpenMW behavior is preserved unless an experiment is selected.

Recommended 32 GB baseline:
[Cells]
ram cache mode = overdrive
ram cache overdrive preload = balanced

[Lua]
v3.3 idle timer fast path = false

[Shadows]
v3.3 far cascade resolution divisor = 1

[Camera]
v3.4 broaden occlusion = false

V3.4 experiments are disabled by default. The unified launcher exposes:
- 15: broadened MSOC (250 min-radius, 8192 max occluder distance, 45000 triangle budget, safe large-object full-buffer rejection)
- 16: aggressive far-shadow divisor 4 (far cascade quarter width/height; near/middle unchanged)
- 17: broadened MSOC + proven Lua idle-timer fast path
- 18: full combined V3.4 experiment

Double-click launchers:
- V3_Unified_Test.bat     : NORMAL benchmark. One game run captures traversal/Lua + render/GPU/MSOC/shadow telemetry.
- V3_City_Frametime.bat   : compatibility alias for the same unified City-mode dataset.
- V3_Transition_Deep.bat  : special door/interior transition trace.
- V3_Render_Deep.bat      : special steady rendering-only diagnostic.

For the unified run: use the same outdoor save, hold the usual heavy outdoor view for about 45 seconds, then walk the same
2-3 minute route across several exterior cell boundaries and quit. The launcher restores settings.cfg and creates one ZIP.

V3.4 also wires the existing persistent occlusion storage pointer into CellOcclusionCallback creation, allowing unpaged
cell occluder proxies to use the same persistent cache path rather than rebuilding them without storage access.

Diagnostic streams include frame/hitch, Lua update/async/callback attribution, paging/resource/workqueue, transition/nav,
render/post-FX, MSOC detail, shadow timing, GPU memory, OSG times/resource, and frame summaries.

V3-applied-source.patch is the exact generated patch compiled by CI.
'''
(ROOT / "V3-LAB-README.txt").write_text(readme, encoding="utf-8", newline="\n")

print("V3.4 Lab packaging/preflight completed successfully.")
