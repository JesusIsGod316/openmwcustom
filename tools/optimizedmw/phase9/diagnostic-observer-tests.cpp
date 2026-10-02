#include <components/debug/diagnosticobserver.hpp>

#include <cstdlib>
#include <atomic>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace Observer = Debug::DiagnosticObserver;

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    std::uint64_t calls(Observer::Category category)
    {
        std::uint64_t result = 0;
        for (const auto& slot : Observer::slots)
            result += slot.counters[static_cast<unsigned>(category)].calls.load();
        if (Observer::localAggregationRequested() && !Observer::localData.closed)
            result += Observer::localData.counters[static_cast<unsigned>(category)].calls;
        return result;
    }

    std::string read(const std::string& path)
    {
        std::ifstream in(path);
        require(bool(in), "Observer output missing");
        return { std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
    }
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 3, "Usage: diagnostic-observer-tests mode output-prefix");
        const std::string mode = argv[1], path = argv[2];
        const bool local = mode.ends_with("-local");
        const bool normal = mode == "normal" || mode == "normal-local";
        require(normal || mode == "deep" || mode == "overflow" || mode == "deep-local"
            || mode == "overflow-local" || mode == "active-local" || mode == "independent-local"
            || mode == "late-tls-local" || mode == "undeclared-local", "Invalid test mode");
#ifdef _WIN32
        require(_putenv_s("OPENMW_P9_OBSERVER_LOCAL", local ? "1" : "0") == 0, "Unable to set local mode");
        require(_putenv_s("OPENMW_P9_HITCH_SPANS_FILE", normal ? "" : path.c_str()) == 0,
            "Unable to set test environment");
#else
        require(setenv("OPENMW_P9_OBSERVER_LOCAL", local ? "1" : "0", 1) == 0, "Unable to set local mode");
        require(setenv("OPENMW_P9_HITCH_SPANS_FILE", normal ? "" : path.c_str(), 1) == 0,
            "Unable to set test environment");
#endif
        if (normal)
        {
            require(!Observer::enabled() && Observer::begin() == 0, "Normal mode started a timer");
            for (unsigned i = 0; i < 1000; ++i)
            {
                Observer::Scope scope(Observer::Category::CatalogPublication);
                Observer::count(Observer::Category::TraceFiltered);
                Observer::record(Observer::Category::TraceAppend, 100, 200);
            }
            Observer::finish(path);
            require(Observer::nextSlot.load() == 0, "Normal mode registered a producer");
            require(calls(Observer::Category::LedgerUpdate) == 0, "Normal mode updated the ledger");
            require(!Observer::stopped.load(), "Normal mode changed capture state");
            require(!std::ifstream(path + ".observer.csv"), "Normal mode wrote an observer file");
        }
        else
        {
            require(Observer::enabled() && Observer::begin() != 0, "Deep mode did not arm");
            Observer::record(Observer::Category::CatalogWork, 100, 1300);
            Observer::record(Observer::Category::CatalogWork, 100, 90);
            require(calls(Observer::Category::CatalogWork) == 2, "Known catalog calls lost");
            const auto knownWall = local
                ? Observer::localData.counters[static_cast<unsigned>(Observer::Category::CatalogWork)].wallNs
                : Observer::threadSlot()->counters[static_cast<unsigned>(Observer::Category::CatalogWork)].wallNs.load();
            require(knownWall == 1200, "Invalid durations or negative-clamp behavior");
            if (mode == "active-local")
            {
                Observer::Scope live(Observer::Category::CatalogPublication);
                Observer::finish(path);
                const auto status = read(path + ".observer-status.txt");
                require(status.find("coverage_complete=0") != std::string::npos, "Live own observer qualified complete");
                require(status.find("active_observer_scopes=1") != std::string::npos, "Live own scope not preserved");
                return 0;
            }
            if (mode == "independent-local")
            {
                std::atomic<bool> entered{false}, release{false};
                std::thread producer([&] {
                    Observer::Scope live(Observer::Category::CatalogPublication);
                    entered.store(true, std::memory_order_release);
                    while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
                });
                while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
                Observer::finish(path);
                release.store(true, std::memory_order_release);
                producer.join();
                const auto status = read(path + ".observer-status.txt");
                require(status.find("coverage_complete=0") != std::string::npos, "Independent live producer qualified");
                require(status.find("unretired_producers=1") != std::string::npos, "Independent live producer not counted");
                return 0;
            }
            if (mode == "late-tls-local")
            {
                std::thread producer([] {
                    struct LateObserver
                    {
                        ~LateObserver() { Observer::count(Observer::Category::TraceFiltered); }
                    };
                    // Constructed first, destroyed after the ledger publisher.
                    static thread_local LateObserver late;
                    (void)late;
                    Observer::count(Observer::Category::TraceFiltered);
                });
                producer.join();
                require(Observer::retiredUpdatesRejected.load() == 1, "Late TLS update silently accepted");
                Observer::declareProducerThreadsJoined();
                Observer::finish(path);
                const auto status = read(path + ".observer-status.txt");
                require(status.find("coverage_complete=0") != std::string::npos, "Late TLS loss qualified complete");
                require(status.find("retired_updates_rejected=1") != std::string::npos, "Late TLS loss missing");
                return 0;
            }
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < 8; ++i)
                workers.emplace_back([] {
                    for (unsigned n = 0; n < 1000; ++n) Observer::count(Observer::Category::TraceFiltered);
                    Observer::Scope scope(Observer::Category::CatalogPublication);
                });
            for (auto& worker : workers) worker.join();
            require(calls(Observer::Category::TraceFiltered) == 8000, "Concurrent count loss");
            require(calls(Observer::Category::CatalogPublication) == 8, "Concurrent scope loss");
            require(Observer::nextSlot.load() == 9, "Thread lifetime slots merged");
            require(calls(Observer::Category::LedgerUpdate) == 8010, "Recursive/missing ledger bookkeeping");
            if (mode == "overflow" || mode == "overflow-local")
            {
                for (unsigned i = 0; i < Observer::ThreadCapacity; ++i)
                {
                    std::thread worker([] { Observer::count(Observer::Category::TraceRetained); });
                    worker.join();
                }
                require(Observer::nextSlot.load() == Observer::ThreadCapacity + 9, "Recycled OS thread reused slot");
                require(Observer::ledgerOverflow.load() == 9, "Overflow did not count every lost observation");
            }
            if (mode != "undeclared-local") Observer::declareProducerThreadsJoined();
            Observer::finish(path);
            require(Observer::stopped.load() && Observer::begin() == 0, "Finished capture still started timers");
            const auto before = calls(Observer::Category::TraceFiltered);
            Observer::count(Observer::Category::TraceFiltered);
            require(calls(Observer::Category::TraceFiltered) == before, "Post-finish observation accepted");
            const auto status = read(path + ".observer-status.txt");
            require(status.find("output_ok=1") != std::string::npos, "Observer output failed");
            require(status.find(mode == "overflow" || mode == "overflow-local" || mode == "undeclared-local"
                    ? "coverage_complete=0" : "coverage_complete=1")
                != std::string::npos, "Overflow qualification was not explicit");
            if (mode == "undeclared-local")
            {
                require(status.find("producer_threads_joined=0") != std::string::npos,
                    "Missing producer-join declaration hidden");
                require(status.find("unretired_producers=0") != std::string::npos,
                    "Test did not retire every producer");
            }
            require(status.find("ledger_update_timing=UNMEASURED") != std::string::npos,
                "Unmeasured ledger cost was presented as timed zero");
            require(read(path + ".observer.csv").find("trace_filtered,1000,0") != std::string::npos,
                "All-call count rows missing");
            const auto csv = read(path + ".observer.csv");
            Observer::finish(path);
            require(csv == read(path + ".observer.csv"), "Second finish overwrote output");
        }
        std::cout << "Diagnostic observer " << mode << " passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
