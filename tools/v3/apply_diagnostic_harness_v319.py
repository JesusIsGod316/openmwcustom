from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

v318 = HERE / "apply_diagnostic_harness_v318.py"
exec(compile(v318.read_text(encoding="utf-8"), str(v318), "exec"), {"__file__": str(v318), "__name__": "__main__"})

for layer_name in (
    "apply_v319_focus_cadence.py",
    "apply_v319_runtime_modes.py",
    "apply_v319_binary_identity.py",
    "apply_v319_stable_gaming.py",
):
    layer = HERE / layer_name
    exec(compile(layer.read_text(encoding="utf-8"), str(layer), "exec"), {"__file__": str(layer), "__name__": "__main__"})

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.19 CPU critical path P0 — focus temporal coherence + OSG scheduler matrix
============================================================================

Purpose
-------
V3.19 pivots back to the CPU/engine ceiling proven by V3.18. P0 preserves the
entire V3.18 renderer/NIS stack and adds two low-risk switchable CPU experiments
in one executable while PBR-aware texture-atlas/ObjectPaging integration is
prepared as the larger drawable-count optimization.

Focus temporal coherence
------------------------
OPENMW_V319_FOCUS_CADENCE remains the lab override for the per-frame GUI focus
refresh in normal gameplay. The stable gaming build exposes the same mechanism as
[V3] v3.19 focus cadence and defaults it to the promoted cadence 2 result. Cadence
1 reproduces the causal P0/V3.18 control and cadence 3 remains available for manual
experimentation. GUI mode still refreshes every frame. Activation/input object
queries are untouched, so this does not turn interaction itself into a half-rate
operation.

OSG scheduler matrix
--------------------
The launcher uses OSG's existing OSG_THREADING control rather than adding a new
engine threading system. Empty/unset is the exact automatic V3.18 control and is
the promoted stable scheduler policy. CullDrawThreadPerContext and
CullThreadPerCameraDrawThreadPerContext remain lab-only causal alternatives.

Stable gaming lineage
---------------------
This branch is generated from clean V3.19 P0 commit 8f94832770cbc97ca991e6a7f9ff83838f7afecc.
It intentionally excludes V3.19 P1/P1b static-instancing and compatibility-shader
changes. Rubic0n remains packaged for V3.17 attribution modes only; normal/direct
runtime selection remains stock LuaJIT. Existing native controls for the validated
V3.6 performance profile and V3.18 render-scale/NIS architecture are preserved
without duplicate settings.

Modes
-----
102 = V3.19 CPU control: native render, OSG automatic, focus cadence 1.
103 = focus cadence 2.
104 = focus cadence 3.
105 = CullDrawThreadPerContext, focus cadence 1.
106 = CullThreadPerCameraDrawThreadPerContext, focus cadence 1.
107 = CullDrawThreadPerContext + focus cadence 2.
108 = CullThreadPerCameraDrawThreadPerContext + focus cadence 2.

For ordinary gaming, no lab mode is required: native [V3] focus cadence 2 plus OSG
automatic is the stable policy. Lab modes remain available for causal reruns.
''')

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE).stdout
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")
engine_text = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
settings_text = (ROOT / "files/settings-default.cfg").read_text(encoding="utf-8")
cells_text = (ROOT / "components/settings/categories/cells.hpp").read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.19-cpu-p0",
    "openmw-custom-v3.19-p0-stable-gaming",
    "OPENMW_V319_FOCUS_CADENCE",
    "Settings::cells().mV319FocusCadence",
    "frameNumber % v319FocusCadence == 0",
    "mWindowManager->isGuiMode()",
    "openmw-custom-v3.18-render-scale-p0",
    "mNisScaler->dispatch",
    "mV35CoarseChunkOcclusion",
    "mV319FocusCadence",
    "v3.19 focus cadence = 2",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.19 stable final source snapshot missing marker: {marker}")

for marker in (
    "102 = V3.19 CPU control",
    "v319-focus2",
    "v319-focus3",
    "CullDrawThreadPerContext",
    "CullThreadPerCameraDrawThreadPerContext",
    "Enter 1 through 108",
    "v319_focus_cadence=$V319FocusCadence",
    "v319_osg_threading=$V319OsgThreading",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.19 launcher missing marker: {marker}")

if engine_text.count("OPENMW_V319_FOCUS_CADENCE") != 1:
    raise RuntimeError("V3.19 focus cadence source marker count is not exactly one")
if "Activation/input queries remain untouched." not in engine_text:
    raise RuntimeError("V3.19 focus safety marker missing")
if "Settings::cells().mV319FocusCadence" not in engine_text:
    raise RuntimeError("V3.19 stable native focus setting is not wired into engine")
if "openmw-custom-v3.19-p0-stable-gaming" not in engine_text:
    raise RuntimeError("V3.19 stable executable identity missing")
if "mV319FocusCadence" not in cells_text or "makeClampSanitizerInt(1, 3)" not in cells_text:
    raise RuntimeError("V3.19 stable focus setting declaration/sanitizer missing")
if "v3.19 focus cadence = 2" not in settings_text:
    raise RuntimeError("V3.19 stable promoted default missing")

for forbidden in ("v319StaticInstance", "OPENMW_V319_STATIC_INSTANCING"):
    if forbidden in patch_text:
        raise RuntimeError(f"V3.19 stable P0 patch contaminated by P1/P1b marker: {forbidden}")

for marker in (
    "V3.19 CPU critical path P0",
    "Stable gaming lineage",
    "Rubic0n remains packaged",
    "native [V3] focus cadence 2 plus OSG",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.19 README missing stable marker: {marker}")

print("V3.19 clean-P0 stable gaming generated-source policy invariants passed")
