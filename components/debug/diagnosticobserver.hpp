#ifndef OPENMW_COMPONENTS_DEBUG_DIAGNOSTICOBSERVER_H
#define OPENMW_COMPONENTS_DEBUG_DIAGNOSTICOBSERVER_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace Debug::DiagnosticObserver
{
    std::uint64_t currentNativeThreadId() noexcept;
    enum class Category : unsigned
    {
        TraceScopeEntry, TraceScopeExit, TraceAppend, TraceRetained, TraceFiltered,
        CatalogWork, CatalogPathCollection, CatalogPublication, CatalogCounterUpdate,
        LuaAllocatorBookkeeping, LuaAllocatorTimerBoundary, LuaDecodeTimerBoundary, LedgerUpdate, Count
    };
    inline constexpr std::array<std::string_view, static_cast<unsigned>(Category::Count)> Names{
        "trace_scope_entry", "trace_scope_exit", "trace_append", "trace_retained", "trace_filtered",
        "catalog_work", "catalog_path_collection", "catalog_publication", "catalog_counter_update",
        "lua_allocator_bookkeeping", "lua_allocator_timer_boundary", "lua_decode_timer_boundary", "ledger_update" };
    inline bool enabled() noexcept
    {
        static const bool value = [] {
            const char* path = std::getenv("OPENMW_P9_HITCH_SPANS_FILE");
            return path && *path && std::string_view(path) != "0";
        }();
        return value;
    }
    inline bool localAggregationRequested() noexcept
    {
        static const bool value = [] {
            const char* mode = std::getenv("OPENMW_P9_OBSERVER_LOCAL");
            return mode && std::string_view(mode) == "1";
        }();
        return value;
    }
    inline std::int64_t now() noexcept
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    struct Counter
    {
        std::atomic<std::uint64_t> calls{0}, wallNs{0};
    };
    struct Slot
    {
        std::atomic<std::uint64_t> thread{0};
        std::array<Counter, static_cast<unsigned>(Category::Count)> counters{};
        // Local producers publish the atomic snapshot once at retirement. A
        // collector never reads another thread's live plain counters.
        std::atomic<bool> retired{false};
        std::atomic<unsigned> activeScopes{0};
    };
    inline constexpr unsigned ThreadCapacity = 256;
    inline std::array<Slot, ThreadCapacity> slots{};
    inline std::atomic<unsigned> nextSlot{0};
    inline std::atomic<std::uint64_t> ledgerOverflow{0};
    inline std::atomic<std::uint64_t> retiredUpdatesRejected{0};
    inline std::atomic<bool> stopped{false};
    inline std::atomic<bool> producerThreadsJoined{false};
    inline void declareProducerThreadsJoined() noexcept
    {
        // The engine calls only after resource, viewer and Lua owner joins.
        // Snapshot publication alone is not proof that all TLS destructors ran.
        if (enabled() && localAggregationRequested())
            producerThreadsJoined.store(true, std::memory_order_release);
    }
    struct LocalCounter { std::uint64_t calls = 0, wallNs = 0; };
    struct LocalData
    {
        Slot* slot = nullptr;
        std::array<LocalCounter, static_cast<unsigned>(Category::Count)> counters{};
        unsigned activeScopes = 0;
        bool closed = false;
    };
    inline thread_local LocalData localData{};
    inline void publishCurrentLocal() noexcept
    {
        if (!localData.slot || localData.closed) return;
        localData.closed = true;
        auto& slot = *localData.slot;
        for (unsigned i = 0; i < Names.size(); ++i)
        {
            slot.counters[i].calls.store(localData.counters[i].calls, std::memory_order_relaxed);
            slot.counters[i].wallNs.store(localData.counters[i].wallNs, std::memory_order_relaxed);
        }
        slot.activeScopes.store(localData.activeScopes, std::memory_order_relaxed);
        slot.retired.store(true, std::memory_order_release);
    }
    struct LocalPublisher
    {
        ~LocalPublisher() { publishCurrentLocal(); }
    };
    inline void registerLocalPublisher() noexcept
    {
        static thread_local LocalPublisher publisher;
        (void)publisher;
    }
    inline Slot* threadSlot() noexcept
    {
        // Each native thread lifetime claims one immutable slot. Recycled OS
        // thread IDs do not reuse a slot. Overflow is explicit, never evicted.
        static thread_local Slot* value = []() -> Slot* {
            const auto index = nextSlot.fetch_add(1, std::memory_order_relaxed);
            if (index >= ThreadCapacity) return nullptr;
            const auto id = currentNativeThreadId();
            slots[index].thread.store(id, std::memory_order_release);
            return &slots[index];
        }();
        if (localAggregationRequested() && value)
        {
            if (localData.closed)
            {
                // A later TLS destructor may try to observe after publication.
                // Never mutate the immutable snapshot or silently qualify it.
                retiredUpdatesRejected.fetch_add(1, std::memory_order_relaxed);
                return nullptr;
            }
            if (!localData.slot)
            {
                localData.slot = value;
                registerLocalPublisher();
            }
        }
        return value;
    }
    inline std::int64_t begin() noexcept
    {
        return enabled() && !stopped.load(std::memory_order_relaxed) ? now() : 0;
    }
    inline void record(Category category, std::int64_t start, std::int64_t end) noexcept
    {
        if (!enabled() || !start || stopped.load(std::memory_order_relaxed)) return;
        auto* slot = threadSlot();
        if (!slot) { ledgerOverflow.fetch_add(1, std::memory_order_relaxed); return; }
        const auto elapsed = static_cast<std::uint64_t>((std::max)(std::int64_t{0}, end-start));
        if (localAggregationRequested())
        {
            auto& value = localData.counters[static_cast<unsigned>(category)];
            ++value.calls;
            value.wallNs += elapsed;
            ++localData.counters[static_cast<unsigned>(Category::LedgerUpdate)].calls;
            return;
        }
        auto& value = slot->counters[static_cast<unsigned>(category)];
        value.calls.fetch_add(1, std::memory_order_relaxed);
        value.wallNs.fetch_add(elapsed, std::memory_order_relaxed);
        // No recursive clocks/scopes to measure these fixed ledger updates.
        slot->counters[static_cast<unsigned>(Category::LedgerUpdate)].calls.fetch_add(1, std::memory_order_relaxed);
    }
    inline void end(Category category, std::int64_t start) noexcept
    {
        if (start) record(category, start, now());
    }
    inline void count(Category category) noexcept
    {
        if (!enabled() || stopped.load(std::memory_order_relaxed)) return;
        auto* slot = threadSlot();
        if (!slot) { ledgerOverflow.fetch_add(1, std::memory_order_relaxed); return; }
        if (localAggregationRequested())
        {
            ++localData.counters[static_cast<unsigned>(category)].calls;
            ++localData.counters[static_cast<unsigned>(Category::LedgerUpdate)].calls;
            return;
        }
        slot->counters[static_cast<unsigned>(category)].calls.fetch_add(1, std::memory_order_relaxed);
        slot->counters[static_cast<unsigned>(Category::LedgerUpdate)].calls.fetch_add(1, std::memory_order_relaxed);
    }
    class Scope
    {
    public:
        explicit Scope(Category category) noexcept : mCategory(category)
        {
            if (enabled() && !stopped.load(std::memory_order_relaxed) && localAggregationRequested())
            {
                // Register independent catalog/allocator observers before the
                // timed body, even when there is no active HitchTrace scope.
                if (threadSlot()) { ++localData.activeScopes; mLocal = true; }
            }
            mStart = begin();
        }
        ~Scope()
        {
            end(mCategory, mStart);
            if (mLocal) --localData.activeScopes;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    private:
        Category mCategory;
        std::int64_t mStart = 0;
        bool mLocal = false;
    };
    inline void finish(const std::string& spanFile) noexcept
    {
        if (!enabled() || stopped.exchange(true)) return;
        const bool local = localAggregationRequested();
        if (local) publishCurrentLocal();
        try
        {
            std::ofstream out(spanFile + ".observer.csv");
            out << "schema,thread_slot,thread,category,calls,wall_ns\n";
            const auto count = (std::min)(nextSlot.load(), ThreadCapacity);
            std::uint64_t rows = 0;
            unsigned unretired = 0, active = 0;
            for (unsigned i=0; i<count; ++i)
            {
                if (local)
                {
                    if (!slots[i].retired.load(std::memory_order_acquire))
                    { ++unretired; continue; }
                    active += slots[i].activeScopes.load(std::memory_order_relaxed);
                }
                for (unsigned c=0; c<Names.size(); ++c)
                {
                    const auto calls = slots[i].counters[c].calls.load();
                    if (!calls) continue;
                    out << "1," << i << ',' << slots[i].thread.load() << ',' << Names[c] << ',' << calls
                        << ',' << slots[i].counters[c].wallNs.load() << '\n';
                    ++rows;
                }
            }
            out.flush(); const bool ok = bool(out); out.close();
            std::ofstream status(spanFile + ".observer-status.txt");
            status << "schema=1\nthread_capacity=" << ThreadCapacity << "\nthread_slots=" << count
                << "\nrows=" << rows << "\nledger_overflow=" << ledgerOverflow.load()
                << "\nnormal_finish=1\noutput_ok=" << (ok && !out.fail())
                << "\ncoverage_complete=" << (ok && !out.fail() && !ledgerOverflow.load()
                    && !unretired && !active && !retiredUpdatesRejected.load()
                    && (!local || producerThreadsJoined.load(std::memory_order_acquire)))
                << "\naggregation=" << (local ? "owner_local_retired_snapshot" : "legacy_atomic")
                << "\nproducer_threads_joined=" << (local && producerThreadsJoined.load(std::memory_order_acquire))
                << "\nunretired_producers=" << unretired << "\nactive_observer_scopes=" << active
                << "\nretired_updates_rejected=" << retiredUpdatesRejected.load()
                << "\ntiming_note=All calls include threshold-filtered trace scopes. Inclusive categories overlap; do not sum catalog_work with its components or trace_scope_exit with append. Clock-call and final fixed ledger-update costs are not fully timed.\n"
                << "ledger_update_timing=UNMEASURED_count_only_no_recursive_timers\n"
                << "lua_allocator_timer_boundary_timing=UNMEASURED_count_only_existing_clock_calls_excludes_allocator_service\n"
                << "lua_decode_timer_boundary_timing=UNMEASURED_count_only_extra_end_clock_reuses_scope_start\n"
                << "count_only_categories=trace_retained,trace_filtered,ledger_update,lua_allocator_timer_boundary,lua_decode_timer_boundary\n"
                << "scope=registered_producer_thread_lifetimes_after_join_no_gameplay_cost_subtraction\n";
        }
        catch (...) { /* Missing status fails qualification; original timeline survives. */ }
    }
}
#endif
