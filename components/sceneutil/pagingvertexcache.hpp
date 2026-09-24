/* OpenSceneGraph - Copyright (C) 1998-2006 Robert Osfield
 * Derived from osgUtil/MeshOptimizers.cpp, OpenSceneGraph-3.6.5.
 * Distributed under the OpenSceneGraph Public License (OSGPL) 0.0 or later;
 * see tools/opimizedmw/gl-p2/OSGPL-LICENSE.txt.
 * Modified for OpenMW / OpimizedMW, 2026-09-24.
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the license for details.
 *
 * Retains the original Forsyth scoring, cache model, and tie order. An indexed
 * max heap replaces the repeated whole-mesh max_element restart scan. Storage
 * is bounded by the input, not by the number of score updates. Cancellation
 * never publishes a partially reordered primitive list.
 */
#ifndef OPENMW_SCENEUTIL_PAGINGVERTEXCACHE_H
#define OPENMW_SCENEUTIL_PAGINGVERTEXCACHE_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <span>
#include <vector>

namespace SceneUtil::PagingVertexCache
{
    enum class Result { Complete, Cancelled, Invalid };
    struct Statistics
    {
        std::uint64_t scoreUpdates = 0;
        std::uint64_t restartSelections = 0;
        std::uint64_t linearRestartVisits = 0;
        std::uint64_t heapComparisons = 0;
    };
    struct Vertex
    {
        int cachePosition = -1;
        float score = 0;
        std::size_t count = 0, active = 0, offset = 0;
    };
    struct Triangle
    {
        float score = 0;
        std::array<unsigned, 3> vertices{};
    };
    inline float score(const Vertex& v)
    {
        if (!v.active) return -1.f;
        float result = 0.f;
        if (v.cachePosition >= 0)
            result = v.cachePosition < 3 ? 0.75f
                : std::pow(1.f - (v.cachePosition - 3) * (1.f / 29.f), 1.5f);
        return result + 2.f * std::pow(static_cast<float>(v.active), -0.5f);
    }

    // Every triangle has exactly one heap entry. Completed triangles remain at
    // score -1; equal scores prefer the original triangle index, as max_element.
    class ScoreHeap
    {
    public:
        ScoreHeap(std::vector<Triangle>& triangles, Statistics& stats)
            : mTriangles(triangles), mStats(stats), mHeap(triangles.size()), mPosition(triangles.size())
        {
            std::iota(mHeap.begin(), mHeap.end(), std::size_t{0});
            std::iota(mPosition.begin(), mPosition.end(), std::size_t{0});
        }
        template<class Stop> bool build(Stop& stop)
        {
            for (std::size_t i = mHeap.size() / 2; i > 0; --i)
            { if (stop()) return false; down(i - 1); }
            return true;
        }
        std::size_t best() const { return mHeap.front(); }
        void changed(std::size_t triangle)
        {
            auto i = mPosition[triangle];
            while (i && better(mHeap[i], mHeap[(i - 1) / 2]))
            { const auto parent = (i - 1) / 2; exchange(i, parent); i = parent; }
            down(i);
        }
    private:
        bool better(std::size_t a, std::size_t b)
        {
            ++mStats.heapComparisons;
            return mTriangles[a].score > mTriangles[b].score
                || (mTriangles[a].score == mTriangles[b].score && a < b);
        }
        void exchange(std::size_t a, std::size_t b)
        { std::swap(mHeap[a], mHeap[b]); mPosition[mHeap[a]] = a; mPosition[mHeap[b]] = b; }
        void down(std::size_t i)
        {
            while (i < mHeap.size() / 2)
            {
                auto child = 2 * i + 1;
                if (child + 1 < mHeap.size() && better(mHeap[child + 1], mHeap[child])) ++child;
                if (!better(mHeap[child], mHeap[i])) break;
                exchange(i, child); i = child;
            }
        }
        std::vector<Triangle>& mTriangles;
        Statistics& mStats;
        std::vector<std::size_t> mHeap, mPosition;
    };

    template<class Stop>
    Result optimize(std::span<const unsigned> input, std::size_t vertexCount,
        std::vector<unsigned>& output, Stop stop, Statistics& stats)
    {
        // Output is transactional: invalid input or cancellation leaves it alone.
        if (input.size() % 3 || vertexCount > std::numeric_limits<unsigned>::max()) return Result::Invalid;
        if (stop()) return Result::Cancelled;
        std::size_t usedVertices = 0;
        std::vector<Triangle> triangles;
        triangles.reserve(input.size() / 3);
        for (std::size_t i = 0; i < input.size(); i += 3)
        {
            if (stop()) return Result::Cancelled;
            const auto a = input[i], b = input[i + 1], c = input[i + 2];
            if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return Result::Invalid;
            // Matches the inherited OSG TriangleCounter/Adder degenerate rule.
            if (a == b || b == c || a == c) continue;
            triangles.push_back({0.f, {a, b, c}});
            usedVertices = (std::max)(usedVertices, static_cast<std::size_t>((std::max)({a, b, c})) + 1);
        }
        if (triangles.empty()) { output.clear(); return Result::Complete; }
        std::vector<Vertex> vertices(usedVertices);
        for (const auto& t : triangles)
        {
            if (stop()) return Result::Cancelled;
            for (auto index : t.vertices) ++vertices[index].count;
        }
        std::size_t count = 0;
        for (auto& v : vertices)
        { if (stop()) return Result::Cancelled; v.offset = count; count += v.count; }
        std::vector<std::size_t> adjacency(count);
        for (std::size_t i = 0; i < triangles.size(); ++i)
        {
            if (stop()) return Result::Cancelled;
            for (auto index : triangles[i].vertices)
            { auto& v = vertices[index]; adjacency[v.offset + v.active++] = i; }
        }
        for (auto& v : vertices) { if (stop()) return Result::Cancelled; v.score = score(v); }
        for (auto& t : triangles)
        { if (stop()) return Result::Cancelled; for (auto v : t.vertices) t.score += vertices[v].score; }
        // Connected meshes usually need only the first restart. Do not charge
        // heap maintenance to that common case; construct it only on the third
        // restart, using the exact then-current (including stale) OSG scores.
        std::optional<ScoreHeap> heap;
        std::vector<unsigned> cache;
        cache.reserve(35);
        std::vector<unsigned> result;
        result.reserve(triangles.size() * 3);
        bool cancelled = false;
        const auto update = [&](Vertex& v) {
            float bestScore = 0.f;
            std::size_t bestTriangle = 0;
            for (std::size_t i = v.offset; i < v.offset + v.active; ++i)
            {
                if (stop()) { cancelled = true; break; }
                const auto index = adjacency[i];
                auto& t = triangles[index];
                t.score = 0.f;
                for (auto vertex : t.vertices) t.score += vertices[vertex].score;
                ++stats.scoreUpdates;
                if (heap) heap->changed(index);
                if (t.score > bestScore) { bestScore = t.score; bestTriangle = index; }
            }
            return std::pair{bestTriangle, bestScore};
        };
        for (std::size_t remaining = triangles.size(); remaining; --remaining)
        {
            if (stop()) return Result::Cancelled;
            float bestScore = 0.f;
            std::size_t selected = triangles.size();
            for (auto vertex : cache)
            {
                const auto [index, value] = update(vertices[vertex]);
                if (cancelled) return Result::Cancelled;
                if (value > bestScore) { bestScore = value; selected = index; }
            }
            if (selected == triangles.size())
            {
                ++stats.restartSelections;
                if (!heap && stats.restartSelections > 2)
                {
                    heap.emplace(triangles, stats);
                    if (!heap->build(stop)) return Result::Cancelled;
                }
                if (heap) selected = heap->best();
                else
                {
                    selected = 0;
                    for (std::size_t i = 0; i < triangles.size(); ++i)
                    {
                        if (stop()) return Result::Cancelled;
                        ++stats.linearRestartVisits;
                        if (triangles[i].score > triangles[selected].score) selected = i;
                    }
                }
            }
            auto& t = triangles[selected];
            if (t.score <= 0.f) return Result::Invalid;
            t.score = -1.f;
            if (heap) heap->changed(selected);
            for (auto index : t.vertices)
            {
                result.push_back(index);
                auto& v = vertices[index];
                std::size_t write = v.offset;
                for (std::size_t read = v.offset; read < v.offset + v.active; ++read)
                {
                    if (stop()) return Result::Cancelled;
                    if (adjacency[read] != selected) adjacency[write++] = adjacency[read];
                }
                if (write + 1 != v.offset + v.active) return Result::Invalid;
                --v.active;
            }
            if (32 - cache.size() < 3)
            {
                for (auto it = cache.end() - 3; it != cache.end(); ++it)
                { vertices[*it].cachePosition = -1; vertices[*it].score = score(vertices[*it]); }
                for (auto it = cache.end() - 3; it != cache.end(); ++it)
                { update(vertices[*it]); if (cancelled) return Result::Cancelled; }
            }
            // Exact inherited LRU insertion order (including the last-triangle
            // tie convention), with at most 32 elements to shift.
            auto validEnd = cache.rend();
            for (auto v : t.vertices) validEnd = std::remove(cache.rbegin(), validEnd, v);
            const auto removed = static_cast<std::size_t>(cache.rend() - validEnd);
            const auto needed = 3 - removed;
            if (needed)
            {
                cache.resize((std::min)(std::size_t{32}, cache.size() + needed));
                std::copy_backward(cache.begin() + static_cast<std::ptrdiff_t>(3 - needed),
                    cache.end() - static_cast<std::ptrdiff_t>(needed), cache.end());
            }
            std::copy(t.vertices.begin(), t.vertices.end(), cache.begin());
            for (std::size_t i = 0; i < cache.size(); ++i)
            { vertices[cache[i]].cachePosition = static_cast<int>(i); vertices[cache[i]].score = score(vertices[cache[i]]); }
        }
        if (stop()) return Result::Cancelled;
        output.swap(result);
        return Result::Complete;
    }
}
#endif
