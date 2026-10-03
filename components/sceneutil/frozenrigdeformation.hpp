#ifndef OPENMW_SCENEUTIL_FROZENRIGDEFORMATION_H
#define OPENMW_SCENEUTIL_FROZENRIGDEFORMATION_H

#include "framecriticaljobgroup.hpp"
#include <osg/Array>
#include <osg/Matrixf>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

namespace SceneUtil::FrozenRigDeformation
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* p = std::getenv("OPENMW_P9_FROZEN_DEFORMATION");
            return p && std::strcmp(p, "1") == 0;
        }();
        return value;
    }
    struct Counters
    {
        inline static std::atomic_uint64_t publications{ 0 }, vertices{ 0 }, groups{ 0 }, fallbacks{ 0 };
    };
    struct Range
    {
        osg::Matrixf matrix;
        std::span<const unsigned short> vertices;
    };
    struct Output
    {
        const osg::Vec3Array& sourcePositions;
        const osg::Vec3Array* sourceNormals;
        const osg::Vec4Array* sourceTangents;
        osg::Vec3Array& positions;
        osg::Vec3Array* normals;
        osg::Vec4Array* tangents;
    };
    inline void evaluateVertex(const Range& range, unsigned short vertex, const Output& output)
    {
        output.positions[vertex] = range.matrix.preMult(output.sourcePositions[vertex]);
        if (output.normals)
            (*output.normals)[vertex] = osg::Matrixf::transform3x3((*output.sourceNormals)[vertex], range.matrix);
        if (output.tangents)
        {
            const auto& tangent = (*output.sourceTangents)[vertex];
            const osg::Vec3f result = osg::Matrixf::transform3x3(osg::Vec3f(tangent.x(), tangent.y(), tangent.z()), range.matrix);
            (*output.tangents)[vertex] = osg::Vec4f(result, tangent.w());
        }
    }
    // All numeric matrices and source ranges are frozen before admission. The
    // only writes are disjoint vertices in the current private CPU geometry slot.
    // A synchronous group joins before dirty/publication, preserving every view
    // and the existing dynamic-draw ownership boundary.
    inline bool run(std::span<const Range> ranges, const Output& output, bool requested = enabled())
    {
        constexpr std::size_t MaximumVertices = 65536;
        constexpr std::size_t MaximumGroups = 512;
        constexpr std::size_t MinimumWork = 2048;
        constexpr std::size_t VerticesPerBlock = 512;
        if (!requested || ranges.size() > MaximumGroups || output.sourcePositions.size() > MaximumVertices
            || output.positions.size() != output.sourcePositions.size()
            || (output.normals && (!output.sourceNormals || output.normals->size() != output.positions.size()
                || output.sourceNormals->size() != output.positions.size()))
            || (output.tangents && (!output.sourceTangents || output.tangents->size() != output.positions.size()
                || output.sourceTangents->size() != output.positions.size()))) return false;
        // The finite fixed bitset proves disjoint writes even for malformed
        // wrapped uint16 producer indices. Unknown/overlapping work uses control.
        std::array<std::uint64_t, MaximumVertices / 64> touched{};
        struct Block { const Range* range; std::size_t begin, end; };
        std::array<Block, MaximumGroups + MaximumVertices / VerticesPerBlock> blocks{};
        std::size_t count = 0, work = 0;
        for (const auto& range : ranges)
        {
            for (const auto vertex : range.vertices)
            {
                if (vertex >= output.positions.size()) return false;
                const std::uint64_t bit = 1ull << (vertex % 64);
                auto& word = touched[vertex / 64];
                if (word & bit) return false;
                word |= bit;
            }
            work += range.vertices.size();
            for (std::size_t begin = 0; begin < range.vertices.size(); begin += VerticesPerBlock)
            {
                if (count == blocks.size()) return false;
                blocks[count++] = Block{ &range, begin, (std::min)(begin + VerticesPerBlock, range.vertices.size()) };
            }
        }
        if (work < MinimumWork) return false;
        auto execute = [&](std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i)
            {
                const auto& block = blocks[i];
                for (std::size_t j = block.begin; j < block.end; ++j)
                    evaluateVertex(*block.range, block.range->vertices[j], output);
            }
        };
        bool complete = false;
        try { complete = FrameCriticalJobGroup::instance().parallelFor(count, 2, 1, execute); }
        catch (...) { complete = false; }
        if (!complete)
        {
            // A partially completed failed worker group is fully recomputed
            // before publication; no partial pose becomes visible.
            execute(0, count);
            Counters::fallbacks.fetch_add(1, std::memory_order_relaxed);
        }
        Counters::groups.fetch_add(1, std::memory_order_relaxed);
        Counters::vertices.fetch_add(work, std::memory_order_relaxed);
        Counters::publications.fetch_add(1, std::memory_order_release);
        return true;
    }
}
#endif
