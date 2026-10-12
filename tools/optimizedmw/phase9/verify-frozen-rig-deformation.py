import os
import subprocess
import sys

exe = sys.argv[1]
results = []
for enabled in ("0", "1"):
    env = dict(os.environ, OPENMW_P9_FROZEN_DEFORMATION=enabled)
    run = subprocess.run([exe], env=env, text=True, capture_output=True, timeout=45)
    if run.returncode:
        raise RuntimeError(f"production rig fixture {enabled} failed:\n{run.stdout}\n{run.stderr}")
    rows = [line for line in run.stdout.splitlines() if line.startswith("frame ")]
    if len(rows) != 16:
        raise RuntimeError(f"production rig fixture {enabled} incomplete: {run.stdout}")
    results.append(rows)
    print(run.stdout)
if results[0] != results[1]:
    raise RuntimeError("Control and candidate production positions, normals and tangents differ bit for bit")
print("Actual production frozen-rig control/candidate parity passed for all 16 frames and secondary views")
