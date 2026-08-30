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
    "apply_v319_p1b_visual_correctness.py",
):
    layer = HERE / layer_name
    exec(compile(layer.read_text(encoding="utf-8"), str(layer), "exec"), {"__file__": str(layer), "__name__": "__main__"})

readme_path = ROOT / "V3-LAB-README.txt"
with readme_path.open("a", encoding="utf-8", newline="\n") as readme:
    readme.write(r'''

V3.19 CPU critical path P1b — P1 visual-correctness isolation
==============================================================

Purpose
-------
P1b keeps the promoted V3.19 focus-cadence2 improvement, OSG Automatic, and
P1 static-instancing mechanism, but repairs shader-state/correctness hazards
found after P1 visual testing. The P1 executable modified the shared object,
BS, and shadow vertex shaders even when static instancing was disabled.

Corrections
-----------
Every ObjectPaging chunk now provides an explicit v319StaticInstanceCount=0
uniform, while an actual instanced batch overrides that value with its positive
instance count. This prevents a shared GL program from carrying an instanced
uniform value into an ordinary draw.

The object and BS shader paths now begin from the literal P0 vertex and
gl_NormalMatrix and enter the instance transform only when the count is
positive. P1 had also reused tangent-composed normalToViewMatrix for viewNormal
on normal-mapped materials; P1b separates the base object normal transform from
the tangent-space matrix so non-instanced shadow/environment normal behavior is
the original P0 behavior. The object particle-occlusion coordinate path now uses
the instance-corrected vertex as well.

Modes
-----
103 remains the visual/control smoke test: focus cadence2 + OSG Automatic +
legacy ObjectPaging. 109 and 110 retain the conservative/aggressive P1 static
instancing policies, now through the P1b-corrected shader path.

Test 103 first. If the P1 visual regression is gone in 103, test 109 then 110 on
the same canonical City route. If 103 still shows the regression, compare
against the exact promoted P0 artifact before further instancing work.
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
nolighting_shader = (ROOT / "files/shaders/compatibility/bs/nolighting.vert").read_text(encoding="utf-8")
shadow_shader = (ROOT / "files/shaders/compatibility/shadowcasting.vert").read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.19-cpu-p1",
    "openmw-custom-v3.19-cpu-p1b",
    "OPENMW_V319_FOCUS_CADENCE",
    "frameNumber % v319FocusCadence == 0",
    "mWindowManager->isGuiMode()",
    "openmw-custom-v3.18-render-scale-p0",
    "mNisScaler->dispatch",
    "mV35CoarseChunkOcclusion",
    "OPENMW_V319_STATIC_INSTANCING",
    "setNumInstances",
    'new osg::Uniform("v319StaticInstanceCount", 0)',
):
    if marker not in patch_text:
        raise RuntimeError(f"V3.19 P1b final source snapshot missing marker: {marker}")

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
        raise RuntimeError(f"V3.19 P1b launcher missing marker: {marker}")

if engine_text.count("OPENMW_V319_FOCUS_CADENCE") != 1:
    raise RuntimeError("V3.19 focus cadence source marker count is not exactly one")
if "Activation/input queries remain untouched." not in engine_text:
    raise RuntimeError("V3.19 focus safety marker missing")
if engine_text.count("openmw-custom-v3.19-cpu-p1b") != 1:
    raise RuntimeError("V3.19 P1b executable identity marker count is not exactly one")
if objectpaging_text.count("OPENMW_V319_STATIC_INSTANCING") != 1:
    raise RuntimeError("V3.19 P1 instancing environment marker count is not exactly one")
if objectpaging_text.count('new osg::Uniform("v319StaticInstanceCount", 0)') != 1:
    raise RuntimeError("V3.19 P1b deterministic zero-state marker count is not exactly one")

for text, label in (
    (objects_shader, "objects"),
    (bs_shader, "bs/default"),
    (nolighting_shader, "bs/nolighting"),
    (shadow_shader, "shadowcasting"),
):
    if "v319StaticInstanceMatrix" not in text or "GL_ARB_draw_instanced" not in text:
        raise RuntimeError(f"V3.19 P1b {label} shader instancing marker missing")
    if "v319StaticInstanceVertex(" in text or "v319StaticInstanceNormal(" in text:
        raise RuntimeError(f"V3.19 P1b {label} still contains P1 shared helper path")
    if "vec4 v319Vertex = gl_Vertex;" not in text:
        raise RuntimeError(f"V3.19 P1b {label} literal P0 vertex path missing")
    if "if (v319StaticInstanceCount > 0)" not in text:
        raise RuntimeError(f"V3.19 P1b {label} opt-in instance branch missing")

for text, label in ((objects_shader, "objects"), (bs_shader, "bs/default")):
    if "normalToViewMatrix = v319BaseNormalToView;" not in text:
        raise RuntimeError(f"V3.19 P1b {label} base normal path missing")
    if "normalize(v319BaseNormalToView * passNormal)" not in text:
        raise RuntimeError(f"V3.19 P1b {label} view-normal path missing")
    if "normalize(normalToViewMatrix * passNormal)" in text:
        raise RuntimeError(f"V3.19 P1b {label} tangent-space view-normal regression remains")

if "orthoDepthMapCoord = ((depthSpaceMatrix * model) * v319Vertex).xyz;" not in objects_shader:
    raise RuntimeError("V3.19 P1b objects particle-occlusion instance vertex missing")

for marker in (
    "V3.19 CPU critical path P1b",
    "P1 visual-correctness isolation",
    "explicit v319StaticInstanceCount=0",
    "103 remains the visual/control smoke test",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.19 P1b README missing marker: {marker}")

print("V3.19 CPU P1b generated-source policy invariants passed")
