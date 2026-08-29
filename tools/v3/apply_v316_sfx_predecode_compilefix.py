import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

hpp = ROOT / "apps/openmw/mwsound/sfxpredecodecache.hpp"
hpp_text = hpp.read_text(encoding="utf-8")
old_hpp = "#include <deque>\n#include <mutex>\n"
new_hpp = "#include <deque>\n#include <functional>\n#include <mutex>\n"
if hpp_text.count(old_hpp) != 1:
    raise RuntimeError(
        f"V3.16 SFX-predecode header fix expected one include anchor, found {hpp_text.count(old_hpp)}"
    )
hpp_text = hpp_text.replace(old_hpp, new_hpp, 1)
old_hpp2 = "#include <unordered_set>\n#include <vector>\n"
new_hpp2 = "#include <unordered_set>\n#include <utility>\n#include <vector>\n"
if hpp_text.count(old_hpp2) != 1:
    raise RuntimeError(
        f"V3.16 SFX-predecode header utility anchor expected one match, found {hpp_text.count(old_hpp2)}"
    )
hpp.write_text(hpp_text.replace(old_hpp2, new_hpp2, 1), encoding="utf-8", newline="\n")

cpp = ROOT / "apps/openmw/mwsound/sfxpredecodecache.cpp"
cpp_text = cpp.read_text(encoding="utf-8")
old_cpp = "#include <memory>\n"
new_cpp = "#include <algorithm>\n#include <memory>\n"
if cpp_text.count(old_cpp) != 1:
    raise RuntimeError(
        f"V3.16 SFX-predecode compile fix expected one include anchor, found {cpp_text.count(old_cpp)}"
    )
cpp_text = cpp_text.replace(old_cpp, new_cpp, 1)

# VFS::Path::Normalized exposes the underlying string through value(); avoid
# relying on an empty() member that is not part of the stable path API.
old_empty = "if (name.empty() || mCancelled.contains(name) || mReady.contains(name) || mQueued.contains(name))"
new_empty = "if (name.value().empty() || mCancelled.contains(name) || mReady.contains(name) || mQueued.contains(name))"
if cpp_text.count(old_empty) != 1:
    raise RuntimeError(
        f"V3.16 SFX-predecode path-empty fix expected one match, found {cpp_text.count(old_empty)}"
    )
cpp.write_text(cpp_text.replace(old_empty, new_empty, 1), encoding="utf-8", newline="\n")

print("V3.16 SFX predecode compile portability/header/path fixes applied")
