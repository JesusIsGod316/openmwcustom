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
cpp_text = cpp_text.replace(old_empty, new_empty, 1)

# C++20 unordered_set supports transparent heterogeneous lookup here, but not
# heterogeneous erase-by-key. MSVC therefore cannot erase a NormalizedView
# directly. Use transparent find() followed by iterator erase; this is valid for
# both NormalizedView and Normalized call sites and keeps allocation off the path.
old_erase = "mQueued.erase(name);"
new_erase = "if (const auto queuedIt = mQueued.find(name); queuedIt != mQueued.end())\n                    mQueued.erase(queuedIt);"
erase_count = cpp_text.count(old_erase)
if erase_count != 6:
    raise RuntimeError(
        f"V3.16 SFX-predecode heterogeneous erase fix expected six matches, found {erase_count}"
    )
cpp_text = cpp_text.replace(old_erase, new_erase)

# DecoderPtr is a SoundManager-private alias and FFmpegDecoder only has the
# one-argument VFS constructor. Hold the concrete decoder through the public
# SoundDecoder interface so its virtual open/getInfo/readAll methods remain
# accessible while dispatching to FFmpegDecoder.
old_decoder = """            DecoderPtr decoder = std::make_shared<FFmpegDecoder>(&mVfs, nullptr);\n            decoder->open(Misc::ResourceHelpers::correctSoundPath(name, *decoder->mResourceMgr));\n"""
new_decoder = """            std::unique_ptr<SoundDecoder> decoder = std::make_unique<FFmpegDecoder>(&mVfs);\n            decoder->open(Misc::ResourceHelpers::correctSoundPath(name, mVfs));\n"""
if cpp_text.count(old_decoder) != 1:
    raise RuntimeError(
        f"V3.16 SFX-predecode decoder API fix expected one match, found {cpp_text.count(old_decoder)}"
    )
cpp_text = cpp_text.replace(old_decoder, new_decoder, 1)

# Fail closed on the exact API mistakes that caused the Windows compiler failure.
for forbidden in (
    "mQueued.erase(name);",
    "DecoderPtr decoder = std::make_shared<FFmpegDecoder>",
    "FFmpegDecoder>(&mVfs, nullptr)",
):
    if forbidden in cpp_text:
        raise RuntimeError(f"V3.16 SFX-predecode compile fix left forbidden source pattern: {forbidden}")
for required in (
    "std::unique_ptr<SoundDecoder> decoder = std::make_unique<FFmpegDecoder>(&mVfs);",
    "decoder->open(Misc::ResourceHelpers::correctSoundPath(name, mVfs));",
    "mQueued.erase(queuedIt);",
):
    if required not in cpp_text:
        raise RuntimeError(f"V3.16 SFX-predecode compile fix missing required source pattern: {required}")

cpp.write_text(cpp_text, encoding="utf-8", newline="\n")

print("V3.16 SFX predecode MSVC/API portability fixes applied")
