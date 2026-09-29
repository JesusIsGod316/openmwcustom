#ifndef OPENMW_SCENEUTIL_GLCALLTRACE_H
#define OPENMW_SCENEUTIL_GLCALLTRACE_H

#include <osg/GLExtensions>
#include <array>
#include <algorithm>
#include <string>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <tuple>
#include <type_traits>

namespace SceneUtil::GLCallTrace
{
    // Diagnostic CPU envelopes only. Direct core GL calls and driver internals
    // are NOT intercepted. No query, flush, error polling or wait is inserted.
    enum Phase : unsigned { Outside, RendererDraw, Matrices, State, Drawable, Retire, CullDraw, Compile };
    struct Location { unsigned context=0, frame=0, phase=Outside; std::uintptr_t drawable=0; };
    inline thread_local Location location;
    struct Scope
    {
        Location previous;
        explicit Scope(Location next) : previous(location) { location=next; }
        ~Scope() { location=previous; }
    };
    enum Api : unsigned
    {
        BufferData, BufferSubData, MapBufferRange, UnmapBuffer, UseProgram, LinkProgram,
        CompileShader, GetProgramiv, GetShaderiv, CompressedTexImage2D, CompressedTexSubImage2D,
        DrawElementsInstanced, DrawArraysInstanced, DrawRangeElements,
        GetQueryObjectiv, GetQueryObjectuiv, GetQueryObjectui64v, Count
    };
    inline constexpr std::array<const char*,Count> names={
        "glBufferData","glBufferSubData","glMapBufferRange","glUnmapBuffer","glUseProgram","glLinkProgram",
        "glCompileShader","glGetProgramiv","glGetShaderiv","glCompressedTexImage2D","glCompressedTexSubImage2D",
        "glDrawElementsInstanced","glDrawArraysInstanced","glDrawRangeElements",
        "glGetQueryObjectiv","glGetQueryObjectuiv","glGetQueryObjectui64v"};
    using Clock=std::chrono::steady_clock;
    struct Row { Location where; unsigned api=0; std::uint64_t arg0=0,arg1=0; double ms=0; std::int64_t startNs=0; };
    struct Total { std::uint64_t calls=0; double ms=0, maximum=0; };
    struct Frame { unsigned frame=0,context=0;std::array<Total,Count> totals{}; };
    class Capture
    {
    public:
        static Capture& instance() { static Capture value; return value; }
        void enable(const std::string& path)
        {
            if (!mRows) { mPath=path; mRows=std::make_unique<std::array<Row,32768>>();
                mFrameRows=std::make_unique<std::array<Frame,32768>>(); }
        }
        void append(const Row& row)
        {
            if (!mRows || row.where.context>=16) return;
            auto& t=mTotals[row.where.context][row.api];
            ++t.calls; t.ms+=row.ms; t.maximum=(std::max)(t.maximum,row.ms);
            if(row.where.frame)
            {
                auto& f=mCurrent[row.where.context];
                if(f.frame && f.frame!=row.where.frame) save(f);
                f.frame=row.where.frame;f.context=row.where.context;
                auto& t=f.totals[row.api];++t.calls;t.ms+=row.ms;t.maximum=(std::max)(t.maximum,row.ms);
            }
            if (row.ms<.20) return;
            const auto i=mNext.fetch_add(1,std::memory_order_relaxed);
            if (i<mRows->size()) (*mRows)[i]=row;
        }
        Total total(unsigned context,Api api) const { return mTotals[context][api]; }
        ~Capture()
        {
            if (!mRows) return;
            try
            {
                std::ofstream out(mPath+".gl.csv");
                out << "frame,context,phase,drawable,api,arg0,arg1,cpu_ms,start_ns\n";
                for(std::size_t i=0;i<(std::min)(mNext.load(),mRows->size());++i)
                {
                    const auto& a=(*mRows)[i];
                    out<<a.where.frame<<','<<a.where.context<<','<<a.where.phase<<','<<a.where.drawable<<','
                        <<names[a.api]<<','<<a.arg0<<','<<a.arg1<<','<<a.ms<<','<<a.startNs<<'\n';
                }
                std::ofstream totals(mPath+".gl-totals.csv");
                totals<<"context,api,calls,cpu_ms,max_ms\n";
                for(unsigned c=0;c<16;++c) for(unsigned a=0;a<Count;++a)
                {
                    const auto& t=mTotals[c][a];
                    if(t.calls) totals<<c<<','<<names[a]<<','<<t.calls<<','<<t.ms<<','<<t.maximum<<'\n';
                }
                for(auto& f:mCurrent) if(f.frame) save(f);
                std::ofstream frames(mPath+".gl-frames.csv");
                frames<<"frame,context,api,calls,cpu_ms,max_ms\n";
                for(std::size_t i=0;i<(std::min)(mFrameNext.load(),mFrameRows->size());++i)
                { const auto& f=(*mFrameRows)[i];for(unsigned a=0;a<Count;++a) {
                    const auto& t=f.totals[a];if(t.calls)frames<<f.frame<<','<<f.context<<','<<names[a]<<','<<t.calls<<','<<t.ms<<','<<t.maximum<<'\n'; } }
                std::ofstream status(mPath+".gl-status.txt");
                status<<"rows_dropped="<<(mNext>mRows->size()?mNext-mRows->size():0)
                    <<"\nframe_rows_dropped="<<(mFrameNext>mFrameRows->size()?mFrameNext-mFrameRows->size():0)
                    <<"\nscope=selected_OSG_extension_dispatch_only_not_direct_core_GL_or_driver_internals\n"
                    <<"phase=0_outside_1_renderer_draw_2_matrices_3_state_4_drawable_5_retire_6_cull_draw_7_compile\n"
                    <<"frame0_in_outer_scopes=unassigned_use_time_and_leaf_frame_not_assumed_alignment\n";
            } catch (...) {}
        }
    private:
        void save(Frame& f)
        { const auto i=mFrameNext.fetch_add(1,std::memory_order_relaxed);
            if(i<mFrameRows->size()) (*mFrameRows)[i]=f;
            f={}; }
        std::array<Frame,16> mCurrent{};
        std::unique_ptr<std::array<Frame,32768>> mFrameRows;
        std::atomic<std::size_t> mFrameNext{0};
        std::string mPath;
        std::unique_ptr<std::array<Row,32768>> mRows;
        std::atomic<std::size_t> mNext{0};
        std::array<std::array<Total,Count>,16> mTotals{};
    };

    template<std::size_t N,typename Tuple> std::uint64_t argument(const Tuple& args)
    {
        if constexpr(N<std::tuple_size_v<Tuple>)
        {
            using T=std::remove_cv_t<std::remove_reference_t<decltype(std::get<N>(args))>>;
            if constexpr(std::is_integral_v<T>) return static_cast<std::uint64_t>(std::get<N>(args));
        }
        return 0;
    }
    template<unsigned Context,Api A,typename F> struct Hook;
    template<unsigned Context,Api A,typename R,typename... Args>
    struct Hook<Context,A,R(GL_APIENTRY *)(Args...)>
    {
        using Function=R(GL_APIENTRY *)(Args...);
        inline static Function original=nullptr;
        static R GL_APIENTRY call(Args... args)
        {
            // A context's function pointer has its own original dispatch. An
            // untraced caller remains a direct pass-through, including returns.
            if(location.phase==Outside || location.context!=Context) return original(args...);
            const auto start=Clock::now();
            struct Record
            {
                Clock::time_point start; std::tuple<Args...> args;
                ~Record()
                {
                    const double ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
                    Capture::instance().append({location,A,argument<0>(args),argument<1>(args),ms,
                        std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()});
                }
            } record{start,{args...}};
            return original(args...);
        }
        static void install(Function& target)
        {
            if(target && target!=&call) { original=target;target=&call; }
        }
    };
    template<unsigned C> void installContext(osg::GLExtensions& e)
    {
#define P9_HOOK(field, id) Hook<C,id,decltype(e.field)>::install(e.field)
        P9_HOOK(glBufferData,BufferData); P9_HOOK(glBufferSubData,BufferSubData);
        P9_HOOK(glMapBufferRange,MapBufferRange); P9_HOOK(glUnmapBuffer,UnmapBuffer);
        P9_HOOK(glUseProgram,UseProgram); P9_HOOK(glLinkProgram,LinkProgram);
        P9_HOOK(glCompileShader,CompileShader); P9_HOOK(glGetProgramiv,GetProgramiv);
        P9_HOOK(glGetShaderiv,GetShaderiv); P9_HOOK(glCompressedTexImage2D,CompressedTexImage2D);
        P9_HOOK(glCompressedTexSubImage2D,CompressedTexSubImage2D);
        P9_HOOK(glDrawElementsInstanced,DrawElementsInstanced); P9_HOOK(glDrawArraysInstanced,DrawArraysInstanced);
        P9_HOOK(glDrawRangeElements,DrawRangeElements); P9_HOOK(glGetQueryObjectiv,GetQueryObjectiv);
        P9_HOOK(glGetQueryObjectuiv,GetQueryObjectuiv); P9_HOOK(glGetQueryObjectui64v,GetQueryObjectui64v);
#undef P9_HOOK
    }
    inline bool install(unsigned context,osg::GLExtensions& e)
    {
        // Called only by the owning graphics thread at its renderer entry,
        // before any GL dispatch in that operation. No second/shared context.
#define P9_CONTEXT(n) case n:installContext<n>(e);return true
        switch(context) {
            P9_CONTEXT(0); P9_CONTEXT(1); P9_CONTEXT(2); P9_CONTEXT(3);
            P9_CONTEXT(4); P9_CONTEXT(5); P9_CONTEXT(6); P9_CONTEXT(7);
            P9_CONTEXT(8); P9_CONTEXT(9); P9_CONTEXT(10); P9_CONTEXT(11);
            P9_CONTEXT(12); P9_CONTEXT(13); P9_CONTEXT(14); P9_CONTEXT(15);
            default:return false;
        }
#undef P9_CONTEXT
    }
}
#endif
