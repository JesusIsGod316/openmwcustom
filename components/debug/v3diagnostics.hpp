#ifndef OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H
#define OPENMW_COMPONENTS_DEBUG_V3DIAGNOSTICS_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <functional>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "v3hitchtelemetry.hpp"
#include "diagnostictransport.hpp"

namespace Debug::V3Diagnostics
{
    using Clock = std::chrono::steady_clock;

    inline double elapsedMs(Clock::time_point start, Clock::time_point end = Clock::now())
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    inline long long epochMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    inline std::size_t threadId()
    {
        return std::hash<std::thread::id>{}(std::this_thread::get_id());
    }

    class ScopedAccumulator
    {
    public:
        ScopedAccumulator(bool enabled, double& accumulator)
            : mEnabled(enabled)
            , mAccumulator(accumulator)
            , mStart(enabled ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScopedAccumulator()
        {
            if (mEnabled)
                mAccumulator += elapsedMs(mStart);
        }

        ScopedAccumulator(const ScopedAccumulator&) = delete;
        ScopedAccumulator& operator=(const ScopedAccumulator&) = delete;

    private:
        bool mEnabled;
        double& mAccumulator;
        Clock::time_point mStart;
    };

    inline bool pathEnabled(std::string_view value)
    {
        return !value.empty() && value != "0" && value != "off" && value != "false";
    }

    class CsvWriter
    {
    public:
        CsvWriter(const char* environmentVariable, std::string header)
            : mEnvironmentVariable(environmentVariable)
            , mHeader(std::move(header))
        {
        }

        bool enabled()
        {
            ensureOpen();
            return mEnabled.load(std::memory_order_acquire);
        }

        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mEnabled.load(std::memory_order_acquire))
                return;
            DiagnosticWriterHub::instance().enqueue(mChannel, line);
        }

        std::size_t droppedLines() const
        {
            return mChannel ? mChannel->mDroppedLines.load(std::memory_order_relaxed) : 0;
        }

    private:
        void ensureOpen()
        {
            if (mAttempted.load(std::memory_order_acquire))
                return;

            std::lock_guard<std::mutex> lock(mInitMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                try
                {
                    mPath = raw;
                    mChannel = DiagnosticWriterHub::instance().registerChannel(mPath, mHeader);
                    mEnabled.store(static_cast<bool>(mChannel), std::memory_order_release);
                }
                catch(const std::bad_alloc&)
                {
                    const char* transport=std::getenv("OPENMW_P9_CAPTURE_TRANSPORT");
                    if(!transport || std::string_view(transport)!="1")
                        throw;
                    // The requested output remains missing and therefore fails
                    // capture qualification. Do not retry allocation each frame
                    // or substitute synchronous producer-thread file output.
                    mEnabled.store(false,std::memory_order_release);
                }
            }

            mAttempted.store(true, std::memory_order_release);
        }

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        DiagnosticWriterHub::ChannelHandle mChannel;
        std::mutex mInitMutex;
        std::atomic<bool> mAttempted{ false };
        std::atomic<bool> mEnabled{ false };
    };

    inline std::string csvQuote(std::string_view value)
    {
        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');
        for (char c : value)
        {
            if (c == '"')
                result.push_back('"');
            result.push_back(c);
        }
        result.push_back('"');
        return result;
    }

    // Generic opt-in scope timer used by the Optimization Lab. The destination
    // writer decides which environment variable enables the stream. A minimum
    // duration keeps hot-path diagnostic files manageable.
    class ScopedCsvTimer
    {
    public:
        ScopedCsvTimer(CsvWriter& writer, std::string_view phase, std::string_view detail = {}, double minimumMs = 0.0)
            : mWriter(writer)
            , mEnabled(writer.enabled())
            , mMinimumMs(minimumMs)
            , mPhase(mEnabled ? phase : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
            , mStart(mEnabled ? Clock::now() : Clock::time_point{})
        {
        }

        ~ScopedCsvTimer()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << csvQuote(mPhase) << ','
                << csvQuote(mDetail) << ',' << std::fixed << std::setprecision(3) << durationMs;
            mWriter.writeLine(row.str());
        }

        ScopedCsvTimer(const ScopedCsvTimer&) = delete;
        ScopedCsvTimer& operator=(const ScopedCsvTimer&) = delete;

    private:
        CsvWriter& mWriter;
        bool mEnabled;
        double mMinimumMs;
        std::string mPhase;
        std::string mDetail;
        Clock::time_point mStart;
    };

    inline CsvWriter& transitionWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRANSITION_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& pagingWriter()
    {
        static CsvWriter writer("OPENMW_V3_PAGING_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& resourceWriter()
    {
        static CsvWriter writer("OPENMW_V3_RESOURCE_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& navWriter()
    {
        static CsvWriter writer("OPENMW_V3_NAV_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& insertionWriter()
    {
        static CsvWriter writer("OPENMW_V3_INSERT_FILE",
            "frame,epoch_ms,cell,total_refs,rendered_refs,physics_refs,actors,animated,doors,render_ms,mechanics_ms,"
            "particles_ms,physics_ms,lua_added_ms,nav_ms");
        return writer;
    }

    inline CsvWriter& v32RendererInsertionWriter()
    {
        static CsvWriter writer("OPENMW_V32_RENDER_INSERT_FILE",
            "frame,epoch_ms,cell,objects,constructed,restored,paged,static_refs,animated_refs,actors,lights,"
            "renderer_total_ms,mean_object_ms,scene_instance_ms,object_root_exclusive_ms,controller_setup_ms,"
            "transform_attach_ms,misc_ms,max_object_ms,max_ref,max_model");
        return writer;
    }

    inline CsvWriter& workQueueWriter()
    {
        static CsvWriter writer("OPENMW_V3_WORKQUEUE_FILE",
            "frame,epoch_ms,thread,event,item,type,queue_depth,active_threads,duration_ms");
        return writer;
    }

    inline CsvWriter& renderWriter()
    {
        static CsvWriter writer("OPENMW_V3_RENDER_FILE", "frame,epoch_ms,phase,detail,duration_ms");
        return writer;
    }

    inline CsvWriter& compileWriter()
    {
        static CsvWriter writer("OPENMW_P4_COMPILE_FILE",
            "frame,epoch_ms,event,kind,compile_class,queue_depth,oldest_age_frames,budget_ms,credit_ms,"
            "predicted_ms,actual_ms,headroom_ms,last_handoff_ms,objects,detail");
        return writer;
    }

    inline CsvWriter& p6RenderPhaseWriter()
    {
        static CsvWriter writer("OPENMW_P6_RENDER_PHASE_FILE",
            "frame,epoch_ms,thread,phase,duration_ms,detail");
        return writer;
    }

    inline CsvWriter& p6TraversalBreakdownWriter()
    {
        // One compact row per benchmark frame. This is intentionally separate
        // from the event-style render writer so subphase attribution does not
        // require high-volume deep tracing.
        static CsvWriter writer("OPENMW_P6_TRAVERSAL_FILE",
            "frame,epoch_ms,total_ms,context_query_ms,window_status_ms,scene_stats_ms,"
            "pager_begin_ms,scene_bound_ms,camera_query_ms,start_barrier_ms,cull_ms,"
            "context_ops_ms,dispatch_wait_ms,main_swap_ms,pager_end_ms,"
            "dynamic_draw_wait_ms,release_context_ms,other_ms,contexts,cameras,threading_model,"
            "sceneview0_dynamic,sceneview1_dynamic,dynamic_max");
        return writer;
    }

    inline CsvWriter& p8DynamicDrawWriter()
    {
        static CsvWriter writer("OPENMW_P8_DYNAMIC_DRAW_FILE",
            "frame,epoch_ms,thread,kind,class,name,duration_ms,data_variance,vertices,primitive_sets,"
            "buffer_objects,buffer_bytes,dynamic_buffer_bytes,max_modified_count");
        return writer;
    }

    inline CsvWriter& p8DynamicFrameWriter()
    {
        static CsvWriter writer("OPENMW_P8_DYNAMIC_FRAME_FILE",
            "frame,epoch_ms,thread,instrumented_draws,total_draw_ms,max_draw_ms,slow_draws,"
            "buffer_bytes,dynamic_buffer_bytes");
        return writer;
    }

    inline CsvWriter& p8DeformWriter()
    {
        static CsvWriter writer("OPENMW_P8_DEFORM_FILE",
            "frame,epoch_ms,thread,kind,name,duration_ms,vertices,units,buffer_bytes");
        return writer;
    }

    inline CsvWriter& postFxWriter()
    {
        static CsvWriter writer("OPENMW_V3_POSTFX_FILE",
            "frame,epoch_ms,thread,technique,pass,cpu_submit_ms,width,height,render_target,mipmap");
        return writer;
    }

    inline CsvWriter& p9TemporalInputWriter()
    {
        static CsvWriter writer("OPENMW_P9_TEMPORAL_FILE",
            "frame,epoch_ms,context,submitted,history_valid,previous_frame,reset_reasons,"
            "render_w,render_h,output_w,output_h,target_revision,jitter_x,jitter_y,"
            "previous_jitter_x,previous_jitter_y,dense_dynamic_motion,color_ptr,depth_ptr,motion_ptr,"
            "input_mask,dlss_ready,writer_dropped_total,dynamic_surfaces,unsupported_surfaces,"
            "ownership_requested,ownership_active,ownership_fallback_reason");
        return writer;
    }

    inline CsvWriter& p9CompositeWriter()
    {
        static CsvWriter writer("OPENMW_P9_COMPOSITE_FILE",
            "frame,epoch_ms,context,total_ms,available_ms,immediate_start,queued_start,maps,"
            "required_maps,drawables,fbo_ms,state_ms,draw_ms,yields_delta,immediate_end,queued_end,"
            "prepare_scheduled_total,prepare_invalidated_total,prepare_cancelled_total,required_fallbacks_total,"
            "age_progress_total,budget_yields_total,pending_prepare_bytes,shared_charged_ms,oldest_age_frames");
        return writer;
    }

    inline CsvWriter& p9DlssCapabilitiesWriter()
    {
        static CsvWriter writer("OPENMW_P9_DLSS_CAPS_FILE",
            "epoch_ms,context,vendor,renderer,version,gl_version,memory_object,memory_object_win32,"
            "semaphore,semaphore_win32,import_memory_win32_fn,texture_storage_mem_2d_fn,"
            "import_semaphore_win32_fn,wait_semaphore_fn,signal_semaphore_fn,copy_image_fn,"
            "gl_vulkan_bridge_candidate");
        return writer;
    }

    inline CsvWriter& streamingWriter()
    {
        static CsvWriter writer("OPENMW_V3_STREAMING_FILE",
            "frame,epoch_ms,event,category,detail,last_frame_ms,limit,count");
        return writer;
    }

    inline CsvWriter& gpuMemoryWriter()
    {
        static CsvWriter writer("OPENMW_V32_GPU_MEMORY_FILE",
            "frame,epoch_ms,dedicated_usage_mb,dedicated_budget_mb,available_for_reservation_mb,"
            "current_reservation_mb,budget_used_pct,effective_soft_mb,effective_hard_mb,pressure,"
            "nvml_available,adapter_used_mb,adapter_free_mb,adapter_total_mb");
        return writer;
    }

    inline CsvWriter& traceWriter()
    {
        static CsvWriter writer("OPENMW_V3_TRACE_FILE",
            "frame,epoch_ms,thread,id,parent,category,name,detail,duration_ms");
        return writer;
    }

    // Nested, cross-thread trace scope. IDs and parent IDs let an offline tool
    // reconstruct the critical path instead of correlating unrelated CSV rows.
    class TraceScope
    {
    public:
        TraceScope(std::string_view category, std::string_view name, std::string_view detail = {}, double minimumMs = 0.0)
            : mEnabled(traceWriter().enabled())
            , mMinimumMs(minimumMs)
            , mCategory(mEnabled ? category : std::string_view{})
            , mName(mEnabled ? name : std::string_view{})
            , mDetail(mEnabled ? detail : std::string_view{})
        {
            if (!mEnabled)
                return;
            mId = sNextId.fetch_add(1, std::memory_order_relaxed);
            mParent = sCurrentParent;
            sCurrentParent = mId;
            mStart = Clock::now();
        }

        ~TraceScope()
        {
            if (!mEnabled)
                return;
            const double durationMs = elapsedMs(mStart);
            sCurrentParent = mParent;
            if (durationMs < mMinimumMs)
                return;

            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << threadId() << ',' << mId << ','
                << mParent << ',' << csvQuote(mCategory) << ',' << csvQuote(mName) << ',' << csvQuote(mDetail) << ','
                << std::fixed << std::setprecision(3) << durationMs;
            traceWriter().writeLine(row.str());
        }

        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;

    private:
        inline static std::atomic<unsigned long long> sNextId{ 1 };
        inline static thread_local unsigned long long sCurrentParent = 0;

        bool mEnabled = false;
        double mMinimumMs = 0.0;
        std::string mCategory;
        std::string mName;
        std::string mDetail;
        unsigned long long mId = 0;
        unsigned long long mParent = 0;
        Clock::time_point mStart{};
    };

    inline void writeEvent(std::string_view event, std::string_view detail = {})
    {
        static CsvWriter writer("OPENMW_V3_EVENT_FILE", "frame,epoch_ms,event,detail");
        if (!writer.enabled())
            return;

        std::ostringstream row;
        row << V3HitchTelemetry::currentFrame() << ',' << epochMs() << ',' << csvQuote(event) << ',' << csvQuote(detail);
        writer.writeLine(row.str());
    }
}

#endif
