from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]

# Install the V3 helper launchers and exact applied-source snapshot beside the
# runtime executable. This script only runs on v3-performance through the V3
# harness, so upstream/default builds are unaffected.
cmake = ROOT / "CMakeLists.txt"
text = cmake.read_text(encoding="utf-8")
marker = "# V3 OPTIMIZATION LAB PACKAGING"
if marker not in text:
    text += r'''

# V3 OPTIMIZATION LAB PACKAGING
if(WIN32)
    install(FILES
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

# Validate the fully transformed source tree, then preserve exactly what the
# build will compile. --check catches whitespace errors and conflict markers.
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"],
    cwd=ROOT,
    check=True,
    stdout=subprocess.PIPE,
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)

stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

readme = r'''OpenMW Custom V3 - Hitch / Frametime / Render Lab
==================================================

This build contains opt-in diagnostics and experiments. Normal OpenMW behavior
is preserved unless a V3 experiment/profile stream is explicitly enabled.

Recommended 32 GB baseline:
[Cells]
ram cache mode = overdrive
ram cache overdrive preload = balanced

New experiments (off by default):
v3 streaming scheduler = off
v3 streaming target frametime = 25
v3 prepared instance cache = false
v3 prepared instance cache max = 8192
v3.3 speculative preload budget = 0

[Shadows]
v3.3 far cascade update interval = 1
v3.3 far cascade max texel drift = 0.75

Double-click launchers:
- V3_City_Frametime.bat   : lightweight walking/cell-boundary frametime test
- V3_Transition_Deep.bat  : deep door/cell transition trace
- V3_Render_Deep.bat      : steady rendering/shadow/MSOC/post-FX diagnostic

The City and Transition launchers temporarily standardize Overdrive settings,
restore settings.cfg after OpenMW exits, create a hardware/build/settings
manifest, and automatically ZIP the completed profile.

Diagnostic streams supported by this build include:
OPENMW_V3_FRAME_FILE
OPENMW_V3_HITCH_FILE
OPENMW_V3_EVENT_FILE
OPENMW_V3_TRANSITION_FILE
OPENMW_V3_PAGING_FILE
OPENMW_V3_RESOURCE_FILE
OPENMW_V3_NAV_FILE
OPENMW_V3_INSERT_FILE
OPENMW_V3_WORKQUEUE_FILE
OPENMW_V3_RENDER_FILE
OPENMW_V3_POSTFX_FILE
OPENMW_V3_STREAMING_FILE
OPENMW_V3_TRACE_FILE
OPENMW_V3_LUA_UPDATE_FILE
OPENMW_V3_LUASYNC_FILE
OPENMW_V3_LUA_ACTION_FILE
OPENMW_V3_MSOC_DETAIL_FILE
OPENMW_V3_SHADOW_FILE
OPENMW_V3_TELEMETRY_FILE
OPENMW_V32_GPU_MEMORY_FILE
OPENMW_V33_FRAME_SUMMARY_FILE
OPENMW_OSG_STATS_FILE

V3-applied-source.patch is the exact patch produced by the V3 harness before
this executable was compiled. v3_trace_to_chrome.py converts v3-trace.csv to a
Chrome/Perfetto-compatible trace JSON.
'''
(ROOT / "V3-LAB-README.txt").write_text(readme, encoding="utf-8", newline="\n")

print("V3 Lab packaging/preflight completed successfully.")
