from pathlib import Path


# V3.7+ generated CellPreloader source already has settings access in the inherited
# stack, so V3.11 must not require the old pristine adjacent include pair. V3.6+
# diagnostics also instrument ObjectPaging::getChunk between the lod calculation
# and createChunk call, so replace that block against the generated source rather
# than the pristine source shape.
base = Path(__file__).with_name("apply_v311_adjacent_active_grid.py")
text = base.read_text(encoding="utf-8")

redundant_include = '''replace_exact(
    "apps/openmw/mwworld/cellpreloader.cpp",
    \'\'\'#include <components/resource/scenemanager.hpp>
#include <components/terrain/view.hpp>\'\'\',
    \'\'\'#include <components/resource/scenemanager.hpp>
#include <components/settings/values.hpp>
#include <components/terrain/view.hpp>\'\'\',
)

'''

pristine_getchunk = '''replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    \'\'\'        const ChunkId id = std::make_tuple(center, size, activeGrid);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            return static_cast<osg::Node*>(obj.get());

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());
        return node;\'\'\',
    \'\'\'        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
        {
            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                if (mV311PreparedActiveChunks.contains(id))
                    mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
            }
            return static_cast<osg::Node*>(obj.get());
        }

        if (v311PrepareMode > 0 && activeGrid && !compile)
        {
            mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.erase(id);
        }

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());

        if (v311PrepareMode > 0 && activeGrid && compile)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;\'\'\',
)

'''

for block, name in ((redundant_include, "CellPreloader settings include"),
                    (pristine_getchunk, "pristine ObjectPaging getChunk")):
    if text.count(block) != 1:
        raise RuntimeError(f"Unable to remove V3.11 {name} edit")
    text = text.replace(block, "", 1)

# Execute every V3.11 edit whose generated-source context is already stable.
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})

# Now layer prepared-hit/fallback accounting into the already-instrumented getChunk.
obj = Path(__file__).resolve().parents[2] / "apps/openmw/mwrender/objectpaging.cpp"
source = obj.read_text(encoding="utf-8")
old = '''        const ChunkId id = std::make_tuple(center, size, activeGrid);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
            return static_cast<osg::Node*>(obj.get());

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());
        return node;'''
new = '''        const ChunkId id = std::make_tuple(center, size, activeGrid);
        const int v311PrepareMode = static_cast<int>(Settings::cells().mV311ActiveGridPrepareMode);

        if (const osg::ref_ptr<osg::Object> obj = mCache->getRefFromObjectCache(id))
        {
            if (v311PrepareMode > 0 && activeGrid && !compile)
            {
                std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
                if (mV311PreparedActiveChunks.contains(id))
                    mV311PreparedActiveHits.fetch_add(1, std::memory_order_relaxed);
            }
            return static_cast<osg::Node*>(obj.get());
        }

        if (v311PrepareMode > 0 && activeGrid && !compile)
        {
            mV311DemandFallbacks.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.erase(id);
        }

        const unsigned char lod = static_cast<unsigned char>(lodFlags >> (4 * 4));
        Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::pagingWriter(), "object_chunk_create",
            activeGrid ? "active_grid" : "distant", 0.25);
        osg::ref_ptr<osg::Node> node = createChunk(size, center, activeGrid, viewPoint, compile, lod);
        mCache->addEntryToObjectCache(id, node.get());

        if (v311PrepareMode > 0 && activeGrid && compile)
        {
            std::lock_guard<std::mutex> lock(mV311PreparedActiveMutex);
            mV311PreparedActiveChunks.insert(id);
            mV311PreparedActiveBuilt.fetch_add(1, std::memory_order_relaxed);
        }
        return node;'''
if source.count(old) != 1:
    raise RuntimeError(f"Instrumented ObjectPaging getChunk: expected 1 match, found {source.count(old)}")
obj.write_text(source.replace(old, new, 1), encoding="utf-8", newline="\n")
print("V3.11 fixed wrapper patched instrumented ObjectPaging::getChunk (1 match)")
