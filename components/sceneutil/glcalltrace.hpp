#ifndef OPENMW_SCENEUTIL_GLCALLTRACE_H
#define OPENMW_SCENEUTIL_GLCALLTRACE_H

#include <osg/FrameStamp>
#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/State>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace SceneUtil::GLCallTrace
{
    // These are OSG extension-dispatch calls, not interception of every OpenGL
    // entry point. In particular core glBindTexture/glTexImage2D/glDrawElements
    // in a prebuilt OSG library remain in the enclosing leaf/stage envelope.
#define P9_GL_TRACE_CALLS(X) \
    X(glCompileShader, Program) X(glLinkProgram, Program) X(glUseProgram, Program) \
    X(glUniform1f, Uniform) X(glUniform1i, Uniform) X(glUniform4fv, Uniform) X(glUniformMatrix4fv, Uniform) \
    X(glGenBuffers, Buffer) X(glDeleteBuffers, Buffer) X(glBindBuffer, Buffer) \
    X(glBufferData, Buffer) X(glBufferSubData, Buffer) X(glMapBuffer, Buffer) \
    X(glMapBufferRange, Buffer) X(glUnmapBuffer, Buffer) \
    X(glBindBufferBase, Buffer) X(glBindBufferRange, Buffer) \
    X(glCompressedTexImage2D, Texture) X(glCompressedTexSubImage2D, Texture) \
    X(glTexStorage2D, Texture) X(glGenerateMipmap, Texture) \
    X(glBindFramebuffer, Framebuffer) X(glFramebufferTexture2D, Framebuffer) \
    X(glBlitFramebuffer, Framebuffer) X(glCheckFramebufferStatus, Framebuffer) \
    X(glBindVertexArray, Buffer) X(glDrawArraysInstanced, Draw) \
    X(glDrawElementsInstanced, Draw) X(glDrawRangeElements, Draw) \
    X(glGetQueryObjectiv, Query) X(glGetQueryObjectuiv, Query) X(glGetQueryObjectui64v, Query) \
    X(glClientWaitSync, Query) X(glWaitSync, Query) X(glMemoryBarrier, Other)

    enum class Category : unsigned { Buffer, Program, Texture, Uniform, Draw, Framebuffer, Query, Other, Count };
    enum class Phase : unsigned { OutsideLeaf, Matrices, State, Draw, Retire };
    enum class Api : unsigned {
#define P9_ENUM(name, category) name,
        P9_GL_TRACE_CALLS(P9_ENUM)
#undef P9_ENUM
        Count
    };
    inline const char* name(Api api)
    {
        switch (api) {
#define P9_NAME(n, category) case Api::n: return #n;
            P9_GL_TRACE_CALLS(P9_NAME)
#undef P9_NAME
            default: return "unknown";
        }
    }
    inline Category category(Api api)
    {
        switch (api) {
#define P9_CATEGORY(n, cat) case Api::n: return Category::cat;
            P9_GL_TRACE_CALLS(P9_CATEGORY)
#undef P9_CATEGORY
            default: return Category::Other;
        }
    }
    inline const char* categoryName(unsigned c)
    {
        static constexpr const char* names[]{"buffer", "program", "texture", "uniform", "draw", "framebuffer", "query", "other"};
        return c < static_cast<unsigned>(Category::Count) ? names[c] : "unknown";
    }
    struct Location { std::uint64_t drawable = 0; Phase phase = Phase::OutsideLeaf; };
    inline thread_local Location location;
    struct LeafScope
    {
        Location previous = location;
        explicit LeafScope(std::uint64_t drawable) { location = {drawable, Phase::Matrices}; }
        void phase(Phase p) { location.phase = p; }
        ~LeafScope() { location = previous; }
    };
    using Clock = std::chrono::steady_clock;
    struct Counter { std::uint64_t calls = 0; double ms = 0, maximum = 0; };
    struct Frame
    {
        unsigned frame = 0, context = 0, generation = 0;
        bool valid = false;
        std::array<Counter, static_cast<unsigned>(Category::Count)> buckets{};
    };
    struct Row
    {
        unsigned frame = 0, context = 0, generation = 0;
        std::uint64_t drawable = 0;
        Phase phase = Phase::OutsideLeaf;
        Api api = Api::Count;
        double beginMs = 0, durationMs = 0;
        std::array<std::int64_t, 3> args{};
    };
    class Capture
    {
    public:
        static constexpr unsigned Contexts = 16;
        static Capture& instance() { static Capture value; return value; }
        bool enabled() const { return static_cast<bool>(mRows); }
        void registerContext(osg::State& state, const osg::GLExtensions* ext, unsigned installed)
        {
            const unsigned id = state.getContextID();
            if (id >= Contexts) { mUnsupported.fetch_add(1); return; }
            if (mStates[id] != &state || mExtensions[id] != ext) ++mGeneration[id];
            mStates[id] = &state; mExtensions[id] = ext; mInstalled[id] = installed;
        }
        bool installed(unsigned id) const { return id < Contexts && mInstalled[id] != 0; }
        Row begin(unsigned id, Api api) const
        {
            Row row;
            row.context = id; row.api = api; row.drawable = location.drawable; row.phase = location.phase;
            if (id < Contexts)
            {
                row.generation = mGeneration[id];
                const auto* stamp = mStates[id] ? mStates[id]->getFrameStamp() : nullptr;
                row.frame = stamp ? stamp->getFrameNumber() : 0;
            }
            row.beginMs = std::chrono::duration<double, std::milli>(Clock::now() - mStart).count();
            return row;
        }
        void end(Row row)
        {
            row.durationMs = std::chrono::duration<double, std::milli>(Clock::now() - mStart).count() - row.beginMs;
            if (row.context >= Contexts) return;
            // Each context has one GL owner, independent of whether that owner
            // is currently the main or the graphics thread. No per-call locks.
            auto& frame = mCurrent[row.context];
            if (!frame.valid || frame.frame != row.frame || frame.generation != row.generation)
            {
                flush(frame);
                frame = {}; frame.valid = true; frame.frame = row.frame;
                frame.context = row.context; frame.generation = row.generation;
            }
            auto add = [&](Counter& c) { ++c.calls; c.ms += row.durationMs; c.maximum = (std::max)(c.maximum, row.durationMs); };
            add(frame.buckets[static_cast<unsigned>(category(row.api))]);
            add(mTotals[row.context][static_cast<unsigned>(row.api)]);
            if (row.durationMs < 0.20) return;
            const auto next = mNext.fetch_add(1, std::memory_order_relaxed);
            if (next < Capacity) (*mRows)[next] = row;
        }
        std::uint64_t calls(unsigned context, Api api) const
        {
            return context < Contexts ? mTotals[context][static_cast<unsigned>(api)].calls : 0;
        }
        ~Capture()
        {
            if (!enabled()) return;
            // Only reached after engine graphics threads have been joined.
            for (auto& f : mCurrent) flush(f);
            try
            {
                std::ofstream out(mPath);
                out << "frame,context,generation,drawable,phase,api,begin_ms,duration_ms,arg0,arg1,arg2\n" << std::setprecision(10);
                const auto n = mNext.load();
                for (std::size_t i = 0; i < (std::min)(n, Capacity); ++i)
                {
                    const auto& r = (*mRows)[i];
                    out << r.frame << ',' << r.context << ',' << r.generation << ',' << r.drawable << ','
                        << static_cast<unsigned>(r.phase) << ',' << name(r.api) << ',' << r.beginMs << ',' << r.durationMs;
                    for (auto a : r.args) out << ',' << a;
                    out << '\n';
                }
                std::ofstream frames(mPath + ".frames.csv");
                frames << "frame,context,generation,category,calls,total_ms,max_call_ms\n" << std::setprecision(10);
                for (std::size_t i = 0; i < (std::min)(mFrameNext.load(), FrameCapacity); ++i)
                {
                    const auto& f = (*mFrames)[i];
                    for (unsigned c = 0; c < f.buckets.size(); ++c)
                    {
                        const auto& b = f.buckets[c];
                        if (b.calls) frames << f.frame << ',' << f.context << ',' << f.generation << ',' << categoryName(c)
                            << ',' << b.calls << ',' << b.ms << ',' << b.maximum << '\n';
                    }
                }
                std::ofstream totals(mPath + ".totals.csv");
                totals << "context,api,calls,total_ms,max_call_ms\n" << std::setprecision(10);
                std::uint64_t allCalls = 0;
                for (unsigned id = 0; id < Contexts; ++id)
                    for (unsigned a = 0; a < static_cast<unsigned>(Api::Count); ++a)
                    {
                        const auto& t = mTotals[id][a]; allCalls += t.calls;
                        if (t.calls) totals << id << ',' << name(static_cast<Api>(a)) << ',' << t.calls << ',' << t.ms << ',' << t.maximum << '\n';
                    }
                std::ofstream status(mPath + ".status.txt");
                status << "calls=" << allCalls << "\nrows_dropped=" << (n > Capacity ? n - Capacity : 0)
                    << "\nframes_dropped=" << (mFrameNext > FrameCapacity ? mFrameNext - FrameCapacity : 0)
                    << "\nunsupported_contexts=" << mUnsupported.load()
                    << "\nscope=selected_OSG_extension_dispatch_CPU_calls_including_driver_wait_not_all_GL_or_GPU_time\n"
                    << "phase_legend=0_outside_leaf_1_matrices_2_state_3_draw_4_retire\n";
                for (unsigned id = 0; id < Contexts; ++id)
                    if (mInstalled[id]) status << "context=" << id << " hooks=" << mInstalled[id] << " generation=" << mGeneration[id] << '\n';
            }
            catch (...) {}
        }
    private:
        static constexpr std::size_t Capacity = 32768, FrameCapacity = 32768;
        Capture()
        {
            if (const char* file = std::getenv("OPENMW_P9_GL_TRACE_FILE"); file && *file)
            {
                mPath = file;
                mRows = std::make_unique<std::array<Row, Capacity>>();
                mFrames = std::make_unique<std::array<Frame, FrameCapacity>>();
            }
        }
        void flush(Frame& f)
        {
            if (!f.valid || !mFrames) return;
            const auto next = mFrameNext.fetch_add(1, std::memory_order_relaxed);
            if (next < FrameCapacity) (*mFrames)[next] = f;
            f.valid = false;
        }
        std::string mPath;
        Clock::time_point mStart = Clock::now();
        std::unique_ptr<std::array<Row, Capacity>> mRows;
        std::unique_ptr<std::array<Frame, FrameCapacity>> mFrames;
        std::array<osg::State*, Contexts> mStates{};
        std::array<const osg::GLExtensions*, Contexts> mExtensions{};
        std::array<unsigned, Contexts> mGeneration{}, mInstalled{};
        std::array<Frame, Contexts> mCurrent{};
        std::array<std::array<Counter, static_cast<unsigned>(Api::Count)>, Contexts> mTotals{};
        std::atomic<std::size_t> mNext{0}, mFrameNext{0};
        std::atomic<unsigned> mUnsupported{0};
    };

    template<class T> inline std::int64_t argument(T value)
    {
        if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) return static_cast<std::int64_t>(value);
        else return 0; // pointers and floats are not resource identities/byte counts
    }
    template<auto Member, Api Value, class Signature> struct Entry;
    template<auto Member, Api Value, class R, class... Args> struct Entry<Member, Value, R (GL_APIENTRY *)(Args...)>
    {
        using Function = R (GL_APIENTRY *)(Args...);
        inline static std::array<Function, Capture::Contexts> original{};
        template<std::size_t Id> static R GL_APIENTRY call(Args... args)
        {
            auto& capture = Capture::instance();
            Row row = capture.begin(Id, Value);
            const auto tuple = std::tuple<Args...>(args...);
            if constexpr (sizeof...(Args) > 0) row.args[0] = argument(std::get<0>(tuple));
            if constexpr (sizeof...(Args) > 1) row.args[1] = argument(std::get<1>(tuple));
            if constexpr (sizeof...(Args) > 2) row.args[2] = argument(std::get<2>(tuple));
            // Call through the original ABI and preserve arguments and return
            // value exactly. No new GL calls, fences, queries, flush or finish.
            if constexpr (std::is_void_v<R>) { original[Id](args...); capture.end(row); }
            else { R result = original[Id](args...); capture.end(row); return result; }
        }
        template<std::size_t... I> static auto wrappers(std::index_sequence<I...>)
        { return std::array<Function, sizeof...(I)>{&call<I>...}; }
        static Function wrapper(unsigned id)
        { static const auto values = wrappers(std::make_index_sequence<Capture::Contexts>{}); return values[id]; }
        static bool install(unsigned id, osg::GLExtensions& ext)
        {
            if (!(ext.*Member)) return false;
            if (ext.*Member == wrapper(id)) return true;
            original[id] = ext.*Member; ext.*Member = wrapper(id); return true;
        }
        static void restore(unsigned id, osg::GLExtensions& ext)
        { if (ext.*Member == wrapper(id)) ext.*Member = original[id]; }
    };
    template<auto Member, Api Value> using Hook = Entry<Member, Value, std::remove_reference_t<decltype(std::declval<osg::GLExtensions&>().*Member)>>;
    inline unsigned install(osg::State& state)
    {
        auto& capture = Capture::instance();
        if (!capture.enabled() || state.getContextID() >= Capture::Contexts) return 0;
        auto* ext = state.get<osg::GLExtensions>();
        if (!ext) return 0;
        unsigned count = 0; const auto id = state.getContextID();
#define P9_INSTALL(n, cat) count += Hook<&osg::GLExtensions::n, Api::n>::install(id, *ext) ? 1 : 0;
        P9_GL_TRACE_CALLS(P9_INSTALL)
#undef P9_INSTALL
        capture.registerContext(state, ext, count);
        return count;
    }
    // Test/reconfiguration hook: invoke only on the stopped/current GL owner.
    inline void restore(osg::State& state)
    {
        if (state.getContextID() >= Capture::Contexts) return;
        if (auto* ext = state.get<osg::GLExtensions>())
        {
            const auto id = state.getContextID();
#define P9_RESTORE(n, cat) Hook<&osg::GLExtensions::n, Api::n>::restore(id, *ext);
            P9_GL_TRACE_CALLS(P9_RESTORE)
#undef P9_RESTORE
        }
    }
    struct InstallOperation final : osg::GraphicsOperation
    {
        InstallOperation() : osg::GraphicsOperation("Phase9 selected GL-call trace", false) {}
        void operator()(osg::GraphicsContext* context) override
        { if (context && context->getState()) install(*context->getState()); }
    };
#undef P9_GL_TRACE_CALLS
}
#endif
