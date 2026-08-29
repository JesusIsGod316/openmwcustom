from pathlib import Path
import os

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_once(path: Path, old: str, new: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"V3.18 NIS compile-fix {label} anchor mismatch in {path}: found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")


# osg::ref_ptr<T>'s destructor calls T::unref(), so T must be complete wherever
# the owning class destructor is instantiated. nisscaler.hpp intentionally only
# forward-declares osg::BindImageTexture to avoid exposing the heavy OSG header to
# every PingPongCanvas consumer. Make NisScaler destruction out-of-line instead;
# nisscaler.cpp already includes <osg/BindImageTexture>, so the type is complete
# at the one point where ref_ptr<BindImageTexture> is destroyed.
hpp = ROOT / "apps/openmw/mwrender/nisscaler.hpp"
replace_once(
    hpp,
    "        NisScaler();\n\n        // Returns a native-resolution NIS output texture on success. Returns\n",
    "        NisScaler();\n        ~NisScaler();\n\n        // Returns a native-resolution NIS output texture on success. Returns\n",
    "destructor declaration",
)

cpp = ROOT / "apps/openmw/mwrender/nisscaler.cpp"
replace_once(
    cpp,
    "    void NisScaler::resizeGLObjectBuffers(unsigned int maxSize)\n",
    "    NisScaler::~NisScaler() = default;\n\n    void NisScaler::resizeGLObjectBuffers(unsigned int maxSize)\n",
    "out-of-line destructor definition",
)

# Fail closed if a future generator edit accidentally removes either half of the
# completeness fix or moves BindImageTexture out of the implementation unit.
hpp_text = hpp.read_text(encoding="utf-8")
cpp_text = cpp.read_text(encoding="utf-8")
assert "class BindImageTexture;" in hpp_text
assert "~NisScaler();" in hpp_text
assert "#include <osg/BindImageTexture>" in cpp_text
assert "NisScaler::~NisScaler() = default;" in cpp_text

print("V3.18 NIS BindImageTexture completeness compile-fix applied")
