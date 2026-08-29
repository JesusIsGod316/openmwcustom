import os
import subprocess
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
engine = ROOT / "apps/openmw/engine.cpp"
text = engine.read_text(encoding="utf-8")

# The artifact gate examines the actual linked PE image. Source-only V3.17
# comments/identifiers are not sufficient because LTO can discard them. Bind one
# explicit lowercase identity literal to a startup OpenGL-identification log path
# that is part of openmw.exe and is executed once per process. This has no
# gameplay-path cost and gives CI a deterministic executable identity marker.
old = '            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);\n'
new = (
    '            Log(Debug::Info) << "Build identity: openmw-custom-v3.17";\n'
    '            Log(Debug::Info) << "OpenGL Vendor: " << glGetString(GL_VENDOR);\n'
)
count = text.count(old)
if count != 1:
    raise RuntimeError(f"apps/openmw/engine.cpp: expected exactly one V3.17 binary-identity anchor, found {count}")
text = text.replace(old, new, 1)
engine.write_text(text, encoding="utf-8", newline="\n")

marker = 'openmw-custom-v3.17'
engine_text = engine.read_text(encoding="utf-8")
if engine_text.count(marker) != 1:
    raise RuntimeError("V3.17 binary identity marker was not inserted exactly once into executable-bound source")

# The V3.17 packager takes its snapshot before this final identity layer. Refresh
# the exact generated patch/stat here so preflight artifacts and the Windows
# compile describe the same final source tree.
subprocess.run(["git", "diff", "--check"], cwd=ROOT, check=True)
patch = subprocess.run(
    ["git", "diff", "--no-ext-diff", "--binary"], cwd=ROOT, check=True, stdout=subprocess.PIPE
).stdout
patch_text = patch.decode("utf-8", errors="replace")
if marker not in patch_text:
    raise RuntimeError("V3.17 generated patch snapshot does not contain the executable-bound identity marker")
(ROOT / "V3-applied-source.patch").write_bytes(patch)
stat = subprocess.run(
    ["git", "diff", "--stat"], cwd=ROOT, check=True, text=True, stdout=subprocess.PIPE
).stdout
(ROOT / "V3-applied-source-stat.txt").write_text(stat, encoding="utf-8", newline="\n")

print("V3.17 executable-bound identity marker applied and generated-source snapshot refreshed.")
