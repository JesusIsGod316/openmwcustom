from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def replace_line_with_tokens(rel: str, tokens: tuple[str, ...], replacement: str) -> None:
    path = ROOT / rel
    lines = path.read_text(encoding="utf-8").splitlines()
    matches = [i for i, line in enumerate(lines) if all(token in line for token in tokens)]
    if len(matches) != 1:
        raise RuntimeError(f"{rel}: expected one generated line containing {tokens!r}, found {len(matches)}")
    lines[matches[0]] = replacement
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"generated-string safety patched {rel}: {' + '.join(tokens)}")


# Avoid embedding escaped quote sequences inside Python-generated C++ string
# literals. csvQuote() produces the same CSV field without fragile nested
# escaping and has already been used safely elsewhere in the V3 harness.
replace_line_with_tokens(
    "apps/openmw/mwworld/scene.cpp",
    ("cell_preload", "pressure", "lastFrameMs"),
    '                    << \',\' << Debug::V3Diagnostics::csvQuote("defer") << \',\' '
    '<< Debug::V3Diagnostics::csvQuote("cell_preload") << \',\' '
    '<< Debug::V3Diagnostics::csvQuote("pressure") << \',\' << lastFrameMs << ",1,1";',
)

replace_line_with_tokens(
    "apps/openmw/mwrender/objectpaging.cpp",
    ("object_chunk_summary",),
    '                << \',\' << Debug::V3Diagnostics::csvQuote("object_chunk_summary") << \',\' '
    '<< Debug::V3Diagnostics::csvQuote(',
)

replace_line_with_tokens(
    "apps/openmw/mwrender/groundcover.cpp",
    ("groundcover_chunk_summary",),
    '                    << \',\' << Debug::V3Diagnostics::csvQuote("groundcover_chunk_summary") << \',\' '
    '<< Debug::V3Diagnostics::csvQuote(',
)

# Catch the exact malformed-token shape MSVC reported in runs #46/#47:
# a string literal ending at "," immediately followed by an identifier, which
# C++ parses as an invalid user-defined literal suffix (e.g. operator""enqueue).
# Restrict the scan to V3-instrumented source files so unrelated upstream code
# is not affected.
scan_files = [
    "apps/openmw/mwworld/scene.cpp",
    "apps/openmw/mwrender/objectpaging.cpp",
    "apps/openmw/mwrender/groundcover.cpp",
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    "components/sceneutil/workqueue.cpp",
    "components/shader/shadermanager.cpp",
]
invalid_suffix = re.compile(r'<<\s*","[A-Za-z_]')
for rel in scan_files:
    text = (ROOT / rel).read_text(encoding="utf-8")
    bad = []
    for line_no, line in enumerate(text.splitlines(), 1):
        if invalid_suffix.search(line):
            bad.append(f"{rel}:{line_no}: {line.strip()}")
    if bad:
        raise RuntimeError("Generated C++ contains malformed CSV string literals:\n" + "\n".join(bad))

print("V3 generated C++ string-safety preflight completed successfully.")
