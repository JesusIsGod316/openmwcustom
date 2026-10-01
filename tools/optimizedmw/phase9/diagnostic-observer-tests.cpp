#include <components/debug/diagnosticobserver.hpp>

#include <cstdlib>
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
        require(argc == 3, "Usage: diagnostic-observer-tests normal|deep|overflow output-prefix");
        const std::string mode = argv[1], path = argv[2];
        require(mode == "normal" || mode == "deep" || mode == "overflow", "Invalid test mode");
#ifdef _WIN32
        require(_putenv_s("OPENMW_P9_HITCH_SPANS_FILE", mode == "normal" ? "" : path.c_str()) == 0,
            "Unable to set test environment");
#else
        require(setenv("OPENMW_P9_HITCH_SPANS_FILE", mode == "normal" ? "" : path.c_str(), 1) == 0,
            "Unable to set test environment");
#endif
        if (mode == "normal")
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
            require(Observer::threadSlot()->counters[static_cast<unsigned>(Observer::Category::CatalogWork)]
                .wallNs.load() == 1200, "Invalid durations or negative-clamp behavior");
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
            if (mode == "overflow")
            {
                for (unsigned i = 0; i < Observer::ThreadCapacity; ++i)
                {
                    std::thread worker([] { Observer::count(Observer::Category::TraceRetained); });
                    worker.join();
                }
                require(Observer::nextSlot.load() == Observer::ThreadCapacity + 9, "Recycled OS thread reused slot");
                require(Observer::ledgerOverflow.load() == 9, "Overflow did not count every lost observation");
            }
            Observer::finish(path);
            require(Observer::stopped.load() && Observer::begin() == 0, "Finished capture still started timers");
            const auto before = calls(Observer::Category::TraceFiltered);
            Observer::count(Observer::Category::TraceFiltered);
            require(calls(Observer::Category::TraceFiltered) == before, "Post-finish observation accepted");
            const auto status = read(path + ".observer-status.txt");
            require(status.find("output_ok=1") != std::string::npos, "Observer output failed");
            require(status.find(mode == "overflow" ? "coverage_complete=0" : "coverage_complete=1")
                != std::string::npos, "Overflow qualification was not explicit");
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
