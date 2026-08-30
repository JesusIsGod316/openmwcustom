import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
engine = ROOT / "apps/openmw/engine.cpp"
text = engine.read_text(encoding="utf-8")
old = "openmw-custom-v3.17 / openmw-custom-v3.18-render-scale-p0"
new = old + " / openmw-custom-v3.19-cpu-p1"
if text.count(old) != 1:
    raise RuntimeError(f"V3.19 identity anchor mismatch: {text.count(old)}")
text = text.replace(old, new, 1)
if text.count("openmw-custom-v3.19-cpu-p1") != 1:
    raise RuntimeError("V3.19 P1 identity marker insertion failed")
engine.write_text(text, encoding="utf-8", newline="\n")
print("V3.19 P1 executable identity extended")
