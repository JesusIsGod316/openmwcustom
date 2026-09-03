from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# V3.22 is intentionally based on the exact final accepted V3.21 stack, not on
# a historical/raw source approximation. Re-run the complete V3.21 base and the
# accepted CP2/CP3/CP4 variant layers before applying V3.22 CP1.
v321 = HERE / "apply_diagnostic_harness_v321.py"
exec(
    compile(v321.read_text(encoding="utf-8"), str(v321), "exec"),
    {"__file__": str(v321), "__name__": "__main__"},
)

for layer_name in (
    "apply_v321_cp2_fairness_repair.py",
    "apply_v321_cp3_fullbody_first_person.py",
    "apply_v321_cp4_shadow_compat.py",
    "apply_v322_cp1_msoc_hotpath.py",
):
    layer = HERE / layer_name
    if not layer.is_file():
        raise RuntimeError(f"V3.22 exact-stack failure: required layer {layer_name} is missing")
    print(f"[V3.22] exact inherited/final layer -> {layer_name}")
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

for required in ("V3-applied-source.patch", "V3-applied-source-stat.txt", "V3-LAB-README.txt"):
    if not (ROOT / required).is_file():
        raise RuntimeError(f"V3.22 exact-stack failure: {required} was not generated")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.22 exact final-V3.21 inheritance + CP1 layer passed")
