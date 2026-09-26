#ifndef OPENMW_COMPONENTS_DEBUG_P8DYNAMICDRAWTELEMETRY_H
#define OPENMW_COMPONENTS_DEBUG_P8DYNAMICDRAWTELEMETRY_H

#include "v3diagnostics.hpp"

#include <osg/Array>
#include <osg/BufferObject>
#include <osg/Drawable>
#include <osg/Geometry>
#include <osg/NodeVisitor>
#include <osg/PrimitiveSet>
#include <osg/RenderInfo>
#include <osg/State>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace Debug::P8DynamicDrawTelemetry
{
    struct GeometryStats
    {
        std::uint64_t vertices = 0;
        std::uint64_t primitiveSets = 0;
        std::uint64_t bufferObjects = 0;
        std::uint64_t bufferBytes = 0;
        std::uint64_t dynamicBufferBytes = 0;
        unsigned int maxModifiedCount = 0;
    };

    inline GeometryStats inspect(const osg::Drawable* drawable)
    {
        GeometryStats result;
        const auto* geometry = drawable ? drawable->asGeometry() : nullptr;
        if (!geometry)
            return result;

        if (const osg::Array* vertices = geometry->getVertexArray())
            result.vertices = vertices->getNumElements();
        result.primitiveSets = geometry->getNumPrimitiveSets();

        std::unordered_set<const osg::BufferObject*> buffers;
        auto addData = [&](const osg::BufferData* data) {
            if (!data)
                return;
            result.maxModifiedCount = (std::max)(result.maxModifiedCount, data->getModifiedCount());
            const osg::BufferObject* buffer = data->getBufferObject();
            if (!buffer || !buffers.insert(buffer).second)
                return;
            const std::uint64_t bytes = buffer->computeRequiredBufferSize();
            ++result.bufferObjects;
            result.bufferBytes += bytes;
            const GLenum usage = buffer->getUsage();
            if (usage == GL_DYNAMIC_DRAW_ARB || usage == GL_STREAM_DRAW_ARB
                || usage == GL_DYNAMIC_COPY_ARB || usage == GL_STREAM_COPY_ARB)
                result.dynamicBufferBytes += bytes;
        };

        addData(geometry->getVertexArray());
        addData(geometry->getNormalArray());
        addData(geometry->getColorArray());
        for (unsigned int i = 0; i < geometry->getNumPrimitiveSets(); ++i)
            addData(geometry->getPrimitiveSet(i));
        return result;
    }

    inline unsigned int currentModifiedCount(const osg::Drawable* drawable)
    {
        const auto* geometry = drawable ? drawable->asGeometry() : nullptr;
        if (!geometry)
            return 0;
        unsigned int value = 0;
        auto sample = [&](const osg::BufferData* data) {
            if (data)
                value = (std::max)(value, data->getModifiedCount());
        };
        sample(geometry->getVertexArray());
        sample(geometry->getNormalArray());
        sample(geometry->getColorArray());
        return value;
    }

    struct ThreadFrameAggregate
    {
        unsigned int frame = std::numeric_limits<unsigned int>::max();
        std::uint64_t draws = 0;
        std::uint64_t slowDraws = 0;
        double totalMs = 0.0;
        double maxMs = 0.0;
        std::uint64_t bufferBytes = 0;
        std::uint64_t dynamicBufferBytes = 0;
    };

    inline void flushAggregate(ThreadFrameAggregate& value)
    {
        if (value.frame == std::numeric_limits<unsigned int>::max() || value.draws == 0)
            return;
        auto& writer = V3Diagnostics::p8DynamicFrameWriter();
        if (!writer.enabled())
            return;

        std::ostringstream row;
        row << value.frame << ',' << V3Diagnostics::epochMs() << ',' << V3Diagnostics::threadId() << ','
            << value.draws << ',' << std::fixed << std::setprecision(3)
            << value.totalMs << ',' << value.maxMs << ',' << value.slowDraws << ','
            << value.bufferBytes << ',' << value.dynamicBufferBytes;
        writer.writeLine(row.str());
    }

    inline void accumulate(unsigned int frame, double durationMs, const GeometryStats& stats)
    {
        static thread_local ThreadFrameAggregate aggregate;
        if (aggregate.frame != frame)
        {
            flushAggregate(aggregate);
            aggregate = {};
            aggregate.frame = frame;
        }
        ++aggregate.draws;
        aggregate.totalMs += durationMs;
        aggregate.maxMs = (std::max)(aggregate.maxMs, durationMs);
        if (durationMs >= 0.25)
            ++aggregate.slowDraws;
        aggregate.bufferBytes += stats.bufferBytes;
        aggregate.dynamicBufferBytes += stats.dynamicBufferBytes;
    }

    class TimedDrawCallback final : public osg::Drawable::DrawCallback
    {
    public:
        TimedDrawCallback() = default;

        TimedDrawCallback(std::string kind, const osg::Drawable* drawable, osg::Drawable::DrawCallback* inner)
            : mKind(std::move(kind))
            , mInner(inner)
            , mStats(inspect(drawable))
        {
        }

        TimedDrawCallback(const TimedDrawCallback& rhs, const osg::CopyOp& copyop)
            : osg::Object(rhs, copyop)
            , osg::Drawable::DrawCallback(rhs, copyop)
            , mKind(rhs.mKind)
            , mInner(rhs.mInner)
            , mStats(rhs.mStats)
        {
        }

        META_Object(Debug, TimedDrawCallback);

        void drawImplementation(osg::RenderInfo& renderInfo, const osg::Drawable* drawable) const override
        {
            unsigned int frame = V3HitchTelemetry::currentFrame();
            if (renderInfo.getState() && renderInfo.getState()->getFrameStamp())
                frame = renderInfo.getState()->getFrameStamp()->getFrameNumber();

            const auto start = V3Diagnostics::Clock::now();
            if (mInner)
                mInner->drawImplementation(renderInfo, drawable);
            else if (drawable)
                drawable->drawImplementation(renderInfo);
            const double durationMs = V3Diagnostics::elapsedMs(start);

            accumulate(frame, durationMs, mStats);

            auto& writer = V3Diagnostics::p8DynamicDrawWriter();
            if (!writer.enabled() || durationMs < 0.20)
                return;

            std::ostringstream row;
            row << frame << ',' << V3Diagnostics::epochMs() << ',' << V3Diagnostics::threadId() << ','
                << V3Diagnostics::csvQuote(mKind) << ','
                << V3Diagnostics::csvQuote(drawable ? drawable->className() : "null") << ','
                << V3Diagnostics::csvQuote(drawable ? drawable->getName() : std::string()) << ','
                << std::fixed << std::setprecision(3) << durationMs << ','
                << (drawable ? static_cast<int>(drawable->getDataVariance()) : -1) << ','
                << mStats.vertices << ',' << mStats.primitiveSets << ',' << mStats.bufferObjects << ','
                << mStats.bufferBytes << ',' << mStats.dynamicBufferBytes << ','
                << currentModifiedCount(drawable);
            writer.writeLine(row.str());
        }

    private:
        std::string mKind;
        osg::ref_ptr<osg::Drawable::DrawCallback> mInner;
        GeometryStats mStats;
    };

    inline bool enabled()
    {
        return V3Diagnostics::p8DynamicDrawWriter().enabled()
            || V3Diagnostics::p8DynamicFrameWriter().enabled();
    }

    inline void install(osg::Drawable& drawable, std::string_view kind)
    {
        if (!enabled())
            return;
        if (dynamic_cast<TimedDrawCallback*>(drawable.getDrawCallback()))
            return;
        drawable.setDrawCallback(new TimedDrawCallback(std::string(kind), &drawable, drawable.getDrawCallback()));
    }

    class InstallVisitor final : public osg::NodeVisitor
    {
    public:
        InstallVisitor()
            : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN)
        {
        }

        void apply(osg::Drawable& drawable) override
        {
            if (drawable.getDataVariance() == osg::Object::DYNAMIC)
                install(drawable, "scene_dynamic");
            traverse(drawable);
        }

        void apply(osg::Geometry& geometry) override
        {
            if (geometry.getDataVariance() == osg::Object::DYNAMIC)
                install(geometry, "scene_dynamic");
            traverse(geometry);
        }
    };

    inline void recordDeformation(std::string_view kind, std::string_view name, double durationMs,
        std::uint64_t vertices, std::uint64_t units, std::uint64_t bufferBytes)
    {
        auto& writer = V3Diagnostics::p8DeformWriter();
        if (!writer.enabled() || durationMs < 0.10)
            return;

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
            << V3Diagnostics::threadId() << ',' << V3Diagnostics::csvQuote(kind) << ','
            << V3Diagnostics::csvQuote(name) << ',' << std::fixed << std::setprecision(3) << durationMs << ','
            << vertices << ',' << units << ',' << bufferBytes;
        writer.writeLine(row.str());
    }
}

#endif
