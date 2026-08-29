import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

# NVIDIA's pinned SDK source contains a small amount of trailing whitespace.
# Preserve all code/tables exactly while normalizing only line-end whitespace so
# OpenMW's `git diff --check` policy remains fail-closed for generated source.
for relative in (
    "apps/openmw/mwrender/v318_nis_config.hpp",
    "apps/openmw/mwrender/v318_nis_shader.hpp",
):
    path = ROOT / relative
    text = path.read_text(encoding="utf-8")
    normalized = "\n".join(line.rstrip() for line in text.splitlines()) + "\n"
    path.write_text(normalized, encoding="utf-8", newline="\n")

print("V3.18 pinned NIS generated-source trailing whitespace normalized")
