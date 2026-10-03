#include "pagingwork.hpp"
#include "pagingvertexcache.hpp"

#include <osg/Array>
#include <osg/BufferObject>
#include <osg/Geometry>
#include <osg/PrimitiveSet>
#include <osg/TriangleIndexFunctor>
#include <osgUtil/MeshOptimizers>

#include <stdexcept>
#include <vector>
#include <cstdlib>

#include <components/debug/hitchtrace.hpp>

namespace SceneUtil
{
    namespace
    {
        bool exactPagingWorkReductionEnabled()
        {
            static const bool enabled = [] {
                const char* value = std::getenv("OPENMW_P9_EXACT_PAGING_WORK");
                return value && std::string_view(value) == "1";
            }();
            return enabled;
        }
        struct CollectTriangles
        {
            std::vector<unsigned>* values = nullptr;
            unsigned remaining = 256;
            void operator()(unsigned a, unsigned b, unsigned c)
            {
                if (--remaining == 0) { PagingWorkScope::checkpoint(); remaining = 256; }
                values->insert(values->end(), {a, b, c});
            }
        };
    }
    void optimizePagingVertices(osg::Node& node)
    {
        PagingWorkScope::checkpoint();
        osgUtil::VertexCacheVisitor collector;
        node.accept(collector);
        for (osg::Geometry* geom : collector.getGeometryList())
        {
            PagingWorkScope::checkpoint();
            const osg::Array* vertices = geom->getVertexArray();
            if (!vertices || vertices->getNumElements() <= 16) continue;
            bool supported = true;
            for (const auto& primitive : geom->getPrimitiveSetList())
            {
                if (!primitive) { supported = false; break; }
                switch (primitive->getMode())
                {
                    case GL_TRIANGLES: case GL_TRIANGLE_STRIP: case GL_TRIANGLE_FAN:
                    case GL_QUADS: case GL_QUAD_STRIP: case GL_POLYGON: break;
                    default: supported = false;
                }
                const auto type = primitive->getType();
                supported = supported && (type == osg::PrimitiveSet::DrawElementsUBytePrimitiveType
                    || type == osg::PrimitiveSet::DrawElementsUShortPrimitiveType
                    || type == osg::PrimitiveSet::DrawElementsUIntPrimitiveType);
                if (!supported) break;
            }
            // Exact same unsupported-primitive fallback as the original visitor.
            if (!supported) continue;
            std::vector<unsigned> input, output;
            osg::TriangleIndexFunctor<CollectTriangles> functor;
            functor.values = &input;
            for (const auto& primitive : geom->getPrimitiveSetList()) primitive->accept(functor);
            unsigned remaining = 256;
            auto stop = [&remaining] {
                if (--remaining != 0) return false;
                remaining = 256;
                return PagingWorkScope::cancelled();
            };
            PagingVertexCache::Statistics stats;
            Debug::HitchTrace::Scope exactWork("paging.vertex_cache_order", Debug::HitchTrace::CpuTiming::Scoped);
            const bool reduced = exactPagingWorkReductionEnabled();
            const auto result = PagingVertexCache::optimize(input, vertices->getNumElements(), output, stop, stats, reduced);
            if (reduced)
            {
                ++ExactPagingWorkCounters::runs;
                ExactPagingWorkCounters::triangles.fetch_add(input.size()/3, std::memory_order_relaxed);
                ExactPagingWorkCounters::scoreUpdates.fetch_add(stats.scoreUpdates, std::memory_order_relaxed);
                ExactPagingWorkCounters::scoreReuses.fetch_add(stats.scoreReuses, std::memory_order_relaxed);
                ExactPagingWorkCounters::heapComparisons.fetch_add(stats.heapComparisons, std::memory_order_relaxed);
                ExactPagingWorkCounters::unchangedHeapUpdates.fetch_add(stats.unchangedHeapUpdates, std::memory_order_relaxed);
            }
            exactWork.setMetric(0, "exact_reduction", reduced);
            exactWork.setMetric(1, "triangles", input.size() / 3);
            exactWork.setMetric(2, "score_updates", stats.scoreUpdates);
            exactWork.setMetric(3, "score_reuses", stats.scoreReuses);
            exactWork.setMetric(4, "heap_comparisons", stats.heapComparisons);
            exactWork.setMetric(5, "unchanged_heap_updates", stats.unchangedHeapUpdates);
            if (result == PagingVertexCache::Result::Cancelled) throw PagingWorkCancelled{};
            if (result == PagingVertexCache::Result::Invalid)
                throw std::runtime_error("Invalid index data in paging vertex optimization");
            PagingWorkScope::checkpoint();
            osg::ref_ptr<osg::DrawElements> elements;
            if (vertices->getNumElements() < 65536)
            {
                osg::ref_ptr<osg::DrawElementsUShort> compact = new osg::DrawElementsUShort(GL_TRIANGLES);
                compact->reserve(output.size());
                for (std::size_t i = 0; i < output.size(); ++i)
                {
                    if ((i & 1023) == 0) PagingWorkScope::checkpoint();
                    compact->push_back(static_cast<GLushort>(output[i]));
                }
                elements = compact;
            }
            else
                elements = new osg::DrawElementsUInt(GL_TRIANGLES, output.begin(), output.end());
            if (geom->getUseVertexBufferObjects()) elements->setElementBufferObject(new osg::ElementBufferObject);
            PagingWorkScope::checkpoint();
            geom->setPrimitiveSetList({elements});
            geom->dirtyGLObjects();
        }
    }
}
