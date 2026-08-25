from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel: str, old: str, new: str) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"settings-access safety patched {rel}")


# SettingValue<T> is intentionally converted to T at an explicit boundary.
# Avoid relying on template operator overload resolution for ostream/string
# comparison, which MSVC rejected for the adaptive scheduler setting.
replace_once(
    "apps/openmw/engine.cpp",
    '''                     << " streaming=" << Settings::cells().mV3StreamingScheduler
                     << " prepared instances=" << (Settings::cells().mV3PreparedInstanceCache ? "on" : "off")
                     << "/" << Settings::cells().mV3PreparedInstanceCacheMax;''',
    '''                     << " streaming=" << std::string(Settings::cells().mV3StreamingScheduler)
                     << " prepared instances="
                     << (static_cast<bool>(Settings::cells().mV3PreparedInstanceCache) ? "on" : "off")
                     << "/" << static_cast<int>(Settings::cells().mV3PreparedInstanceCacheMax);''',
)

# Preflight the files touched by the new settings. Any new direct comparison or
# stream of a V3 SettingValue should be made explicit before C++ compilation.
scan_files = [
    "components/settings/ramcache.hpp",
    "apps/openmw/engine.cpp",
    "apps/openmw/mwworld/scene.cpp",
]
problems = []
comparison = re.compile(r'cells\(\)\.mV3[A-Za-z0-9_]+\s*(?:==|!=)')
stream = re.compile(r'<<\s*Settings::cells\(\)\.mV3[A-Za-z0-9_]+')
for rel in scan_files:
    text = (ROOT / rel).read_text(encoding="utf-8")
    for line_no, line in enumerate(text.splitlines(), 1):
        if comparison.search(line) or stream.search(line):
            problems.append(f"{rel}:{line_no}: {line.strip()}")
if problems:
    raise RuntimeError(
        "V3 SettingValue access should use an explicit value conversion:\n" + "\n".join(problems)
    )

print("V3 settings-access safety pass completed successfully.")
