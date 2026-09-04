#ifndef OPENMW_COMPONENTS_DEBUG_V36STRUCTURETRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36STRUCTURETRACE_H

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>

#include <osg/Drawable>
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

        void apply(osg::Drawable& drawable) override
        {
            ++mStats.mDrawables;
            if (const osg::Geometry* geometry = drawable.asGeometry())
                if (const osg::Array* vertices = geometry->getVertexArray())
                    mStats.mVertices += vertices->getNumElements();
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
