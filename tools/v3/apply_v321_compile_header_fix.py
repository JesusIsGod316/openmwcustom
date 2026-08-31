import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

path = ROOT / "apps/openmw/engine.cpp"
text = path.read_text(encoding="utf-8")

header = "#include <osgUtil/IncrementalCompileOperation>"
if header in text:
    raise RuntimeError("V3.21 ICO header fix unexpectedly already applied")

anchor = "#include <osgViewer/Renderer>\n"
if text.count(anchor) != 1:
    raise RuntimeError(f"V3.21 ICO header anchor mismatch: {text.count(anchor)}")

text = text.replace(anchor, header + "\n\n" + anchor, 1)
path.write_text(text, encoding="utf-8", newline="\n")

if header not in path.read_text(encoding="utf-8"):
    raise RuntimeError("V3.21 ICO header fix failed")

print("V3.21 explicit IncrementalCompileOperation header dependency applied")
