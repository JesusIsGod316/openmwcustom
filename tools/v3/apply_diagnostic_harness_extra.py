from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"patched {rel}")


replace_once(
    "apps/openmw/mwphysics/physicssystem.cpp",
    '''#include <components/settings/values.hpp>''',
    '''#include <components/settings/ramcache.hpp>\n#include <components/settings/values.hpp>''',
)

replace_once(
    "apps/openmw/mwphysics/physicssystem.cpp",
    '''              Settings::cells().mCacheExpiryDelay))''',
    '''              Settings::RamCache::cacheExpiryDelay()))''',
)

print("V3 RAM policy collision-cache patch completed successfully.")
