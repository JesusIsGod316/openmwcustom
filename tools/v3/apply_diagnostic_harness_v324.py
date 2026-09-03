from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# V3.24 is an exact layer on the final V3.23 generated stack. Reuse the complete
# V3.23 harness so Mode135 and all dormant historical mechanisms retain their
# already-validated source identity, then apply only the V3.24 QoS/async layer.
v323 = HERE / "apply_diagnostic_harness_v323.py"
if not v323.is_file():
    raise RuntimeError("V3.24 exact-stack failure: apply_diagnostic_harness_v323.py is missing")
exec(
    compile(v323.read_text(encoding="utf-8"), str(v323), "exec"),
    {"__file__": str(v323), "__name__": "__main__"},
)

layer = HERE / "apply_v324_frame_job_qos.py"
if not layer.is_file():
    raise RuntimeError("V3.24 exact-stack failure: apply_v324_frame_job_qos.py is missing")
print("[V3.24] exact inherited V3.23 stack -> apply_v324_frame_job_qos.py")
exec(
    compile(layer.read_text(encoding="utf-8"), str(layer), "exec"),
    {"__file__": str(layer), "__name__": "__main__"},
)

for required in (
    "V3-applied-source.patch",
    "V3-applied-source-stat.txt",
    "V3-LAB-README.txt",
    "components/sceneutil/framejobservice.hpp",
):
    if not (ROOT / required).is_file():
        raise RuntimeError(f"V3.24 exact-stack failure: {required} is missing")

subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
print("V3.24 exact V3.23 inheritance + frame-job QoS + zero-wait async MSOC layer passed")
