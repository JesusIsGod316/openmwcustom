from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# V3.25 is the final V3.x bridge. CP2 inherits the exact closed V3.24 generated
# stack, then CP1 batching/packaging, then only the Mode151 parallel-binding layer.
v324 = HERE / "apply_diagnostic_harness_v324.py"
if not v324.is_file():
    raise RuntimeError("V3.25 exact-stack failure: apply_diagnostic_harness_v324.py is missing")
exec(
    compile(v324.read_text(encoding="utf-8"), str(v324), "exec"),
    {"__file__": str(v324), "__name__": "__main__"},
)

cp1 = HERE / "apply_v325_engine_ownership_bridge_v2.py"
if not cp1.is_file():
    raise RuntimeError("V3.25 exact-stack failure: apply_v325_engine_ownership_bridge_v2.py is missing")
print("[V3.25] exact inherited V3.24 stack -> apply_v325_engine_ownership_bridge_v2.py")
exec(
    compile(cp1.read_text(encoding="utf-8"), str(cp1), "exec"),
    {"__file__": str(cp1), "__name__": "__main__"},
)

cp2 = HERE / "apply_v325_engine_ownership_bridge_cp2_v2.py"
if not cp2.is_file():
    raise RuntimeError("V3.25 exact-stack failure: apply_v325_engine_ownership_bridge_cp2_v2.py is missing")
print("[V3.25] CP1 batching foundation -> apply_v325_engine_ownership_bridge_cp2_v2.py")
exec(
    compile(cp2.read_text(encoding="utf-8"), str(cp2), "exec"),
    {"__file__": str(cp2), "__name__": "__main__"},
)

for required in (
    "V3-applied-source.patch",
    "V3-applied-source-stat.txt",
    "V3-LAB-README.txt",
    "components/sceneutil/framejobservice.hpp",
    "components/sceneutil/framecriticaljobgroup.hpp",
    "components/debug/v3deeptelemetry.hpp",
    "tools/v3/V325-ENGINE-OWNERSHIP-BRIDGE-SOURCE-AUDIT.txt",
    "tools/v3/V325-CP2-PARALLEL-BINDING-AUDIT.txt",
):
    if not (ROOT / required).is_file():
        raise RuntimeError(f"V3.25 exact-stack failure: {required} is missing")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.25 exact V3.24 inheritance + CP1 batching/packaging + CP2 Mode151 parallel binding passed")
