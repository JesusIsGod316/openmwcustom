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
    "apply_v319_static_instancing_generated_compat.py",
):
    layer = HERE / layer_name
    exec(compile(layer.read_text(encoding="utf-8"), str(layer), "exec"), {"__file__": str(layer), "__name__": "__main__"})

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.19 CPU critical path P1 — promoted focus cadence2 + static hardware instancing
================================================================================

Purpose
-------
V3.19 P1 keeps the validated focus-cadence2 improvement and OSG Automatic
threading, then attacks the dominant CPU cull/draw submission path structurally.
The live source audit showed SceneManager already runs global OSG duplicate-state
sharing after shaders are created, so another StateSet/material canonicalization
pass would mostly duplicate existing work. ObjectPaging also already groups
references by template and has a legacy geometry-merge path that can duplicate
vertex data per reference. P1 therefore adds true hardware instancing for
repeated distant static templates while preserving the legacy path as control
and fallback.

Safety / semantics
------------------
OPENMW_V319_STATIC_INSTANCING defaults to 0, which is exact legacy ObjectPaging
behavior. Instancing is restricted to the distant grid: active-grid objects keep
their existing refnum/picking semantics. Candidate templates are deep-copied
once per small batch, static transforms are flattened, and any residual transform
or live LOD fails closed to the legacy path. Existing material/texture StateSets
are not rewritten, so standard OpenMW and BS/PBR material paths retain their
current states. Main object, BS default/nolighting, and shadow-casting vertex
paths all apply the same per-instance matrix. Batch size is capped at 8 to keep
GLSL 1.20 uniform pressure conservative and limit loss of per-object culling.

Modes
-----
103 = promoted V3.19 control: focus cadence2, OSG Automatic, legacy ObjectPaging.
109 = conservative P1: instance repeated distant templates only when legacy
      geometry merging would not have fired.
110 = aggressive P1: prefer instancing for eligible repeated distant templates,
      including candidates the old merge heuristic would otherwise duplicate.

Test 103 -> 109 -> 110 on the same canonical City route. The key acceptance
criteria are lower cull/draw CPU time and improved frame-time distribution with
no PBR/alpha/shadow/LOD visual regressions and no material VRAM-residency rise.
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
objectpaging_text = (ROOT / "apps/openmw/mwrender/objectpaging.cpp").read_text(encoding="utf-8")
objects_shader = (ROOT / "files/shaders/compatibility/objects.vert").read_text(encoding="utf-8")
bs_shader = (ROOT / "files/shaders/compatibility/bs/default.vert").read_text(encoding="utf-8")
shadow_shader = (ROOT / "files/shaders/compatibility/shadowcasting.vert").read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.19-cpu-p1",
    "OPENMW_V319_FOCUS_CADENCE",
    "frameNumber % v319FocusCadence == 0",
    "mWindowManager->isGuiMode()",
    "openmw-custom-v3.18-render-scale-p0",
    "mNisScaler->dispatch",
    "mV35CoarseChunkOcclusion",
    "OPENMW_V319_STATIC_INSTANCING",
    "setNumInstances",
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.19 P1 final source snapshot missing marker: {marker}")

for marker in (
    "102 = V3.19 CPU control",
    "v319-focus2",
    "CullDrawThreadPerContext",
    "CullThreadPerCameraDrawThreadPerContext",
    "109 = V3.19 P1 conservative static instancing",
    "110 = V3.19 P1 aggressive static instancing",
    "Enter 1 through 110",
    "v319_static_instancing=$V319StaticInstancing",
    "OPENMW_V319_STATIC_INSTANCING",
):
    if marker not in launcher_text:
        raise RuntimeError(f"V3.19 P1 launcher missing marker: {marker}")

if engine_text.count("OPENMW_V319_FOCUS_CADENCE") != 1:
    raise RuntimeError("V3.19 focus cadence source marker count is not exactly one")
if "Activation/input queries remain untouched." not in engine_text:
    raise RuntimeError("V3.19 focus safety marker missing")
if objectpaging_text.count("OPENMW_V319_STATIC_INSTANCING") != 1:
    raise RuntimeError("V3.19 P1 instancing environment marker count is not exactly one")
for text, label in ((objects_shader, "objects"), (bs_shader, "bs/default"), (shadow_shader, "shadowcasting")):
    if "v319StaticInstanceMatrix" not in text or "GL_ARB_draw_instanced" not in text:
        raise RuntimeError(f"V3.19 P1 {label} shader instancing marker missing")

for marker in (
    "V3.19 CPU critical path P1",
    "global OSG duplicate-state",
    "sharing after shaders are created",
    "103 -> 109 -> 110",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.19 P1 README missing marker: {marker}")

print("V3.19 CPU P1 generated-source policy invariants passed")
