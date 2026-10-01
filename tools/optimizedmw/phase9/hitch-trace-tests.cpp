#include <components/debug/hitchtrace.hpp>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

using namespace Debug::HitchTrace;
void spin()
{
    const auto end = Clock::now() + std::chrono::microseconds(350);
    while (Clock::now() < end) {}
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
#ifdef _WIN32
    _putenv_s("OPENMW_P9_HITCH_SPANS_FILE", std::string_view(argv[2]) == "unarmed" ? "0" : argv[1]);
#else
    setenv("OPENMW_P9_HITCH_SPANS_FILE", std::string_view(argv[2]) == "unarmed" ? "0" : argv[1], 1);
#endif
    auto& capture = Capture::instance();
    if (std::string_view(argv[2]) == "unarmed")
    {
        { Scope unarmed("unarmed", CpuTiming::Scoped); spin(); }
        if (capture.enabled()) return 4;
        capture.finish(); return 0;
    }
    if (!capture.enabled()) return 3;
    if (std::string_view(argv[2]) == "overflow")
    {
        Row r{};
        r.cpu = -1;
        for (std::size_t i = 0; i < Capture::Capacity + 7; ++i)
        { r.id = i + 1; capture.append(r); }
    }
    else
    {
        { Scope parent("parent", "identity,with\"quote", "asset.nif", 123);
          { Scope child("child", {}, {}, 123, CpuTiming::Scoped); spin(); }
          { Scope filtered("fast_child", {}, {}, 123); } }
        try { Scope unwind("exception"); spin(); throw 1; } catch (...) {}
        std::vector<std::thread> workers;
        for (int i = 0; i < 4; ++i)
            workers.emplace_back([] {
                for (int j = 0; j < 60; ++j)
                { Scope root("worker", {}, {}, 456); Scope child("worker_child", {}, {}, 456); spin(); }
            });
        for (auto& worker : workers) worker.join();
        { Scope truncated("truncated", std::string(300, 'x')); spin(); }
    }
    capture.finish();
    std::cout << "hitch timeline fixture complete\n";
}
