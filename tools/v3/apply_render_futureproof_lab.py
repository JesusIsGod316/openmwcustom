from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"render-futureproof lab patched {rel}")


# ---------------------------------------------------------------------------
# Scene template/instance diagnostics. Template misses isolate actual asset
# conversion/shader/optimizer work; instance timing isolates repeated runtime
# cloning/particle initialization during cell activation.
# ---------------------------------------------------------------------------
replace_once(
    "components/resource/scenemanager.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
)
replace_once(
    "components/resource/scenemanager.cpp",
    '''        else
        {
            osg::ref_ptr<osg::Node> loaded;
            try
            {
                loaded = load(path, mVFS, mImageManager, mNifFileManager, mBgsmFileManager);''',
    '''        else
        {
            Debug::V3Diagnostics::TraceScope trace("render", "scene_template_miss", path.value(), 0.1);
            Debug::V3Diagnostics::ScopedCsvTimer timer(
                Debug::V3Diagnostics::renderWriter(), "scene_template_miss", path.value(), 0.25);
            osg::ref_ptr<osg::Node> loaded;
            try
            {
                loaded = load(path, mVFS, mImageManager, mNifFileManager, mBgsmFileManager);''',
)
replace_once(
    "components/resource/scenemanager.cpp",
    '''    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path)
    {
        return getInstance(getTemplate(path));
    }''',
    '''    osg::ref_ptr<osg::Node> SceneManager::getInstance(VFS::Path::NormalizedView path)
    {
        Debug::V3Diagnostics::TraceScope trace("render", "scene_instance", path.value(), 0.1);
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::renderWriter(), "scene_instance", path.value(), 0.25);
        return getInstance(getTemplate(path));
    }''',
)
replace_once(
    "components/resource/scenemanager.cpp",
    '''    osg::ref_ptr<osg::Node> SceneManager::cloneNode(const osg::Node* base)
    {
        SceneUtil::CopyOp copyop;''',
    '''    osg::ref_ptr<osg::Node> SceneManager::cloneNode(const osg::Node* base)
    {
        Debug::V3Diagnostics::ScopedCsvTimer timer(
            Debug::V3Diagnostics::renderWriter(), "scene_clone", base ? base->getName() : std::string(), 0.25);
        SceneUtil::CopyOp copyop;''',
)

# ---------------------------------------------------------------------------
# Object paging deep profiler. Large distant-page builds were visible in the
# first Optimization Lab; these scopes split reference collection, template
# analysis, instance/occluder construction, merge optimization and compile-map
# submission so later optimizations can target the right stage.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod)
    {
        const osg::Vec2i startCell''',
    '''    osg::ref_ptr<osg::Node> ObjectPaging::createChunk(float size, const osg::Vec2f& center, bool activeGrid,
        const osg::Vec3f& viewPoint, bool compile, unsigned char lod)
    {
        Debug::V3Diagnostics::TraceScope trace(
            "paging", "object_chunk_create", activeGrid ? "active_grid" : "distant", 0.1);
        const osg::Vec2i startCell''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        std::map<ESM::RefNum, PagedCellRef> refs;

        if (mWorldspace == ESM::Cell::sDefaultWorldspaceId)
        {
            refs = collectESM3References(size, startCell, store);
        }
        else
        {
            refs = collectESM4References(size, startCell, mWorldspace);
        }''',
    '''        std::map<ESM::RefNum, PagedCellRef> refs;

        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_collect_refs", activeGrid ? "active_grid" : "distant", 0.1);
            if (mWorldspace == ESM::Cell::sDefaultWorldspaceId)
            {
                refs = collectESM3References(size, startCell, store);
            }
            else
            {
                refs = collectESM4References(size, startCell, mWorldspace);
            }
        }''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        AnalyzeVisitor analyzeVisitor(copyMask);
        const float minSize = mMinSizeMergeFactor ? mMinSize * mMinSizeMergeFactor : mMinSize;
        for (const auto& [refNum, ref] : refs)
        {''',
    '''        AnalyzeVisitor analyzeVisitor(copyMask);
        const float minSize = mMinSizeMergeFactor ? mMinSize * mMinSizeMergeFactor : mMinSize;
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_template_analysis", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& [refNum, ref] : refs)
            {''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            emplaced.first->second.mInstances.push_back(&ref);
        }

        const osg::Vec3f worldCenter''',
    '''                emplaced.first->second.mInstances.push_back(&ref);
            }
        }

        const osg::Vec3f worldCenter''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        for (const auto& pair : nodes)
        {
            const osg::Node* cnode = pair.first;''',
    '''        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_build_instances", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& pair : nodes)
            {
                const osg::Node* cnode = pair.first;''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            }
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;''',
    '''                }
            }
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        if (mergeGroup->getNumChildren())
        {
            SceneUtil::Optimizer optimizer;''',
    '''        if (mergeGroup->getNumChildren())
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_merge_optimize", activeGrid ? "active_grid" : "distant", 0.1);
            SceneUtil::Optimizer optimizer;''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
        if (!stateToCompile.empty() && ico)
        {
            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);''',
    '''        osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
        if (!stateToCompile.empty() && ico)
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_compile_map", activeGrid ? "active_grid" : "distant", 0.1);
            auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(group);''',
)
replace_once(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        group->getBound();
        group->setNodeMask(Mask_Static);''',
    '''        if (Debug::V3Diagnostics::renderWriter().enabled())
        {
            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                << ",\"object_chunk_summary\"," << Debug::V3Diagnostics::csvQuote(
                    std::string(activeGrid ? "active" : "distant") + " refs=" + std::to_string(refs.size())
                    + " templates=" + std::to_string(nodes.size()))
                << ",0";
            Debug::V3Diagnostics::renderWriter().writeLine(row.str());
        }

        group->getBound();
        group->setNodeMask(Mask_Static);''',
)

# ---------------------------------------------------------------------------
# Groundcover workload summaries: model count and actual instance count make it
# possible to compare CPU/GPU tests at equivalent foliage density.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/groundcover.cpp",
    '''            InstanceMap instances;
            collectInstances(instances, size, center);
            osg::ref_ptr<osg::Node> node = createChunk(instances, center);''',
    '''            InstanceMap instances;
            collectInstances(instances, size, center);
            if (Debug::V3Diagnostics::renderWriter().enabled())
            {
                std::size_t instanceCount = 0;
                for (const auto& [_, entries] : instances)
                    instanceCount += entries.size();
                std::ostringstream row;
                row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs()
                    << ",\"groundcover_chunk_summary\"," << Debug::V3Diagnostics::csvQuote(
                        std::string("models=") + std::to_string(instances.size()) + " instances="
                        + std::to_string(instanceCount))
                    << ",0";
                Debug::V3Diagnostics::renderWriter().writeLine(row.str());
            }
            osg::ref_ptr<osg::Node> node = createChunk(instances, center);''',
)

# ---------------------------------------------------------------------------
# Post-processing pass diagnostics. The post chain is manually submitted from
# PingPongCanvas rather than separate OSG cameras, so whole-camera GPU stats
# cannot identify effects. Record CPU submission cost and target dimensions at
# draw time. Do not mutate StateSets or programs during chain construction.
# ---------------------------------------------------------------------------
replace_once(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''#include <cassert>

#include <components/shader/shadermanager.hpp>''',
    '''#include <cassert>
#include <iomanip>
#include <sstream>

#include <components/debug/v3diagnostics.hpp>
#include <components/shader/shadermanager.hpp>''',
)
replace_once(
    "apps/openmw/mwrender/pingpongcanvas.cpp",
    '''                if (!state.getLastAppliedProgramObject())
                    mFallbackProgram->apply(state);

                drawGeometry(renderInfo);

                if (pass.mRenderTarget && pass.mRenderTexture->getNumMipmapLevels() > 0)''',
    '''                if (!state.getLastAppliedProgramObject())
                    mFallbackProgram->apply(state);

                auto& v3PostFxWriter = Debug::V3Diagnostics::postFxWriter();
                if (v3PostFxWriter.enabled())
                {
                    const auto v3Start = Debug::V3Diagnostics::Clock::now();
                    drawGeometry(renderInfo);
                    const double v3CpuMs = Debug::V3Diagnostics::elapsedMs(v3Start);
                    const osg::Viewport* v3Viewport = state.getCurrentViewport();
                    const int v3Width = v3Viewport ? static_cast<int>(v3Viewport->width()) : 0;
                    const int v3Height = v3Viewport ? static_cast<int>(v3Viewport->height()) : 0;
                    std::ostringstream row;
                    row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                        << Debug::V3Diagnostics::threadId() << ','
                        << Debug::V3Diagnostics::csvQuote(node.mHandle ? node.mHandle->getName() : std::string("unknown"))
                        << ',' << passIndex << ',' << std::fixed << std::setprecision(4) << v3CpuMs << ',' << v3Width
                        << ',' << v3Height << ',' << (pass.mRenderTarget ? 1 : 0) << ',' << (pass.mMipMap ? 1 : 0);
                    v3PostFxWriter.writeLine(row.str());
                }
                else
                    drawGeometry(renderInfo);

                if (pass.mRenderTarget && pass.mRenderTexture->getNumMipmapLevels() > 0)''',
)

print("V3 Render Future-Proof Lab source patch completed successfully.")
