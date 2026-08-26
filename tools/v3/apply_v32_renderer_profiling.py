from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def replace_once(rel, old, new):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{rel}: expected exactly one V3.2 renderer-profile match, found {count}")
    path.write_text(text.replace(old, new, 1), encoding="utf-8", newline="\n")
    print(f"V3.2 renderer-profile patched {rel}")


replace_once(
    "components/debug/v3diagnostics.hpp",
    '''    inline CsvWriter& insertionWriter()
    {
        static CsvWriter writer("OPENMW_V3_INSERT_FILE",
            "frame,epoch_ms,cell,total_refs,rendered_refs,physics_refs,actors,animated,doors,render_ms,mechanics_ms,"
            "particles_ms,physics_ms,lua_added_ms,nav_ms");
        return writer;
    }

    inline CsvWriter& workQueueWriter()''',
    '''    inline CsvWriter& insertionWriter()
    {
        static CsvWriter writer("OPENMW_V3_INSERT_FILE",
            "frame,epoch_ms,cell,total_refs,rendered_refs,physics_refs,actors,animated,doors,render_ms,mechanics_ms,"
            "particles_ms,physics_ms,lua_added_ms,nav_ms");
        return writer;
    }

    inline CsvWriter& v32RendererInsertionWriter()
    {
        static CsvWriter writer("OPENMW_V32_RENDER_INSERT_FILE",
            "frame,epoch_ms,cell,objects,constructed,restored,paged,static_refs,animated_refs,actors,lights,"
            "renderer_total_ms,mean_object_ms,scene_instance_ms,object_root_exclusive_ms,controller_setup_ms,"
            "transform_attach_ms,misc_ms,max_object_ms,max_ref,max_model");
        return writer;
    }

    inline CsvWriter& workQueueWriter()''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v32rendererprofiling.hpp>''',
)

replace_once(
    "apps/openmw/mwrender/animation.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v32rendererprofiling.hpp>''',
)

replace_once(
    "apps/openmw/mwrender/animation.cpp",
    '''    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)
    {
        constexpr VFS::Path::ExtensionView kf("kf");''',
    '''    void Animation::addAnimSource(std::string_view model, const std::string& baseModel)
    {
        Debug::V32RendererProfiling::ScopedPhase v32ControllerTimer(
            Debug::V32RendererProfiling::Phase::ControllerSetup);
        constexpr VFS::Path::ExtensionView kf("kf");''',
)

replace_once(
    "apps/openmw/mwrender/animation.cpp",
    '''    osg::ref_ptr<osg::Node> getModelInstance(Resource::ResourceSystem* resourceSystem, const std::string& model,
        bool baseonly, bool inject, const std::string& defaultSkeleton)
    {
        Resource::SceneManager* sceneMgr = resourceSystem->getSceneManager();''',
    '''    osg::ref_ptr<osg::Node> getModelInstance(Resource::ResourceSystem* resourceSystem, const std::string& model,
        bool baseonly, bool inject, const std::string& defaultSkeleton)
    {
        Debug::V32RendererProfiling::ScopedPhase v32InstanceTimer(
            Debug::V32RendererProfiling::Phase::SceneInstance);
        Resource::SceneManager* sceneMgr = resourceSystem->getSceneManager();''',
)

replace_once(
    "apps/openmw/mwrender/animation.cpp",
    '''    void Animation::setObjectRoot(const std::string& model, bool forceskeleton, bool baseonly, bool isCreature)
    {
        osg::ref_ptr<osg::StateSet> previousStateset;''',
    '''    void Animation::setObjectRoot(const std::string& model, bool forceskeleton, bool baseonly, bool isCreature)
    {
        Debug::V32RendererProfiling::ScopedPhase v32ObjectRootTimer(
            Debug::V32RendererProfiling::Phase::ObjectRoot);
        osg::ref_ptr<osg::StateSet> previousStateset;''',
)

replace_once(
    "apps/openmw/mwrender/objects.cpp",
    '''#include <components/esm4/loaddoor.hpp>
#include <components/misc/resourcehelpers.hpp>''',
    '''#include <components/esm4/loaddoor.hpp>
#include <components/debug/v32rendererprofiling.hpp>
#include <components/misc/resourcehelpers.hpp>''',
)

replace_once(
    "apps/openmw/mwrender/objects.cpp",
    '''    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        assert(mObjects.find(ptr.mRef) == mObjects.end());''',
    '''    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        Debug::V32RendererProfiling::ScopedPhase v32AttachTimer(
            Debug::V32RendererProfiling::Phase::TransformAttach);
        assert(mObjects.find(ptr.mRef) == mObjects.end());''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        if (!restoredRendering)
        {
            if (!paged)
            {
                ptr.getClass().insertObjectRendering(ptr, model, rendering);
                if (stats)
                    ++stats->mRenderedRefs;
            }
            else
                ptr.getRefData().setBaseNode(pagedNode);
        }
        else if (stats)
            ++stats->mRenderedRefs;
        setNodeRotation(ptr, rendering, rotation);
        if (stats)
            stats->mRenderMs += Debug::V3Diagnostics::elapsedMs(phaseStart);''',
    '''        auto phaseStart = stats ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};
        const auto v32ObjectStart = Debug::V32RendererProfiling::beginObject();
        if (!restoredRendering)
        {
            if (!paged)
            {
                ptr.getClass().insertObjectRendering(ptr, model, rendering);
                if (stats)
                    ++stats->mRenderedRefs;
            }
            else
                ptr.getRefData().setBaseNode(pagedNode);
        }
        else if (stats)
            ++stats->mRenderedRefs;
        setNodeRotation(ptr, rendering, rotation);
        Debug::V32RendererProfiling::finishObject(v32ObjectStart, !restoredRendering && !paged, restoredRendering,
            paged, ptr.getClass().isActor(), ptr.getClass().useAnim(),
            ptr.getType() == ESM::Light::sRecordId || ptr.getType() == ESM4::Light::sRecordId,
            ptr.getCellRef().getRefId().toDebugString(), model.value());
        if (stats)
            stats->mRenderMs += Debug::V3Diagnostics::elapsedMs(phaseStart);''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''        auto& insertionWriter = Debug::V3Diagnostics::insertionWriter();
        V3InsertionAccumulator insertionStats;
        V3InsertionAccumulatorScope insertionScope(insertionWriter.enabled() ? &insertionStats : nullptr);

        {''',
    '''        auto& insertionWriter = Debug::V3Diagnostics::insertionWriter();
        V3InsertionAccumulator insertionStats;
        V3InsertionAccumulatorScope insertionScope(insertionWriter.enabled() ? &insertionStats : nullptr);

        auto& v32RendererWriter = Debug::V3Diagnostics::v32RendererInsertionWriter();
        const bool v32RendererProfileEnabled
            = Settings::cells().mV32RendererInsertionProfiling && v32RendererWriter.enabled();
        Debug::V32RendererProfiling::Stats v32RendererStats;
        Debug::V32RendererProfiling::Scope v32RendererScope(
            v32RendererProfileEnabled ? &v32RendererStats : nullptr);

        {''',
)

replace_once(
    "apps/openmw/mwworld/scene.cpp",
    '''            insertionWriter.writeLine(row.str());
        }
    }

    void Scene::addObjectToScene''',
    '''            insertionWriter.writeLine(row.str());
        }

        if (v32RendererProfileEnabled)
        {
            const double objectRootExclusiveMs = v32RendererStats.mObjectRootMs > v32RendererStats.mSceneInstanceMs
                ? v32RendererStats.mObjectRootMs - v32RendererStats.mSceneInstanceMs
                : 0.0;
            const double accountedMs = v32RendererStats.mSceneInstanceMs + objectRootExclusiveMs
                + v32RendererStats.mControllerSetupMs + v32RendererStats.mTransformAttachMs;
            const double miscMs = v32RendererStats.mRendererTotalMs > accountedMs
                ? v32RendererStats.mRendererTotalMs - accountedMs
                : 0.0;
            const double meanObjectMs = v32RendererStats.mObjects != 0
                ? v32RendererStats.mRendererTotalMs / static_cast<double>(v32RendererStats.mObjects)
                : 0.0;

            std::ostringstream row;
            row << Debug::V3HitchTelemetry::currentFrame() << ',' << Debug::V3Diagnostics::epochMs() << ','
                << Debug::V3Diagnostics::csvQuote(cell.getCell()->getDescription()) << ','
                << v32RendererStats.mObjects << ',' << v32RendererStats.mConstructed << ','
                << v32RendererStats.mRestored << ',' << v32RendererStats.mPaged << ','
                << v32RendererStats.mStatic << ',' << v32RendererStats.mAnimated << ','
                << v32RendererStats.mActors << ',' << v32RendererStats.mLights << ',' << std::fixed
                << std::setprecision(3) << v32RendererStats.mRendererTotalMs << ',' << meanObjectMs << ','
                << v32RendererStats.mSceneInstanceMs << ',' << objectRootExclusiveMs << ','
                << v32RendererStats.mControllerSetupMs << ',' << v32RendererStats.mTransformAttachMs << ','
                << miscMs << ',' << v32RendererStats.mMaxObjectMs << ','
                << Debug::V3Diagnostics::csvQuote(v32RendererStats.mMaxRef) << ','
                << Debug::V3Diagnostics::csvQuote(v32RendererStats.mMaxModel);
            v32RendererWriter.writeLine(row.str());
        }
    }

    void Scene::addObjectToScene''',
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    """    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V32_GPU_MEMORY_FILE',
    'OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'""",
    """    'OPENMW_V3_TRACE_FILE','OPENMW_V3_MSOC_DETAIL_FILE','OPENMW_V3_SHADOW_FILE','OPENMW_V32_GPU_MEMORY_FILE',
    'OPENMW_V32_RENDER_INSERT_FILE','OPENMW_OSG_STATS_FILE','OPENMW_OSG_STATS_LIST'""",
)

replace_once(
    "tools/v3/launchers/V3_Lab.ps1",
    """    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'""",
    """    $env:OPENMW_V3_INSERT_FILE = Join-Path $ProfileDir 'v3-insertion.csv'
    $env:OPENMW_V32_RENDER_INSERT_FILE = Join-Path $ProfileDir 'v3-render-insertion.csv'
    $env:OPENMW_V3_WORKQUEUE_FILE = Join-Path $ProfileDir 'v3-workqueue.csv'""",
)

print("V3.2 aggregate renderer-insertion profiling patch completed successfully.")
