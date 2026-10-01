#include "hitchtrace.hpp"
#include <sstream>
#include <type_traits>
#include <stdexcept>

#ifdef _WIN32
#include <components/misc/windows.hpp>
#else
#include <time.h>
#endif

namespace Debug::HitchTrace
{
    std::int64_t Capture::now() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }

    Capture::Capture() noexcept
    {
        if (!requested()) return;
        try
        {
            Options options;
            options.path = std::getenv("OPENMW_P9_HITCH_SPANS_FILE");
            const char* transport = std::getenv("OPENMW_P9_HITCH_TRANSPORT");
            options.streaming = transport && std::string_view(transport) == "stream";
            options.capacity = options.streaming ? MaximumCapacity : Capacity;
            if (const char* capacity = std::getenv("OPENMW_P9_HITCH_CAPACITY"); capacity && *capacity)
            {
                std::size_t parsed = 0;
                const auto last = capacity + std::strlen(capacity);
                const auto result = std::from_chars(capacity, last, parsed);
                if (result.ec == std::errc{} && result.ptr == last
                    && (parsed == 131072 || parsed == Capacity || parsed == MaximumCapacity))
                    options.capacity = parsed;
            }
            initialize(std::move(options));
        }
        catch (...) { mArmed = true; mAllocationFailed = mInitializationFailed = true; }
    }

    Capture::Capture(Options options) noexcept { initialize(std::move(options)); }

    void Capture::initialize(Options options) noexcept
    {
        mPath = std::move(options.path);
        mArmed = !mPath.empty() && mPath != "0";
        if (!mArmed) return;
        mStreaming = options.streaming;
        mCapacity = options.capacity;
        mBeforeConsumerRead = options.beforeConsumerRead;
        mConsumerContext = options.consumerContext;
        try
        {
            if (mCapacity < 2 || mCapacity > MaximumCapacity || (mCapacity & (mCapacity - 1)))
                throw std::invalid_argument("Hitch storage capacity must be a bounded power of two");
            writeClockMap(mPath, false);
            if (mStreaming)
            {
                mSlots = std::make_unique<Slot[]>(mCapacity);
                // At less than one page per slot these stores pre-touch every
                // storage page before the consumer or any producer starts.
                for (std::size_t i = 0; i < mCapacity; ++i)
                    mSlots[i].sequence.store(i, std::memory_order_relaxed);
                mWriter = std::thread([this] { writerEntry(); });
            }
            else
            {
                mRows = std::make_unique<Row[]>(mCapacity);
                volatile unsigned char* pages = reinterpret_cast<unsigned char*>(mRows.get());
                for (std::size_t i = 0; i < mCapacity * sizeof(Row); i += 4096) pages[i] = 0;
            }
        }
        catch (...) { mAllocationFailed = mInitializationFailed = true; }
    }

    Capture::~Capture() { finish(); }

    Capture::PushResult Capture::push(const Row& row) noexcept
    {
        static_assert(std::is_nothrow_copy_assignable_v<Row>);
        auto position = mProducer.load(std::memory_order_relaxed);
        for (unsigned attempt = 0; attempt < 128; ++attempt)
        {
            auto& slot = mSlots[position & (mCapacity - 1)];
            const auto sequence = slot.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::intptr_t>(sequence - position);
            if (difference == 0)
            {
                if (mProducer.compare_exchange_weak(position, position + 1, std::memory_order_relaxed))
                {
                    // Acceptance and pending counts precede release publication:
                    // the consumer can never complete an uncounted accepted row.
                    mAccepted.fetch_add(1, std::memory_order_relaxed);
                    mPending.fetch_add(1, std::memory_order_relaxed);
                    const auto queued = mQueued.fetch_add(1, std::memory_order_relaxed) + 1;
                    auto high = mHighWater.load(std::memory_order_relaxed);
                    for (unsigned retry = 0; retry < 8 && high < queued; ++retry)
                        if (mHighWater.compare_exchange_weak(high, queued, std::memory_order_relaxed)) break;
                    slot.row = row;
                    slot.sequence.store(position + 1, std::memory_order_release);
                    if (queued == 1 || (position & 127) == 0) mWake.notify_one();
                    return PushResult::Published;
                }
            }
            else if (difference < 0) return PushResult::Full;
            else position = mProducer.load(std::memory_order_relaxed);
        }
        return PushResult::Contended;
    }

    bool Capture::pop(Row& row) noexcept
    {
        const auto position = mConsumer.load(std::memory_order_relaxed);
        auto& slot = mSlots[position & (mCapacity - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != position + 1) return false;
        row = slot.row;
        // Remove occupancy before publishing the reusable slot, so a new
        // producer cannot temporarily count this slot twice.
        mQueued.fetch_sub(1, std::memory_order_relaxed);
        slot.sequence.store(position + mCapacity, std::memory_order_release);
        mConsumer.store(position + 1, std::memory_order_release);
        return true;
    }

    void Capture::append(const Row& row) noexcept
    {
        if (!mArmed) return;
        const auto observer = DiagnosticObserver::begin();
        mAttempted.fetch_add(1, std::memory_order_relaxed);
        mAppenders.fetch_add(1, std::memory_order_acq_rel);
        if (mStopped.load(std::memory_order_acquire))
            mStoppedDropped.fetch_add(1, std::memory_order_relaxed);
        else if (mStreaming)
        {
            if (!mSlots) mAllocationDropped.fetch_add(1, std::memory_order_relaxed);
            else
            {
                switch (push(row))
                {
                    case PushResult::Published: break;
                    case PushResult::Full: mCapacityDropped.fetch_add(1, std::memory_order_relaxed); break;
                    case PushResult::Contended: mContentionDropped.fetch_add(1, std::memory_order_relaxed); break;
                }
            }
        }
        else if (!mRows) mAllocationDropped.fetch_add(1, std::memory_order_relaxed);
        else
        {
            // This control is intentionally drained only after producer joins.
            const auto index = mProducer.fetch_add(1, std::memory_order_relaxed);
            if (index < mCapacity)
            {
                mRows[index] = row;
                mAccepted.fetch_add(1, std::memory_order_relaxed);
                mPending.fetch_add(1, std::memory_order_relaxed);
            }
            else mCapacityDropped.fetch_add(1, std::memory_order_relaxed);
        }
        mAppenders.fetch_sub(1, std::memory_order_release);
        DiagnosticObserver::end(DiagnosticObserver::Category::TraceAppend, observer);
    }

    void Capture::quote(std::ostream& out, std::string_view text)
    {
        out << '"';
        for (char c : text) { if (c == '"') out << '"'; out << c; }
        out << '"';
    }
    void Capture::writeHeader(std::ostream& out)
    {
        out << "span,parent,frame,thread,start_ns,end_ns,epoch_ms,phase,identity,asset,wall_ms,children_ms,unattributed_ms,thread_cpu_ms,identity_truncated,link";
        for (unsigned i = 1; i <= 4; ++i) out << ",metric" << i << "_name,metric" << i << "_value";
        out << '\n';
    }
    void Capture::writeRow(std::ostream& out, const Row& row)
    {
        const auto duration = row.end - row.start;
        out << row.id << ',' << row.parent << ',' << row.frame << ',' << row.thread << ',' << row.start
            << ',' << row.end << ',' << row.epoch << ',';
        quote(out, row.phase.data()); out << ','; quote(out, row.identity.data()); out << ',';
        quote(out, row.asset.data());
        out << std::fixed << std::setprecision(6) << ',' << duration / 1e6 << ',' << row.children / 1e6
            << ',' << (std::max)(std::int64_t(0), duration - row.children) / 1e6 << ','
            << (row.cpu < 0 ? -1.0 : row.cpu / 1e6) << ',' << row.truncated << ',' << row.link;
        for (const auto& metric : row.metrics)
        { out << ','; quote(out, metric.name.data()); out << ',' << metric.value; }
        out << '\n';
    }
    void Capture::nameWriter() noexcept
    {
#ifdef _WIN32
        using SetDescription = HRESULT(WINAPI*)(HANDLE, PCWSTR);
        const auto function = reinterpret_cast<SetDescription>(
            GetProcAddress(GetModuleHandleW(L"Kernel32.dll"), "SetThreadDescription"));
        if (function) function(GetCurrentThread(), L"OptimizedMW Hitch CSV Writer");
#endif
    }

    void Capture::writerEntry() noexcept
    {
        nameWriter();
        mWriterThreadId = threadId();
        mWriterStarted = now();
        const auto cpuStart = threadCpuNs();
        std::ofstream out;
        std::size_t inFlight = 0;
        try
        {
            out.open(mPath, std::ios::out | std::ios::trunc | std::ios::binary);
            writeHeader(out);
            if (!out) mInitializationFailed = mWriterFailed = true;
            if (mBeforeConsumerRead) mBeforeConsumerRead(mConsumerContext);
            std::array<Row, 256> batch;
            std::ostringstream encoded;
            for (;;)
            {
                std::size_t count = 0;
                while (count < batch.size() && pop(batch[count])) ++count;
                inFlight = count;
                if (count)
                {
                    if (!mWriterFailed)
                    {
                        const auto formatStart = now();
                        encoded.str({}); encoded.clear();
                        for (std::size_t i = 0; i < count; ++i) writeRow(encoded, batch[i]);
                        const auto text = encoded.str();
                        mFormatNs += static_cast<std::uint64_t>(now() - formatStart);
                        const auto writeStart = now();
                        out.write(text.data(), static_cast<std::streamsize>(text.size()));
                        mWriteNs += static_cast<std::uint64_t>(now() - writeStart);
                        ++mBatches;
                        if (out) { mWritten.fetch_add(count); mBytes += text.size(); }
                        else { mWriterFailed = true; mIoDropped.fetch_add(count); }
                    }
                    else mIoDropped.fetch_add(count);
                    mPending.fetch_sub(count);
                    inFlight = 0;
                    continue;
                }
                if (mStopped.load(std::memory_order_acquire) && !mAppenders.load(std::memory_order_acquire)
                    && mProducer.load(std::memory_order_acquire) == mConsumer.load(std::memory_order_acquire))
                    break;
                std::unique_lock lock(mWakeMutex);
                mWake.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
        catch (...)
        {
            mWriterFailed = true;
            if (inFlight) { mIoDropped.fetch_add(inFlight); mPending.fetch_sub(inFlight); }
            // Drain accepted records without substituting producer-thread I/O.
            // The reserved row copy/publish operation is noexcept.
            for (;;)
            {
                Row row;
                if (pop(row)) { mIoDropped.fetch_add(1); mPending.fetch_sub(1); continue; }
                if (mStopped.load(std::memory_order_acquire) && !mAppenders.load(std::memory_order_acquire)
                    && mProducer.load(std::memory_order_acquire) == mConsumer.load(std::memory_order_acquire)) break;
                std::unique_lock lock(mWakeMutex);
                mWake.wait_for(lock, std::chrono::milliseconds(10));
            }
        }
        mWriterFinished = now();
        const auto cpuEnd = threadCpuNs();
        if (cpuStart >= 0 && cpuEnd >= cpuStart) mWriterCpu = cpuEnd - cpuStart;
        try
        {
            // Successful queued batch writes are not proof of final flush.
            out.flush();
            if (!out) mWriterFailed = true;
            mDrainComplete = !mPending.load() && !mQueued.load() && !mAppenders.load() && !mWriterFailed;
            mOutputOk = mDrainComplete && !mActiveAtFinish && !mAppendersAtFinish && !mInitializationFailed
                && !mAllocationFailed;
            writeStatus(out, "# ", mOutputOk);
            out.flush();
            if (!out) { mWriterFailed = true; mOutputOk = mDrainComplete = false; }
            out.close();
            if (out.fail()) { mWriterFailed = true; mOutputOk = mDrainComplete = false; }
        }
        catch (...) { mWriterFailed = true; mOutputOk = mDrainComplete = false; }
    }

    void Capture::writeStatus(std::ostream& out, std::string_view p, bool outputOk)
    {
        const auto dropped = mCapacityDropped.load() + mContentionDropped.load() + mAllocationDropped.load()
            + mIoDropped.load() + mStoppedDropped.load();
        out << p << "rows=" << mWritten.load() << '\n' << p << "capacity=" << mCapacity << '\n'
            << p << "dropped=" << dropped << '\n' << p << "allocation_failed=" << mAllocationFailed << '\n'
            << p << "active_at_finish=" << mActiveAtFinish << '\n' << p << "schema=2\n"
            << p << "normal_finish=1\n" << p << "output_ok=" << outputOk << '\n'
            << p << "minimum_detail_ms=0.25\n" << p << "forced_dependency_events=retained\n"
            << p << "transport_schema=1\n" << p << "transport="
            << (mStreaming ? "stream_mpsc_v1" : "deferred_arena_v1") << '\n'
            << p << "attempted=" << mAttempted.load() << '\n' << p << "accepted=" << mAccepted.load() << '\n'
            << p << "written=" << mWritten.load() << '\n' << p << "pending=" << mPending.load() << '\n'
            << p << "capacity_dropped=" << mCapacityDropped.load() << '\n'
            << p << "contention_dropped=" << mContentionDropped.load() << '\n'
            << p << "allocation_dropped=" << mAllocationDropped.load() << '\n'
            << p << "io_dropped=" << mIoDropped.load() << '\n'
            << p << "stopped_dropped=" << mStoppedDropped.load() << '\n'
            << p << "initialization_failed=" << mInitializationFailed << '\n'
            << p << "writer_failed=" << mWriterFailed << '\n'
            << p << "drain_complete=" << mDrainComplete << '\n'
            << p << "active_producers_at_finish=" << mAppendersAtFinish << '\n'
            << p << "queue_capacity=" << (mStreaming ? mCapacity : 0) << '\n'
            << p << "queue_high_water=" << mHighWater.load() << '\n'
            << p << "row_size_bytes=" << sizeof(Row) << '\n'
            << p << "storage_bytes=" << mCapacity * (mStreaming ? sizeof(Slot) : sizeof(Row)) << '\n'
            << p << "writer_thread_id=" << mWriterThreadId << '\n'
            << p << "writer_started_ns=" << mWriterStarted << '\n'
            << p << "writer_finished_ns=" << mWriterFinished << '\n'
            << p << "writer_cpu_available=" << (mWriterCpu >= 0) << '\n'
            << p << "writer_cpu_ns=" << (mWriterCpu >= 0 ? mWriterCpu : 0) << '\n'
            << p << "writer_format_ns=" << mFormatNs << '\n' << p << "writer_write_ns=" << mWriteNs << '\n'
            << p << "writer_batches=" << mBatches << '\n' << p << "writer_serialized_bytes=" << mBytes << '\n'
            << p << "writer_timing_scope=row_format_and_batch_write_excludes_final_footer_close_and_wait\n"
            << p << "queue_high_water_scope=observed_claimed_slots_bounded_CAS_not_continuous_exact_peak\n"
            << p << "flush_failure_scope=written_counts_successful_batch_calls_not_durable_media_on_final_flush_failure\n";
    }

    void Capture::finish() noexcept
    {
        if (!mArmed || mFinishing.exchange(true)) return;
        mActiveAtFinish = mActive.load(std::memory_order_acquire);
        mAppendersAtFinish = mAppenders.load(std::memory_order_acquire);
        mStopped.store(true, std::memory_order_release);
        mWake.notify_one();
        if (mStreaming)
        {
            if (mWriter.joinable()) mWriter.join();
            else
            {
                mWriterFailed = true;
                Row row;
                if (mSlots) while (pop(row)) { mIoDropped.fetch_add(1); mPending.fetch_sub(1); }
            }
        }
        else
        {
            try
            {
                std::ofstream out(mPath, std::ios::out | std::ios::trunc | std::ios::binary);
                writeHeader(out);
                const auto count = mAccepted.load();
                if (!mActiveAtFinish && !mAppendersAtFinish && mRows)
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        writeRow(out, mRows[i]);
                        if (out) mWritten.fetch_add(1);
                        else mIoDropped.fetch_add(1);
                        mPending.fetch_sub(1);
                    }
                out.flush();
                mWriterFailed = !out;
                mDrainComplete = !mPending.load() && !mWriterFailed;
                mOutputOk = mDrainComplete && !mActiveAtFinish && !mAppendersAtFinish && !mAllocationFailed;
                writeStatus(out, "# ", mOutputOk); out.flush(); out.close();
                if (out.fail()) { mWriterFailed = true; mOutputOk = mDrainComplete = false; }
            }
            catch (...) { mWriterFailed = true; mOutputOk = mDrainComplete = false; }
        }
        // These files do not consume queue slots and survive data saturation.
        writeClockMap(mPath, true);
        try
        {
            std::ofstream status(mPath + ".capture-status.txt");
            writeStatus(status, "", mOutputOk); status.flush();
        }
        catch (...) { /* Missing status fails qualification. */ }
        if (!mActiveAtFinish && !mAppendersAtFinish) DiagnosticObserver::finish(mPath);
    }
}

bool Debug::HitchTrace::frameIntervalsRequested() noexcept
{
    return requested();
}

void Debug::HitchTrace::recordFrameInterval(
    unsigned frame, std::int64_t start, std::int64_t end, std::int64_t epoch) noexcept
{
    if (!requested() || !Capture::instance().enabled()) return;
    Row row{};
    row.id = Capture::instance().nextId();
    row.frame = frame;
    row.thread = threadId();
    row.start = start;
    row.end = end;
    row.epoch = epoch;
    row.cpu = -1;
    std::memcpy(row.phase.data(), "frame.wall_interval", sizeof("frame.wall_interval"));
    Capture::instance().append(row);
}

std::uint64_t Debug::HitchTrace::threadId() noexcept
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
}

void Debug::HitchTrace::writeClockMap(const std::string& path, bool final) noexcept
{
    try
    {
        std::ofstream out(path + ".clock-map.txt", final ? std::ios::app : std::ios::trunc);
#ifdef _WIN32
        LARGE_INTEGER frequency{}, before{}, after{};
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&before);
        const auto steady = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        const auto utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        QueryPerformanceCounter(&after);
        const char* prefix = final ? "end_" : "start_";
        if (!final)
            out << "schema=1\nprocess_id=" << GetCurrentProcessId()
                << "\nthread_id_kind=windows_native_tid\nqpc_frequency=" << frequency.QuadPart
                << "\nthread_cpu_resolution=coarse_GetThreadTimes_not_submillisecond\n";
        out << prefix << "qpc_before=" << before.QuadPart << '\n'
            << prefix << "steady_ns=" << steady << '\n'
            << prefix << "utc_ns=" << utc << '\n'
            << prefix << "qpc_after=" << after.QuadPart << '\n';
#else
        if (!final) out << "schema=1\nthread_id_kind=std_thread_hash_no_ETW_mapping\n";
#endif
        out << (final ? "normal_finish=1\n" : "started=1\n");
    }
    catch (...) { /* Missing clock map explicitly prevents ETW alignment. */ }
}

std::int64_t Debug::HitchTrace::threadCpuNs() noexcept
{
#ifdef _WIN32
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) return -1;
    const auto ticks = [](FILETIME t) { return (std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return static_cast<std::int64_t>((ticks(kernel) + ticks(user)) * 100);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    timespec t{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) != 0) return -1;
    return t.tv_sec * 1000000000LL + t.tv_nsec;
#else
    return -1;
#endif
}
