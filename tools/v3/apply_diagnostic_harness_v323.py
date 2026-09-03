from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# V3.23 starts from the exact final V3.21 behavior foundation plus the complete
# dormant V3.22 research stack. V3.22 mechanisms remain compiled but off unless
# a launcher mode explicitly enables them.
v321 = HERE / "apply_diagnostic_harness_v321.py"
exec(
    compile(v321.read_text(encoding="utf-8"), str(v321), "exec"),
    {"__file__": str(v321), "__name__": "__main__"},
)

layer_names = [
    "apply_v321_cp2_fairness_repair.py",
    "apply_v321_cp3_fullbody_first_person.py",
    "apply_v321_cp4_shadow_compat.py",
    "apply_v322_cp1_msoc_hotpath.py",
    "apply_v322_cp2_occluder_efficiency.py",
    "apply_v322_cp2_rank_hotpath_refinement.py",
    "apply_v322_cp2_eligibility_decoupling.py",
    "apply_v322_parallel_architecture_cp1.py",
    "apply_v323_parallel_msoc.py",
]

for layer_name in layer_names:
    layer = HERE / layer_name
    if not layer.is_file():
        raise RuntimeError(f"V3.23 exact-stack failure: required layer {layer_name} is missing")
    print(f"[V3.23] exact inherited/checkpoint layer -> {layer_name}")
    exec(
        compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
        {"__file__": str(layer), "__name__": "__main__"},
    )

for required in ("V3-applied-source.patch", "V3-applied-source-stat.txt", "V3-LAB-README.txt"):
    if not (ROOT / required).is_file():
        raise RuntimeError(f"V3.23 exact-stack failure: {required} was not generated")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.23 exact final-V3.21 inheritance + dormant V3.22 + parallel MSOC layer passed")
