import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# V3.16 creates these C++ source files from branch-local patch generators. Plain
# `git diff` ignores untracked files, which would make the exact generated-source
# artifact omit the most important new audio implementations even though CMake
# compiles them. Mark them intent-to-add before the final snapshot so the patch
# artifact and QC see their complete contents.
GENERATED_NEW_FILES = {
    "apps/openmw/mwsound/headcache.hpp": (
        "class HeadCache",
        "std::shared_ptr<const HeadBuffer> lookup",
        "const std::size_t mMaxBytes",
    ),
    "apps/openmw/mwsound/headcache.cpp": (
        "constexpr std::streamoff sMaxHeadBytes = 256 * 1024",
        "HeadCache::insert",
        "makeHeadStream",
    ),
    "apps/openmw/mwsound/sfxpredecodecache.hpp": (
        "class SfxPredecodeCache",
        "std::optional<PredecodedSound> take",
        "mCancelled",
    ),
    "apps/openmw/mwsound/sfxpredecodecache.cpp": (
        "sMaxPredecodedEntryBytes = 16 * 1024 * 1024",
        "Misc::setCurrentThreadIdlePriority()",
        "std::unique_ptr<SoundDecoder> decoder = std::make_unique<FFmpegDecoder>(&mVfs, nullptr);",
        "decoder->open(Misc::ResourceHelpers::correctSoundPath(name, mVfs));",
        "mQueued.erase(queuedIt);",
        "SfxPredecodeCache::workerLoop",
    ),
}

for rel, markers in GENERATED_NEW_FILES.items():
    path = ROOT / rel
    if not path.is_file():
        raise RuntimeError(f"V3.16 generated-file QC missing required source: {rel}")
    text = path.read_text(encoding="utf-8")
    for marker in markers:
        if marker not in text:
            raise RuntimeError(f"V3.16 generated-file QC {rel} missing marker: {marker}")

# The head-cache layer rewrites FFmpegDecoder before the SFX-predecode layer is
# applied. Verify the generated constructor contract itself, then verify the
# predecode worker uses that exact contract with a null HeadCache.
ffmpeg_hpp = (ROOT / "apps/openmw/mwsound/ffmpegdecoder.hpp").read_text(encoding="utf-8")
required_ctor = "explicit FFmpegDecoder(const VFS::Manager* vfs, HeadCache* headCache);"
if required_ctor not in ffmpeg_hpp:
    raise RuntimeError("V3.16 generated-file QC missing two-argument FFmpegDecoder head-cache constructor")
if "explicit FFmpegDecoder(const VFS::Manager* vfs);" in ffmpeg_hpp:
    raise RuntimeError("V3.16 generated-file QC found stale one-argument FFmpegDecoder constructor")

predecode_text = (ROOT / "apps/openmw/mwsound/sfxpredecodecache.cpp").read_text(encoding="utf-8")
for forbidden in (
    "std::make_unique<FFmpegDecoder>(&mVfs);",
    "DecoderPtr decoder = std::make_shared<FFmpegDecoder>",
    "mQueued.erase(name);",
):
    if forbidden in predecode_text:
        raise RuntimeError(f"V3.16 generated-file QC found forbidden predecode API pattern: {forbidden}")

subprocess.run(
    ["git", "add", "-N", "--", *GENERATED_NEW_FILES.keys()], cwd=ROOT, check=True
)

# Verify each generated file is now represented as a full new-file diff rather
# than merely being referenced by CMake or a forward declaration elsewhere.
for rel, markers in GENERATED_NEW_FILES.items():
    diff = subprocess.run(
        ["git", "diff", "--no-ext-diff", "--binary", "--", rel],
        cwd=ROOT,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    ).stdout
    if "new file mode" not in diff or f"+++ b/{rel}" not in diff:
        raise RuntimeError(f"V3.16 generated-file QC snapshot does not contain full new file: {rel}")
    for marker in markers:
        if marker not in diff:
            raise RuntimeError(f"V3.16 generated-file QC diff for {rel} missing marker: {marker}")

# Fail closed if any other untracked C/C++ source/header appears under the engine
# trees. A future write_new() addition must be deliberately added to the snapshot
# list instead of silently disappearing from V3-applied-source.patch.
untracked = subprocess.run(
    ["git", "ls-files", "--others", "--exclude-standard", "--", "apps", "components"],
    cwd=ROOT,
    check=True,
    text=True,
    stdout=subprocess.PIPE,
).stdout.splitlines()
source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}
untracked_source = [rel for rel in untracked if Path(rel).suffix.lower() in source_suffixes]
if untracked_source:
    raise RuntimeError(
        "V3.16 generated-file QC found unsnapshotted source files: " + ", ".join(untracked_source)
    )

subprocess.run(["git", "diff", "--check", "--", *GENERATED_NEW_FILES.keys()], cwd=ROOT, check=True)
print("V3.16 generated new-source snapshot QC passed (4 files, no unsnapshotted engine source).")
