import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
path = ROOT / "apps/openmw/mwsound/sfxpredecodecache.cpp"
text = path.read_text(encoding="utf-8")
old = "#include <memory>\n"
new = "#include <algorithm>\n#include <memory>\n"
if text.count(old) != 1:
    raise RuntimeError(f"V3.16 SFX-predecode compile fix expected one include anchor, found {text.count(old)}")
path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
print("V3.16 SFX predecode compile portability include applied")
