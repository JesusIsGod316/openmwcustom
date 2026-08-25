from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"hitch-frametime safety patched {rel}")


def remove_between(rel, start, end):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    start_pos = text.find(start)
    if start_pos < 0:
        raise RuntimeError(f"{rel}: safety start marker not found: {start!r}")
    end_pos = text.find(end, start_pos)
    if end_pos < 0:
        raise RuntimeError(f"{rel}: safety end marker not found: {end!r}")
    # Keep the end marker itself; only remove the unsafe block before it.
    path.write_text(text[:start_pos] + text[end_pos:], encoding="utf-8", newline="\n")
    print(f"hitch-frametime safety removed unsafe block from {rel}")


# QuadTreeWorld stores a PositionAttitudeTransform even when a ChunkManager
# returns nullptr. If the view is unchanged it then reuses that empty container,
# so returning nullptr as a one-frame throttle can create missing pages. Keep
# only the safe predictive-preload pacing experiment for this checkpoint.
replace_once(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<int> mV3StreamingDistantObjectChunkLimit{ mIndex, "Cells",
            "v3 streaming distant object chunks per frame", makeClampSanitizerInt(1, 64) };
        SettingValue<int> mV3StreamingGroundcoverChunkLimit{ mIndex, "Cells",
            "v3 streaming groundcover chunks per frame", makeClampSanitizerInt(1, 128) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
    '''        SettingValue<float> mV3StreamingTargetFrametime{ mIndex, "Cells", "v3 streaming target frametime",
            makeMaxStrictSanitizerFloat(0) };
        SettingValue<bool> mPreloadEnabled{ mIndex, "Cells", "preload enabled" };''',
)

replace_once(
    "files/settings-default.cfg",
    '''# V3 experimental walking-stutter scheduler. OFF preserves normal OpenMW behavior.
# adaptive only throttles speculative distant object/groundcover page bursts and predictive preload scheduling
# after an already-slow frame; nearby/required cell and terrain loading is never skipped.
v3 streaming scheduler = off
v3 streaming target frametime = 25
v3 streaming distant object chunks per frame = 2
v3 streaming groundcover chunks per frame = 4''',
    '''# V3 experimental walking-stutter scheduler. OFF preserves normal OpenMW behavior.
# adaptive paces predictive background cell-preload scheduling after an already-slow frame.
# Required cell, terrain, object-page and groundcover rendering is never skipped.
v3 streaming scheduler = off
v3 streaming target frametime = 25''',
)

remove_between(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        if (!activeGrid && Settings::RamCache::adaptiveStreamingEnabled())
''',
    '''        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
''',
)

remove_between(
    "apps/openmw/mwrender/groundcover.cpp",
    '''            if (Settings::RamCache::adaptiveStreamingEnabled())
''',
    '''            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
''',
)

print("V3 Hitch + Frametime safety pass completed successfully.")
