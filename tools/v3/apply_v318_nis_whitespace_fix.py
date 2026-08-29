import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()
HERE = Path(__file__).resolve().parent

# Apply the MSVC/OSG completeness correction immediately after the NIS generator.
# Keeping this as a generated-source fixup avoids hand-editing generated files and
# makes every preflight/Windows build reproduce the exact same correction.
compile_fix = HERE / "apply_v318_nis_compilefix.py"
exec(
    compile(compile_fix.read_text(encoding="utf-8"), str(compile_fix), "exec"),
    {"__file__": str(compile_fix), "__name__": "__main__"},
)

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

# Preflight guard for the exact Windows C2027 regression seen in run 33272682913.
nis_hpp = (ROOT / "apps/openmw/mwrender/nisscaler.hpp").read_text(encoding="utf-8")
nis_cpp = (ROOT / "apps/openmw/mwrender/nisscaler.cpp").read_text(encoding="utf-8")
assert "class BindImageTexture;" in nis_hpp
assert "~NisScaler();" in nis_hpp
assert "#include <osg/BindImageTexture>" in nis_cpp
assert "NisScaler::~NisScaler() = default;" in nis_cpp

print("V3.18 pinned NIS generated-source post-fixups applied")
