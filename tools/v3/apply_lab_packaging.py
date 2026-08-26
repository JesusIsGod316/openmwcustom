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

# V3.6 promotes the proven RAM/Lua/coarse-MSOC set through an override-safe profile, then adds only bounded,
# independently selected experiments and attribution. Riskier architectural work remains measurement-only.
for script_name in (
    "apply_v36_defaults.py",
    "apply_v36_gpu_profiler.py",
    "apply_v36_shadow_culling.py",
    "apply_v36_structure_telemetry.py",
    "apply_v36_attribution.py",
    "apply_v36_launcher.py",
):
    script = Path(__file__).with_name(script_name)
    exec(compile(script.read_text(encoding="utf-8"), str(script), "exec"),
        {"__file__": str(script), "__name__": "__main__"})

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
# Intent-to-add makes generated new headers part of git diff without staging their contents. Without this, the
# applied-source artifact would silently omit files that are nevertheless present during the CI compile.
generated_new_files = [
    "components/debug/v36controllertrace.hpp",
    "components/debug/v36gpuprofiler.hpp",
    "components/debug/v36luaaddscripttrace.hpp",
    "components/debug/v36structuretrace.hpp",
    "components/settings/v36profile.hpp",
]
subprocess.run(["git", "add", "--intent-to-add", "--", *generated_new_files], cwd=ROOT, check=True)

required_markers = {
    "components/settings/v36profile.hpp": "ramOverdriveEnabled",
    "components/debug/v36gpuprofiler.hpp": "GL_QUERY_RESULT_AVAILABLE",
    "components/debug/v36luaaddscripttrace.hpp": "OPENMW_V36_LUA_ADDSCRIPT_FILE",
    "components/debug/v36controllertrace.hpp": "OPENMW_V36_CONTROLLER_FILE",
    "components/debug/v36structuretrace.hpp": "OPENMW_V36_BATCHING_FILE",
    "tools/v3/launchers/V3_Lab.ps1": "v36-true-custom-baseline",
}
for relative, marker_text in required_markers.items():
    if marker_text not in (ROOT / relative).read_text(encoding="utf-8"):
        raise RuntimeError(f"V3.6 generated-source marker missing: {relative}: {marker_text}")
if "glFinish" in (ROOT / "components/debug/v36gpuprofiler.hpp").read_text(encoding="utf-8"):
    raise RuntimeError("V3.6 asynchronous GPU profiler must not contain glFinish")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)

patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)

stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

readme = r'''OpenMW Custom V3.6 - Multipath Optimization
============================================

V3.6 makes the three validated optimizations normal custom-build behavior while retaining explicit per-feature disable switches. New runtime experiments and all deep diagnostics remain independently selectable and default off.

Normal V3.6 profile (default):
[V3]
v3.6 performance profile = true
v3.6 disable ram overdrive = false
v3.6 disable lua fast path = false
v3.6 disable coarse chunk occlusion = false

Effective normal behavior:
- RAM cache Overdrive with Balanced preload
- semantics-preserving V3.3 Lua idle-timer fast path
- visually validated V3.5 coarse paged-object/groundcover MSOC

The V3.6 profile deliberately overrides stale legacy false values. Set an individual V3.6 disable switch true to troubleshoot one proven optimization, or set the profile false to return to legacy per-setting control.

New default-off runtime experiments:
- v3.6 async gpu profiler: delayed GL timestamp queries for main world, each shadow cascade, water reflection/refraction, post-processing composite, and individual post-processing passes. It never calls glFinish and reads results only after GL_QUERY_RESULT_AVAILABLE.
- v3.6 far caster minimum pixels: OSG projected-size pruning only on the farthest shadow cascade. Near/middle cascades and ordinary rendering are unchanged.

New V3.6 attribution (environment/launcher selected, no runtime behavior change):
- v36-gpu-passes.csv: asynchronous per-pass GPU time with source/report frame and query latency.
- v36-lua-addscript.csv: sandbox, package, module, body, handler, and interface phases by container/script.
- v36-controller-build.csv: keyframe lookup, node map, controller clone/type, and source assignment by model.
- v36-source-residency.csv: largest cached source images, estimated source bytes, dimensions, mip levels, references, and cache recency. These are source-memory estimates, not exact driver allocations.
- v36-static-batching-audit.csv: repeated template groups and exact drawable/vertex topology before and after the existing ObjectPaging merge optimizer.
- Existing MSOC CSVs now include estimated paged children and groundcover instances skipped by successful coarse rejection.

Unified launcher V3.6 choices:
- 24: true custom baseline, including normal RAM cache and all V3 runtime optimizations off.
- 25: normal V3.6 performance profile.
- 26: normal profile plus asynchronous GPU pass profiler.
- 27: far-caster pruning isolated.
- 28: coarse MSOC isolated plus v2 skipped-work telemetry.
- 29: Lua/controller/residency/static-batching attribution only.
- 30: steady-state combined (normal profile + GPU profiler + far-caster pruning).
- 31: hitch combined (normal profile + deep attribution).
- 32: full V3.6 diagnostic combination.
- V3.6 choices support 4096, 6144, and 8192 shadow-distance tests. Shadow experiments support far divisors 1, 2, and 4; divisor 1 is recommended and divisor 4 remains a flicker-risk comparison only.

Safety/deferred architecture:
- Whole-far-cascade reuse remains default off and is not part of V3.6 combinations. A real static/dynamic far split needs another depth layer, RTT pass, texture bindings, and shader composition; it was not faked into this checkpoint.
- Groundcover already uses hardware instancing. ObjectPaging already groups identical templates and adaptively merges geometry. V3.6 measures that pipeline before adding a potentially conflicting general instancer.
- Foliage depth prepass is deferred because a safe alpha-test-only pass requires isolated render-bin/shader work; ordinary blended effects must not be changed.
- Lua activation/worker-barrier ordering and mutable controller instance state are unchanged.
- Residency is attribution-only; no NVML-driven eviction or destructive cache policy is present.

Benchmark invariant:
- The unified benchmark forces [Groundcover] density = 1.0 for the test so results remain comparable with the historical V3 baseline set.
- The launcher restores the user's original settings.cfg afterward, so a lower normal-play density is preserved outside benchmarking.

Double-click launchers:
- V3_Unified_Test.bat     : NORMAL benchmark. One run captures traversal/Lua + render/GPU/MSOC/shadow telemetry.
- V3_City_Frametime.bat   : compatibility alias for the same unified City-mode dataset.
- V3_Transition_Deep.bat  : special door/interior transition trace.
- V3_Render_Deep.bat      : special steady rendering-only diagnostic.

For a unified run: use the same outdoor save, hold the usual heavy outdoor view for about 45 seconds, then walk the same 2-3 minute exterior route across several cell boundaries and quit.

V3-applied-source.patch is the exact generated patch compiled by CI.
'''
(ROOT / "V3-LAB-README.txt").write_text(readme, encoding="utf-8", newline="\n")

print("V3.6 Lab packaging/preflight completed successfully.")
