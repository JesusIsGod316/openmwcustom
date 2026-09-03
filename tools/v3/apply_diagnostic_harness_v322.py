import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
branch = os.environ.get("GITHUB_REF_NAME", "")

# V3.22 is intentionally based on the exact final accepted V3.21 stack, not on
# a historical/raw source approximation. Re-run the complete V3.21 base and the
# accepted CP2/CP3/CP4 variant layers before applying V3.22 layers.
v321 = HERE / "apply_diagnostic_harness_v321.py"
exec(
    compile(v321.read_text(encoding="utf-8"), str(v321), "exec"),
    {"__file__": str(v321), "__name__": "__main__"},
)

base_layers = [
    "apply_v321_cp2_fairness_repair.py",
    "apply_v321_cp3_fullbody_first_person.py",
    "apply_v321_cp4_shadow_compat.py",
    "apply_v322_cp1_msoc_hotpath.py",
]
if branch == "v3.22-cp1-msoc-hotpath":
    layer_names = base_layers
elif branch == "v3.22-cp2-occluder-efficiency":
    layer_names = base_layers + ["apply_v322_cp2_occluder_efficiency.py"]
else:
    raise RuntimeError(
        f"V3.22 exact-stack failure: branch {branch!r} is not an admitted V3.22 branch. "
        "Refusing to guess which checkpoint layers should be compiled."
    )

for layer_name in layer_names:
    layer = HERE / layer_name
    if not layer.is_file():
        raise RuntimeError(f"V3.22 exact-stack failure: required layer {layer_name} is missing")
    print(f"[V3.22] exact inherited/checkpoint layer -> {layer_name}")
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

for required in ("V3-applied-source.patch", "V3-applied-source-stat.txt", "V3-LAB-README.txt"):
    if not (ROOT / required).is_file():
        raise RuntimeError(f"V3.22 exact-stack failure: {required} was not generated")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print(f"V3.22 exact final-V3.21 inheritance + checkpoint layers passed for {branch}")
