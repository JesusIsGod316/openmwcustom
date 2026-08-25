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

replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        if (!activeGrid && Settings::RamCache::adaptiveStreamingEnabled())
        {
            const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
            if (lastFrameMs > Settings::RamCache::streamingTargetFrameMs())
            {
                static thread_local unsigned v3Frame = std::numeric_limits<unsigned>::max();
                static thread_local int v3Created = 0;
                const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
                if (v3Frame != frame)
                {
                    v3Frame = frame;
                    v3Created = 0;
                }
                const int limit = Settings::RamCache::streamingDistantObjectChunkLimit();
                if (v3Created >= limit)
                {
                    if (Debug::V3Diagnostics::streamingWriter().enabled())
                    {
                        std::ostringstream row;
                        row << frame << ',' << Debug::V3Diagnostics::epochMs()
                            << ",\\\"defer\\\",\\\"object_chunk\\\",\\\"distant\\\"," << lastFrameMs << ',' << limit << ','
                            << v3Created;
                        Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
                    }
                    return nullptr;
                }
                ++v3Created;
            }
        }
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);''',
    '''        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);''',
)

replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        else
        {
            if (Settings::RamCache::adaptiveStreamingEnabled())
            {
                const double lastFrameMs = Debug::V3HitchTelemetry::lastFrameWallMs();
                if (lastFrameMs > Settings::RamCache::streamingTargetFrameMs())
                {
                    static thread_local unsigned v3Frame = std::numeric_limits<unsigned>::max();
                    static thread_local int v3Created = 0;
                    const unsigned frame = Debug::V3HitchTelemetry::currentFrame();
                    if (v3Frame != frame)
                    {
                        v3Frame = frame;
                        v3Created = 0;
                    }
                    const int limit = Settings::RamCache::streamingGroundcoverChunkLimit();
                    if (v3Created >= limit)
                    {
                        if (Debug::V3Diagnostics::streamingWriter().enabled())
                        {
                            std::ostringstream row;
                            row << frame << ',' << Debug::V3Diagnostics::epochMs()
                                << ",\\\"defer\\\",\\\"groundcover_chunk\\\",\\\"distant\\\"," << lastFrameMs << ',' << limit
                                << ',' << v3Created;
                            Debug::V3Diagnostics::streamingWriter().writeLine(row.str());
                        }
                        return nullptr;
                    }
                    ++v3Created;
                }
            }
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
            InstanceMap instances;''',
    '''        else
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::pagingWriter(), "groundcover_chunk_create", "", 0.25);
            InstanceMap instances;''',
)

print("V3 Hitch + Frametime safety pass completed successfully.")
