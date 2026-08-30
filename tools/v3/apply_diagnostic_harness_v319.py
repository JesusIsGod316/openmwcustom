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

V3.19 CPU critical path P1b — semantic-control repair + static hardware instancing
==================================================================================

Purpose
-------
V3.19 P1b keeps the validated focus-cadence2 improvement and OSG Automatic
threading, then repairs the semantic-control flaw found in the first P1 package.
The first P1 executable globally rewrote the base compatibility vertex shaders,
so OPENMW_V319_STATIC_INSTANCING=0 disabled ObjectPaging batching but did not
restore the validated P0 shader program. That package is rejected for benchmark
interpretation: its 103/109/110 results cannot isolate the instancing mechanism.

P1b preserves the same distant-grid hardware-instancing experiment but makes the
shader side strictly opt-in. Before P1 runs, the generated P0 base shaders are
captured. After the original P1 layer applies ObjectPaging/launcher changes, all
four base vertex shaders are restored byte-for-byte. Separate
*_v319_instanced.vert variants are then generated for objects, BS default,
BS nolighting, and shadow casting. When the promoted Rafael PBR overlay contains
a targeted runtime shader, its exact archived payload is used as the variant
source so P1b does not bypass the promoted PBR semantics.

Safety / semantics
------------------
OPENMW_V319_STATIC_INSTANCING=0 is now a true semantic control: ShaderVisitor
uses its original getProgram(shaderPrefix, ...) path, MWShadowTechnique loads the
original shadowcasting.vert, and the four generated P0 base vertex shader files
are unchanged by P1b. Modes 1/2 select only the alternate instancing vertex
shader while retaining the same fragment shader, define map, program template,
and sampler bindings. The generated shader-control provenance file records
source-control, restored-control, runtime-control, and variant SHA-256 values.

Instancing remains restricted to the distant grid. Active-grid objects retain
their refnum/picking semantics. Candidate templates are deep-copied once per
small batch, static transforms are flattened, residual transform/live LOD fails
closed to legacy, and batch size remains capped at 8. Existing material/texture
StateSets are preserved.

Modes
-----
103 = promoted V3.19 control: focus cadence2, OSG Automatic, exact P0 shader path,
      legacy ObjectPaging/static instancing off.
109 = conservative P1b: opt-in instancing shaders + repeated distant templates
      only where legacy geometry merging would not have fired.
110 = aggressive P1b: opt-in instancing shaders + eligible repeated distant
      templates including candidates the legacy merge heuristic would duplicate.

Validation order
----------------
First validate mode 103 visually in the exact scene that exposed the P1 shadow
regression. It must match the validated P0 build before any performance result is
accepted. Then validate 109/110 visually. Only after semantic-control QA passes
should 103 -> 109 -> 110 be benchmarked on the canonical City route.
''')

base_shader_paths = (
    ROOT / "files/shaders/compatibility/objects.vert",
    ROOT / "files/shaders/compatibility/bs/default.vert",
    ROOT / "files/shaders/compatibility/bs/nolighting.vert",
    ROOT / "files/shaders/compatibility/shadowcasting.vert",
)
variant_shader_paths = (
    ROOT / "files/shaders/compatibility/objects_v319_instanced.vert",
    ROOT / "files/shaders/compatibility/bs/default_v319_instanced.vert",
    ROOT / "files/shaders/compatibility/bs/nolighting_v319_instanced.vert",
    ROOT / "files/shaders/compatibility/shadowcasting_v319_instanced.vert",
)
control_manifest_path = ROOT / "V3.19-P1-SHADER-CONTROL.txt"

for path in (*variant_shader_paths, control_manifest_path):
    if not path.is_file():
        raise RuntimeError(f"V3.19 P1b generated file missing before source snapshot: {path}")

# Include generated variant/control files in the exact applied-source patch while
# leaving the index clean for the downstream Windows build.
intent_paths = [str(path.relative_to(ROOT)) for path in (*variant_shader_paths, control_manifest_path)]
subprocess.run(["git", "add", "-N", "--", *intent_paths], cwd=ROOT, check=True)
try:
    subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
    patch = subprocess.run(
        ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
    ).stdout
    stat = subprocess.run(
        ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
    ).stdout
finally:
    subprocess.run(["git", "reset", "--", *intent_paths], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)

(ROOT / "V3-applied-source.patch").write_bytes(patch)
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

patch_text = patch.decode("utf-8", errors="replace")
launcher_text = (ROOT / "tools/v3/launchers/V3_Lab.ps1").read_text(encoding="utf-8")
readme_text = readme_path.read_text(encoding="utf-8")
engine_text = (ROOT / "apps/openmw/engine.cpp").read_text(encoding="utf-8")
objectpaging_text = (ROOT / "apps/openmw/mwrender/objectpaging.cpp").read_text(encoding="utf-8")
visitor_text = (ROOT / "components/shader/shadervisitor.cpp").read_text(encoding="utf-8")
shadow_cpp_text = (ROOT / "components/sceneutil/mwshadowtechnique.cpp").read_text(encoding="utf-8")
shader_cmake_text = (ROOT / "files/shaders/CMakeLists.txt").read_text(encoding="utf-8")
control_manifest = control_manifest_path.read_text(encoding="utf-8")

for marker in (
    "openmw-custom-v3.19-cpu-p1b",
    "OPENMW_V319_FOCUS_CADENCE",
    "frameNumber % v319FocusCadence == 0",
    "mWindowManager->isGuiMode()",
    "openmw-custom-v3.18-render-scale-p0",
    "mNisScaler->dispatch",
    "mV35CoarseChunkOcclusion",
    "OPENMW_V319_STATIC_INSTANCING",
    "setNumInstances",
    "objects_v319_instanced.vert",
    "shadowcasting_v319_instanced.vert",
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
    raise RuntimeError("V3.19 P1b instancing environment marker count is not exactly one")

for path in base_shader_paths:
    text = path.read_text(encoding="utf-8")
    if "v319StaticInstanceMatrix" in text or "GL_ARB_draw_instanced" in text:
        raise RuntimeError(f"V3.19 P1b semantic-control contamination remains in base shader: {path}")

for path in variant_shader_paths:
    text = path.read_text(encoding="utf-8")
    if "v319StaticInstanceMatrix" not in text or "GL_ARB_draw_instanced" not in text:
        raise RuntimeError(f"V3.19 P1b instancing marker missing in shader variant: {path}")
    cmake_rel = path.relative_to(ROOT / "files/shaders").as_posix()
    if shader_cmake_text.count(cmake_rel) != 1:
        raise RuntimeError(f"V3.19 P1b shader variant not shipped exactly once: {cmake_rel}")

for marker in (
    "v319StaticInstancingShaderVariantsEnabled",
    'shaderPrefix + "_v319_instanced.vert"',
    "program = mShaderManager.getProgram(shaderPrefix, defineMap, mProgramTemplate, samplers);",
):
    if marker not in visitor_text:
        raise RuntimeError(f"V3.19 P1b ShaderVisitor policy marker missing: {marker}")
for marker in (
    "v319StaticInstancingShadowVariantEnabled",
    '"shadowcasting_v319_instanced.vert"',
    ': "shadowcasting.vert";',
):
    if marker not in shadow_cpp_text:
        raise RuntimeError(f"V3.19 P1b shadow policy marker missing: {marker}")

if "mode0_shader_control=byte-identical" not in control_manifest:
    raise RuntimeError("V3.19 P1b byte-identical shader-control proof missing")
control_lines = [line for line in control_manifest.splitlines() if line.startswith("files/shaders/")]
if len(control_lines) != 4:
    raise RuntimeError(f"V3.19 P1b shader-control provenance line count mismatch: {len(control_lines)}")
for line in control_lines:
    parts = line.split("|")
    fields = dict(part.split("=", 1) for part in parts[1:])
    if fields.get("source_control_sha256") != fields.get("restored_sha256"):
        raise RuntimeError(f"V3.19 P1b base shader byte-identity proof failed: {line}")
    if not fields.get("runtime_control_sha256") or not fields.get("variant_sha256") or not fields.get("runtime_origin"):
        raise RuntimeError(f"V3.19 P1b shader provenance incomplete: {line}")

for marker in (
    "V3.19 CPU critical path P1b",
    "semantic-control flaw",
    "restored byte-for-byte",
    "exact P0 shader path",
    "103 -> 109 -> 110",
):
    if marker not in readme_text:
        raise RuntimeError(f"V3.19 P1b README missing marker: {marker}")

print("V3.19 CPU P1b generated-source semantic-control policy invariants passed")
