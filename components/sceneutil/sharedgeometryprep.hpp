#ifndef OPENMW_SCENEUTIL_SHAREDGEOMETRYPREP_H
#define OPENMW_SCENEUTIL_SHAREDGEOMETRYPREP_H

#include <osg/Array>
#include <osg/BufferObject>
#include <osg/Geometry>
#include <osg/RenderInfo>
#include <osg/State>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace SceneUtil::SharedGeometryPrep
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* v = std::getenv("OPENMW_P9_SHARED_PREP");
            return v && std::strcmp(v, "1") == 0;
        }();
        return value;
    }
    struct Result
    {
        std::uint64_t submitted = 0, bytes = 0, clean = 0, skipped = 0;
    };
    struct Counters
    {
        std::atomic_uint64_t calls{0}, submitted{0}, bytes{0}, clean{0}, skipped{0};
        static Counters& instance() { static Counters result; return result; }
        void add(const Result& r)
        {
            calls.fetch_add(1, std::memory_order_relaxed);
            submitted.fetch_add(r.submitted, std::memory_order_relaxed);
            bytes.fetch_add(r.bytes, std::memory_order_relaxed);
            clean.fetch_add(r.clean, std::memory_order_relaxed);
            skipped.fetch_add(r.skipped, std::memory_order_relaxed);
        }
        ~Counters()
        {
            const char* path = std::getenv("OPENMW_P9_SHARED_PREP_FILE");
            if (!path || !*path) return;
            try
            {
                std::ofstream out(path);
                out << "calls=" << calls.load() << "\nsubmitted_buffers=" << submitted.load()
                    << "\nsubmitted_bytes=" << bytes.load() << "\nclean_buffers=" << clean.load()
                    << "\nskipped_buffers=" << skipped.load()
                    << "\nscope=shared_static_buffer_preparation_not_private_pose_or_GPU_completion\n";
            }
            catch (...) {}
        }
    };

    // Called only by the existing context-owned compile operation. The retained
    // source template owns these arrays; neither live CPU pose nor private VBOs
    // are visited. Existing ICO program/texture collection remains responsible
    // for materials -- do not repeat arbitrary StateSet application here.
    inline Result compile(osg::RenderInfo& info, const osg::Geometry& source, bool rigged)
    {
        Result result;
        osg::State* state = info.getState();
        if (!state) return result;
        constexpr std::uint64_t byteLimit = 2u * 1024u * 1024u;
        std::array<osg::BufferObject*, 32> seen{};
        unsigned count = 0;
        auto prepare = [&](osg::BufferObject* buffer) {
            if (!buffer) return;
            for (unsigned i = 0; i < count; ++i) if (seen[i] == buffer) return;
            if (count == seen.size()) { ++result.skipped; return; }
            seen[count++] = buffer;
            if (buffer->getUsage() != GL_STATIC_DRAW_ARB
                || (buffer->getTarget() != GL_ARRAY_BUFFER_ARB && buffer->getTarget() != GL_ELEMENT_ARRAY_BUFFER_ARB))
            { ++result.skipped; return; }
            for (unsigned i = 0; i < buffer->getNumBufferData(); ++i)
            {
                const auto* data = buffer->getBufferData(i);
                if (!data || !data->getDataPointer() || !data->getTotalDataSize()
                    || data->getDataVariance() == osg::Object::DYNAMIC)
                { ++result.skipped; return; }
            }
            const auto size = static_cast<std::uint64_t>(buffer->computeRequiredBufferSize());
            if (!size || size > byteLimit || result.bytes > byteLimit - size)
            { ++result.skipped; return; }
            auto* gl = buffer->getOrCreateGLBufferObject(state->getContextID());
            if (!gl) { ++result.skipped; return; }
            if (!gl->isDirty()) { ++result.clean; return; }
            // compileBuffer retains OSG's revision tracking. No forced clear,
            // no private storage renaming, no CPU mutation and no GPU wait.
            state->unbindVertexBufferObject();
            state->unbindElementBufferObject();
            gl->compileBuffer();
            state->unbindVertexBufferObject();
            state->unbindElementBufferObject();
            ++result.submitted;
            result.bytes += size;
        };
        auto array = [&](const osg::Array* value) {
            if (value) prepare(const_cast<osg::BufferObject*>(value->getBufferObject()));
        };
        // Positions are private in both consumers; rig normals/tangents are
        // private too. A shared UV/color VBO may co-pack immutable bind-pose data;
        // that whole static allocation is prepared, never an evaluated pose.
        if (!rigged) array(source.getNormalArray());
        array(source.getColorArray()); array(source.getSecondaryColorArray()); array(source.getFogCoordArray());
        for (unsigned i = 0; i < source.getNumTexCoordArrays(); ++i)
            if (!rigged || i != 7) array(source.getTexCoordArray(i));
        for (unsigned i = 0; i < source.getNumVertexAttribArrays(); ++i) array(source.getVertexAttribArray(i));
        for (const auto& primitive : source.getPrimitiveSetList())
            if (auto* elements = primitive->getDrawElements()) prepare(elements->getElementBufferObject());
        return result;
    }
    inline void prepare(osg::RenderInfo& info, const osg::Geometry* source, bool rigged)
    {
        if (enabled() && source) Counters::instance().add(compile(info, *source, rigged));
    }
}
#endif
