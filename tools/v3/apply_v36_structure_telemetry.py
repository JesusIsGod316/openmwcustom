import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 structure match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 structure telemetry patched {rel} ({count} match(es))")


def write_new(rel, text):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: refusing to overwrite an existing file")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"V3.6 structure telemetry added {rel}")


# Observational static-batching audit. It measures the scene graph immediately before and after OpenMW's existing
# adaptive merge optimizer; it does not add a second, potentially conflicting instancing implementation.
write_new(
    "components/debug/v36structuretrace.hpp",
    r'''#ifndef OPENMW_COMPONENTS_DEBUG_V36STRUCTURETRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36STRUCTURETRACE_H

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include <osg/Geode>
#include <osg/Geometry>
#include <osg/NodeVisitor>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36StructureTrace
{
    struct StructureStats
    {
        std::uint64_t mDrawables = 0;
        std::uint64_t mVertices = 0;

        StructureStats& operator+=(const StructureStats& other)
        {
            mDrawables += other.mDrawables;
            mVertices += other.mVertices;
            return *this;
        }
    };

    class StructureVisitor : public osg::NodeVisitor
    {
    public:
        StructureVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Geode& geode) override
        {
            for (unsigned int i = 0; i < geode.getNumDrawables(); ++i)
            {
                ++mStats.mDrawables;
                if (const osg::Geometry* geometry = geode.getDrawable(i)->asGeometry())
                    if (const osg::Array* vertices = geometry->getVertexArray())
                        mStats.mVertices += vertices->getNumElements();
            }
            traverse(geode);
        }

        StructureStats mStats;
    };

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_BATCHING_FILE",
            "frame,epoch_ms,active_grid,chunk_size,lod,source_refs,template_groups,repeated_template_groups,"
            "repeated_instances,total_instances,merge_candidate_groups,drawables_before,drawables_after,"
            "vertices_before,vertices_after,chunk_build_ms");
        return writer;
    }

    inline StructureStats inspect(osg::Node& node)
    {
        StructureVisitor visitor;
        node.accept(visitor);
        return visitor.mStats;
    }

    inline void writeChunk(bool activeGrid, float size, unsigned int lod, std::size_t sourceRefs,
        std::size_t templateGroups, std::size_t repeatedGroups, std::size_t repeatedInstances,
        std::size_t totalInstances, std::size_t mergeCandidateGroups, const StructureStats& before,
        const StructureStats& after, double buildMs)
    {
        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ',' << (activeGrid ? 1 : 0)
            << ',' << std::fixed << std::setprecision(3) << size << ',' << lod << ',' << sourceRefs << ','
            << templateGroups << ',' << repeatedGroups << ',' << repeatedInstances << ',' << totalInstances << ','
            << mergeCandidateGroups << ',' << before.mDrawables << ',' << after.mDrawables << ',' << before.mVertices
            << ',' << after.mVertices << ',' << buildMs;
        writer().writeLine(row.str());
    }
}

#endif
''',
)

replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''#include <components/debug/v3diagnostics.hpp>''',
    '''#include <components/debug/v3diagnostics.hpp>
#include <components/debug/v36structuretrace.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        Debug::V3Diagnostics::TraceScope trace(
            "paging", "object_chunk_create", activeGrid ? "active_grid" : "distant", 0.1);''',
    '''        Debug::V3Diagnostics::TraceScope trace(
            "paging", "object_chunk_create", activeGrid ? "active_grid" : "distant", 0.1);
        const bool v36StructureEnabled = Debug::V36StructureTrace::writer().enabled();
        const auto v36StructureStart
            = v36StructureEnabled ? Debug::V3Diagnostics::Clock::now() : Debug::V3Diagnostics::Clock::time_point{};''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_build_instances", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& pair : nodes)''',
    '''        std::size_t v36RepeatedGroups = 0;
        std::size_t v36RepeatedInstances = 0;
        std::size_t v36TotalInstances = 0;
        std::size_t v36MergeCandidateGroups = 0;
        {
            Debug::V3Diagnostics::ScopedCsvTimer timer(Debug::V3Diagnostics::renderWriter(),
                "object_chunk_build_instances", activeGrid ? "active_grid" : "distant", 0.1);
            for (const auto& pair : nodes)''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            if (numinstances > 0)
            {
                // add a ref to the original template''',
    '''            if (numinstances > 0)
            {
                v36TotalInstances += numinstances;
                if (numinstances > 1)
                {
                    ++v36RepeatedGroups;
                    v36RepeatedInstances += numinstances;
                }
                if (merge)
                    ++v36MergeCandidateGroups;
                // add a ref to the original template''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;

        if (mergeGroup->getNumChildren())''',
    '''        Debug::V36StructureTrace::StructureStats v36BeforeStats;
        if (v36StructureEnabled)
        {
            v36BeforeStats += Debug::V36StructureTrace::inspect(*group);
            v36BeforeStats += Debug::V36StructureTrace::inspect(*mergeGroup);
        }

        const osg::Vec3f relativeViewPoint = viewPoint - worldCenter;

        if (mergeGroup->getNumChildren())''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''        group->getBound();
        if (mV35CoarseChunkOcclusion && pagedOccluderData)''',
    '''        if (v36StructureEnabled)
        {
            const Debug::V36StructureTrace::StructureStats v36AfterStats
                = Debug::V36StructureTrace::inspect(*group);
            Debug::V36StructureTrace::writeChunk(activeGrid, size, lod, refs.size(), nodes.size(),
                v36RepeatedGroups, v36RepeatedInstances, v36TotalInstances, v36MergeCandidateGroups,
                v36BeforeStats, v36AfterStats, Debug::V3Diagnostics::elapsedMs(v36StructureStart));
        }

        group->getBound();
        if (mV35CoarseChunkOcclusion && pagedOccluderData)''',
)

# V3.6 MSOC-v2 telemetry: quantify how much work each successful V3.5 coarse rejection bypasses. No new
# occluder classes are trusted and the culler remains bound to the main camera traversal.
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        bool testVisibleCoarseAABB(const osg::BoundingBox& worldBB, OcclusionTestCategory category) const;''',
    '''        bool testVisibleCoarseAABB(const osg::BoundingBox& worldBB, OcclusionTestCategory category,
            std::uint64_t estimatedChildren = 0) const;''',
)
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                "paged_chunks_tested,paged_chunks_occluded,groundcover_chunks_tested,groundcover_chunks_occluded");''',
    '''                "paged_chunks_tested,paged_chunks_occluded,groundcover_chunks_tested,groundcover_chunks_occluded,"
                "paged_estimated_children_skipped,groundcover_estimated_instances_skipped");''',
)
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                << mGroundcoverChunksOccluded;''',
    '''                << mPagedChunksTested << ',' << mPagedChunksOccluded << ',' << mGroundcoverChunksTested << ','
                << mGroundcoverChunksOccluded << ',' << mPagedEstimatedChildrenSkipped << ','
                << mGroundcoverEstimatedInstancesSkipped;''',
)
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                          "groundcover_chunks_tested,groundcover_chunks_occluded,cache_mem_hits_total,cache_db_hits_total,"''',
    '''                          "groundcover_chunks_tested,groundcover_chunks_occluded,paged_estimated_children_skipped,"
                          "groundcover_estimated_instances_skipped,cache_mem_hits_total,cache_db_hits_total,"''',
)
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''                   << mGroundcoverChunksOccluded << ',' << cache.memHits << ',' << cache.dbHits << ',' << cache.misses''',
    '''                   << mGroundcoverChunksOccluded << ',' << mPagedEstimatedChildrenSkipped << ','
                   << mGroundcoverEstimatedInstancesSkipped << ',' << cache.memHits << ',' << cache.dbHits << ','
                   << cache.misses''',
)
replace_exact(
    "components/sceneutil/occlusionculling.hpp",
    '''        mutable unsigned int mGroundcoverChunksOccluded = 0;

        // V3 telemetry''',
    '''        mutable unsigned int mGroundcoverChunksOccluded = 0;
        mutable std::uint64_t mPagedEstimatedChildrenSkipped = 0;
        mutable std::uint64_t mGroundcoverEstimatedInstancesSkipped = 0;

        // V3 telemetry''',
)
replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''        mGroundcoverChunksTested = 0;
        mGroundcoverChunksOccluded = 0;''',
    '''        mGroundcoverChunksTested = 0;
        mGroundcoverChunksOccluded = 0;
        mPagedEstimatedChildrenSkipped = 0;
        mGroundcoverEstimatedInstancesSkipped = 0;''',
)
replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''    bool OcclusionCuller::testVisibleCoarseAABB(
        const osg::BoundingBox& worldBB, OcclusionTestCategory category) const''',
    '''    bool OcclusionCuller::testVisibleCoarseAABB(const osg::BoundingBox& worldBB,
        OcclusionTestCategory category, std::uint64_t estimatedChildren) const''',
)
replace_exact(
    "components/sceneutil/occlusionculling.cpp",
    '''            ++mNumOccluded;
            ++*occluded;
        }
        return visible;''',
    '''            ++mNumOccluded;
            ++*occluded;
            if (category == OcclusionTestCategory::PagedChunk)
                mPagedEstimatedChildrenSkipped += estimatedChildren;
            else
                mGroundcoverEstimatedInstancesSkipped += estimatedChildren;
        }
        return visible;''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''#include <unordered_map>
#include <vector>''',
    '''#include <cstdint>
#include <unordered_map>
#include <vector>''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''            , mChunkBounds(copy.mChunkBounds)
        {''',
    '''            , mChunkBounds(copy.mChunkBounds)
            , mEstimatedChildren(copy.mEstimatedChildren)
        {''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        osg::BoundingBox mChunkBounds;''',
    '''        osg::BoundingBox mChunkBounds;
        std::uint64_t mEstimatedChildren = 0;''',
)
replace_exact(
    "apps/openmw/mwrender/objectpaging.cpp",
    '''            pagedOccluderData->mChunkBounds = v35BoundsVisitor.getBoundingBox();''',
    '''            pagedOccluderData->mChunkBounds = v35BoundsVisitor.getBoundingBox();
            pagedOccluderData->mEstimatedChildren = v36TotalInstances;''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''            visible = mCuller->testVisibleCoarseAABB(
                worldBounds, SceneUtil::OcclusionTestCategory::PagedChunk);''',
    '''            visible = mCuller->testVisibleCoarseAABB(worldBounds,
                SceneUtil::OcclusionTestCategory::PagedChunk, pagedData->mEstimatedChildren);''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        CoarseOcclusionCallback(
            SceneUtil::OcclusionCuller* culler, const osg::BoundingBox& localBounds, bool groundcover);''',
    '''        CoarseOcclusionCallback(SceneUtil::OcclusionCuller* culler, const osg::BoundingBox& localBounds,
            bool groundcover, std::uint64_t estimatedChildren);''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.hpp",
    '''        bool mGroundcover;
    };''',
    '''        bool mGroundcover;
        std::uint64_t mEstimatedChildren;
    };''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''    CoarseOcclusionCallback::CoarseOcclusionCallback(
        SceneUtil::OcclusionCuller* culler, const osg::BoundingBox& localBounds, bool groundcover)
        : mCuller(culler)
        , mLocalBounds(localBounds)
        , mGroundcover(groundcover)''',
    '''    CoarseOcclusionCallback::CoarseOcclusionCallback(SceneUtil::OcclusionCuller* culler,
        const osg::BoundingBox& localBounds, bool groundcover, std::uint64_t estimatedChildren)
        : mCuller(culler)
        , mLocalBounds(localBounds)
        , mGroundcover(groundcover)
        , mEstimatedChildren(estimatedChildren)''',
)
replace_exact(
    "apps/openmw/mwrender/occlusionculling.cpp",
    '''        if (mCuller->testVisibleCoarseAABB(worldBounds, category))''',
    '''        if (mCuller->testVisibleCoarseAABB(worldBounds, category, mEstimatedChildren))''',
)
replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''        osg::ComputeBoundsVisitor cbv;
        group->accept(cbv);''',
    '''        std::uint64_t v36InstanceCount = 0;
        for (const auto& [_, entries] : instances)
            v36InstanceCount += entries.size();

        osg::ComputeBoundsVisitor cbv;
        group->accept(cbv);''',
)
replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''            group->addCullCallback(new CoarseOcclusionCallback(mOcclusionCuller, box, true));''',
    '''            group->addCullCallback(
                new CoarseOcclusionCallback(mOcclusionCuller, box, true, v36InstanceCount));''',
)

print("V3.6 coarse-MSOC and existing-static-batching attribution source patch completed successfully.")
