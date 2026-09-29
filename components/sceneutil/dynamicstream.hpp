#ifndef OPENMW_SCENEUTIL_DYNAMICSTREAM_H
#define OPENMW_SCENEUTIL_DYNAMICSTREAM_H

#include <osg/Array>
#include <osg/BufferObject>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace SceneUtil::DynamicStream
{
    // A startup-only experiment. It neither changes OSG's CPU ownership barrier
    // nor assumes that two CPU geometries imply completion of the GPU's reads.
    inline bool enabled()
    {
        static const bool value = [] {
            const char* v = std::getenv("OPENMW_P9_DYNAMIC_STREAM");
            return v && std::strcmp(v, "1") == 0;
        }();
        return value;
    }

    enum class Result : std::uint8_t
    {
        Disabled, ColdOrClean, Refreshed, Ineligible, BudgetFallback
    };

    struct Outcome
    {
        Result result = Result::Disabled;
        std::uint64_t bytes = 0;
    };

    // GL objects stay owned by OSG. This is a per-draw-context admission bound,
    // not a promise about how many orphaned allocations the driver retains.
    struct Budget
    {
        unsigned frame = 0;
        bool initialized = false;
        std::uint64_t bytes = 0;
        bool admit(unsigned newFrame, std::uint64_t size)
        {
            constexpr std::uint64_t perBuffer = 4u * 1024u * 1024u;
            constexpr std::uint64_t perFrame = 32u * 1024u * 1024u;
            if (!initialized || frame != newFrame)
            {
                frame = newFrame;
                bytes = 0;
                initialized = true;
            }
            if (!size || size > perBuffer || bytes > perFrame - size)
                return false;
            bytes += size;
            return true;
        }
    };

    // Explicitly installed only on the private position/normal/tangent VBO
    // created by RigGeometry or the private position VBO from MorphGeometry.
    // No traversal, shared asset VBO discovery, or CPU-array mutation here.
    class PrivateBuffer
    {
    public:
        explicit PrivateBuffer(osg::VertexBufferObject* buffer) : mBuffer(buffer)
        {
            if (!buffer || buffer->getNumBufferData() > mArrays.size()) return;
            mCount = buffer->getNumBufferData();
            for (unsigned i = 0; i < mCount; ++i)
                mArrays[i] = buffer->getBufferData(i);
        }

        Outcome refresh(osg::State& state, const osg::Geometry& geometry, Budget& budget) const
        {
            if (!mBuffer || !geometry.getVertexArray()
                || geometry.getVertexArray()->getBufferObject() != mBuffer.get()
                || mBuffer->getTarget() != GL_ARRAY_BUFFER_ARB
                || mBuffer->getUsage() != GL_DYNAMIC_DRAW_ARB
                || !mCount || mBuffer->getNumBufferData() != mCount)
                return {Result::Ineligible, 0};

            // GLBufferObject::clear() in pinned OSG 3.6.5 initializes newly
            // rebuilt entry revisions to 0xffffff. Do not take this optional
            // path for that revision: normal OSG handles it without data loss.
            for (unsigned i = 0; i < mCount; ++i)
            {
                const auto* data = mBuffer->getBufferData(i);
                if (!data || data != mArrays[i] || !data->getDataPointer()
                    || !data->getTotalDataSize() || data->getModifiedCount() == 0xffffffu
                    || data->getBufferObject() != mBuffer.get())
                    return {Result::Ineligible, 0};
            }

            osg::GLBufferObject* glBuffer = mBuffer->getGLBufferObject(state.getContextID());
            if (!glBuffer || !glBuffer->getGLObjectID() || !glBuffer->isDirty())
                return {Result::ColdOrClean, 0};
            const auto& profile = glBuffer->getProfile();
            const auto required = mBuffer->computeRequiredBufferSize();
            if (profile._target != GL_ARRAY_BUFFER_ARB || !profile._size || required > profile._size)
                return {Result::Ineligible, 0};
            const auto* stamp = state.getFrameStamp();
            if (!stamp || !budget.admit(stamp->getFrameNumber(), profile._size))
                return {Result::BudgetFallback, 0};

            auto* ext = state.get<osg::GLExtensions>();
            if (!ext || !ext->glBindBuffer || !ext->glBufferData || !ext->glBufferSubData)
                return {Result::Ineligible, 0};

            // Invalidate OSG's binding cache before issuing a direct bind.
            // The GL name remains stable, so existing VAO attribute references
            // remain valid. Full contents are refilled before normal drawing.
            // Use the ARB spelling supplied by OSG on Windows GL headers.
            state.unbindVertexBufferObject();
            ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, glBuffer->getGLObjectID());
            ext->glBufferData(GL_ARRAY_BUFFER_ARB, profile._size, nullptr, GL_DYNAMIC_DRAW_ARB);
            glBuffer->clear();
            glBuffer->compileBuffer();
            ext->glBindBuffer(GL_ARRAY_BUFFER_ARB, 0);
            return {Result::Refreshed, profile._size};
        }

    private:
        osg::ref_ptr<osg::VertexBufferObject> mBuffer;
        std::array<const osg::BufferData*, 3> mArrays{};
        unsigned mCount = 0;
    };

    // Numeric capture is deliberately limited to this private stream boundary.
    // It is NOT a timer for inherited state application before the drawable.
    // No file output, string formatting, or growing containers during draw.
    class Capture
    {
    public:
        struct Row
        {
            std::uint64_t id = 0, bytes = 0;
            double prepareMs = 0, drawMs = 0;
            unsigned frame = 0, context = 0;
            Result result = Result::Disabled;
        };
        static Capture& instance() { static Capture value; return value; }
        bool active() const { return static_cast<bool>(mRows); }
        std::uint64_t registerGeometry(const char* kind, const std::string& name)
        {
            if (!active()) return 0;
            std::lock_guard<std::mutex> lock(mCatalogMutex);
            if (mCatalog.size() >= CatalogCapacity)
            {
                mCatalogDropped.fetch_add(1, std::memory_order_relaxed);
                return 0; // unknown diagnostic identity, never omit the draw
            }
            const std::uint64_t id = mCatalog.size() + 1;
            mCatalog.push_back(std::to_string(id) + "," + quote(kind ? kind : "unknown")
                + "," + quote(name.substr(0, 256)));
            return id;
        }
        void append(const Row& row)
        {
            if (!active()) return;
            mCalls.fetch_add(1, std::memory_order_relaxed);
            if (row.result == Result::Refreshed)
            {
                mRefreshes.fetch_add(1, std::memory_order_relaxed);
                mBytes.fetch_add(row.bytes, std::memory_order_relaxed);
            }
            if (row.prepareMs < .20 && row.drawMs < .20) return;
            const auto index = mNext.fetch_add(1, std::memory_order_relaxed);
            if (index < Capacity) (*mRows)[index] = row;
        }
        ~Capture()
        {
            if (!active()) return;
            // Engine shutdown joins its draw threads before static destruction.
            // Raw records remain bounded; terminal loss is reported explicitly.
            try
            {
                std::ofstream out(mPath);
                out << "frame,context,geometry,prepare_ms,draw_ms,result,bytes\n";
                const auto count = mNext.load();
                for (std::size_t i = 0; i < (std::min)(count, Capacity); ++i)
                {
                    const auto& r = (*mRows)[i];
                    out << r.frame << ',' << r.context << ',' << r.id << ',' << r.prepareMs
                        << ',' << r.drawMs << ',' << static_cast<unsigned>(r.result) << ',' << r.bytes << '\n';
                }
                std::ofstream catalog(mPath + ".catalog.csv");
                catalog << "geometry,kind,name\n";
                for (const auto& line : mCatalog) catalog << line << '\n';
                std::ofstream status(mPath + ".status.txt");
                status << "calls=" << mCalls.load() << "\nrefreshes=" << mRefreshes.load()
                    << "\nsubmitted_bytes=" << mBytes.load()
                    << "\nrows_dropped=" << (count > Capacity ? count - Capacity : 0)
                    << "\ncatalog_entries=" << mCatalog.size()
                    << "\ncatalog_dropped=" << mCatalogDropped.load()
                    << "\nscope=private_dynamic_stream_not_inherited_state_or_gpu_time\n";
            }
            catch (...) { /* no exceptions escape shutdown */ }
        }
    private:
        static constexpr std::size_t Capacity = 32768;
        static constexpr std::size_t CatalogCapacity = 65536;
        Capture()
        {
            if (const char* file = std::getenv("OPENMW_P9_DYNAMIC_TRACE_FILE"); file && *file)
            {
                mPath = file;
                mRows = std::make_unique<std::array<Row, Capacity>>();
            }
        }
        static std::string quote(const std::string& input)
        {
            std::string result = "\"";
            for (char c : input) { if (c == '"') result += '"'; result += c; }
            return result + '"';
        }
        std::string mPath;
        std::unique_ptr<std::array<Row, Capacity>> mRows;
        std::atomic<std::size_t> mNext{0};
        std::atomic<std::uint64_t> mCalls{0}, mRefreshes{0}, mBytes{0}, mCatalogDropped{0};
        std::mutex mCatalogMutex;
        std::vector<std::string> mCatalog;
    };

    class Callback final : public osg::Drawable::DrawCallback
    {
    public:
        Callback() : mPrivate(nullptr) {}
        Callback(osg::VertexBufferObject* buffer, osg::Drawable::DrawCallback* inner,
            bool refresh, const char* kind, const std::string& name)
            : mPrivate(buffer), mInner(inner), mRefresh(rhs.mRefresh), mId(rhs.mId) {}
        Callback(const Callback& rhs, const osg::CopyOp& op)
            : osg::Object(rhs, op), osg::Drawable::DrawCallback(rhs, op)
            , mPrivate(rhs.mPrivate), mInner(rhs.mInner), mRefresh(rhs.mRefresh), mId(rhs.mId) {}
        META_Object(SceneUtil, Callback)
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        {
            const auto* geometry = drawable ? drawable->asGeometry() : nullptr;
            auto* state = info.getState();
            auto& capture = Capture::instance();
            const bool timed = capture.active();
            using Clock = std::chrono::steady_clock;
            const auto begin = timed ? Clock::now() : Clock::time_point{};
            Outcome outcome;
            if (mRefresh && geometry && state)
            {
                // One thread owns each graphics context; keep admission across
                // all private buffers on that thread, without per-draw locks.
                thread_local std::array<Budget, 16> budgets;
                if (state->getContextID() < budgets.size())
                    outcome = mPrivate.refresh(*state, *geometry, budgets[state->getContextID()]);
                else outcome.result = Result::BudgetFallback;
            }
            const auto prepared = timed ? Clock::now() : Clock::time_point{};
            if (mInner) mInner->drawImplementation(info, drawable);
            else if (drawable) drawable->drawImplementation(info);
            if (timed && state)
            {
                const auto end = Clock::now();
                const auto* stamp = state->getFrameStamp();
                capture.append({mId, outcome.bytes,
                    std::chrono::duration<double, std::milli>(prepared - begin).count(),
                    std::chrono::duration<double, std::milli>(end - prepared).count(),
                    stamp ? stamp->getFrameNumber() : 0, state->getContextID(), outcome.result});
            }
        }
    private:
        PrivateBuffer mPrivate;
        osg::ref_ptr<osg::Drawable::DrawCallback> mInner;
        bool mRefresh = false;
        std::uint64_t mId = 0;
    };

    inline void install(osg::Geometry& geometry, osg::VertexBufferObject* privateBuffer, const char* kind)
    {
        if (!enabled() && !Capture::instance().active()) return;
        geometry.setDrawCallback(new Callback(privateBuffer, geometry.getDrawCallback(), enabled(), kind, geometry.getName()));
    }
}
#endif
