#include <components/debug/hitchtrace.hpp>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>
#include <fstream>
#include <map>
#include <atomic>

using namespace Debug::HitchTrace;
void spin()
{
    const auto end = Clock::now() + std::chrono::microseconds(350);
    while (Clock::now() < end) {}
}
std::map<std::string,std::string> status(const std::string& path)
{
    std::map<std::string,std::string> result;
    std::ifstream in(path + ".capture-status.txt");
    std::string line;
    while(std::getline(in,line))
    {
        const auto delimiter=line.find('=');
        if(delimiter!=std::string::npos) result[line.substr(0,delimiter)]=line.substr(delimiter+1);
    }
    return result;
}
struct Gate { std::atomic<bool> entered{false}, release{false}; };
void pauseConsumer(void* context) noexcept
{
    auto& gate=*static_cast<Gate*>(context); gate.entered.store(true,std::memory_order_release);
    while(!gate.release.load(std::memory_order_acquire)) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
Row fixtureRow(std::uint64_t id)
{
    Row row{}; row.id=id; row.thread=threadId(); row.frame=static_cast<unsigned>(id/100+1);
    row.start=static_cast<std::int64_t>(id)*1000000; row.end=row.start+350000;
    row.epoch=1790891672756+static_cast<std::int64_t>(id/1000); row.cpu=-1;
    std::memcpy(row.phase.data(),"fixture.volume",sizeof("fixture.volume"));
    std::memcpy(row.identity.data(),"volume,\"quoted\"",sizeof("volume,\"quoted\""));
    std::memcpy(row.asset.data(),"meshes/volume.nif",sizeof("meshes/volume.nif"));
    std::memcpy(row.metrics[0].name.data(),"fixture_id",sizeof("fixture_id")); row.metrics[0].value=id;
    return row;
}
int transportFixture(const std::string& path,std::string_view mode)
{
    Capture::Options options; options.path=path; options.streaming=true; options.capacity=131072;
    Gate gate;
    if(mode=="stream-overflow" || mode=="stream-stop")
    { options.capacity=8; options.beforeConsumerRead=&pauseConsumer; options.consumerContext=&gate; }
    if(mode=="stream-init-failure") options.capacity=3;
    if(mode=="stream-concurrent") options.capacity=128;
    if(mode=="stream-failure") std::filesystem::create_directory(path);
    Capture capture(std::move(options));
    if(!capture.enabled()) return 11;
    if(mode=="stream-overflow" || mode=="stream-stop")
    {
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        while(!gate.entered.load(std::memory_order_acquire) && Clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if(!gate.entered.load()) { gate.release.store(true); capture.finish(); return 12; }
        if(mode=="stream-stop")
        {
            capture.append(fixtureRow(1));
            std::thread closer([&capture] { capture.finish(); });
            while(capture.enabled()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            capture.append(fixtureRow(2));
            gate.release.store(true,std::memory_order_release);
            closer.join();
        }
        else for(std::uint64_t i=1;i<=15;++i) capture.append(fixtureRow(i));
        gate.release.store(true,std::memory_order_release);
    }
    else if(mode=="stream-volume")
    {
        // More rows than the historical full arena and ten minutes at the
        // measured average rate. Fixture pacing waits outside append(), so
        // it tests transport reuse without changing the nonblocking producer.
        for(std::uint64_t i=1;i<=2500000;++i)
        {
            capture.append(fixtureRow(i));
            if((i & 1023)==0)
                while(capture.pendingRows()>32768) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    else if(mode=="stream-concurrent")
    {
        std::vector<std::thread> producers;
        for(std::uint64_t worker=0;worker<4;++worker)
            producers.emplace_back([&capture,worker] {
                for(std::uint64_t j=1;j<=10000;++j)
                {
                    // Caller pacing is outside append and deliberately leaves
                    // enough slots for all four producers to race publication.
                    while(capture.pendingRows()>32) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    capture.append(fixtureRow(worker*10000+j));
                }
            });
        for(auto& producer:producers) producer.join();
    }
    else if(mode=="stream-zero") {}
    else if(mode=="stream-active") { capture.enter(); capture.append(fixtureRow(1)); }
    else
        for(std::uint64_t i=1;i<=3;++i) capture.append(fixtureRow(i));
    capture.finish();
    if(mode=="stream-active") capture.leave();
    const auto s=status(path);
    const auto expect=[&](const char* key,const char* value) { const auto found=s.find(key); return found!=s.end()&&found->second==value; };
    if(mode=="stream-overflow")
        return expect("attempted","15")&&expect("accepted","8")&&expect("written","8")
            &&expect("capacity_dropped","7")&&expect("pending","0")?0:13;
    if(mode=="stream-stop")
        return expect("attempted","2")&&expect("accepted","1")&&expect("written","1")
            &&expect("stopped_dropped","1")&&expect("pending","0")?0:18;
    if(mode=="stream-init-failure")
        return expect("attempted","3")&&expect("accepted","0")&&expect("allocation_dropped","3")
            &&expect("initialization_failed","1")&&expect("output_ok","0")?0:14;
    if(mode=="stream-failure")
        return expect("attempted","3")&&expect("accepted","3")&&expect("written","0")
            &&expect("io_dropped","3")&&expect("writer_failed","1")&&expect("output_ok","0")?0:15;
    if(mode=="stream-active") return expect("active_at_finish","1")&&expect("output_ok","0")?0:16;
    const char* expected=mode=="stream-zero"?"0":mode=="stream-concurrent"?"40000":"2500000";
    return expect("attempted",expected)&&expect("accepted",expected)
        &&expect("written",expected)&&expect("dropped","0")
        &&expect("drain_complete","1")&&expect("output_ok","1")?0:17;
}
int main(int argc, char** argv)
{
    if (argc != 3) return 2;
#ifdef _WIN32
    _putenv_s("OPENMW_P9_HITCH_SPANS_FILE", std::string_view(argv[2]) == "unarmed" ? "0" : argv[1]);
#else
    setenv("OPENMW_P9_HITCH_SPANS_FILE", std::string_view(argv[2]) == "unarmed" ? "0" : argv[1], 1);
#endif
    const std::string_view mode=argv[2];
    if(mode=="stream-volume" || mode=="stream-overflow" || mode=="stream-init-failure"
        || mode=="stream-failure" || mode=="stream-zero" || mode=="stream-active" || mode=="stream-stop"
        || mode=="stream-concurrent")
        return transportFixture(argv[1],mode);
    if(mode=="stream")
    {
#ifdef _WIN32
        _putenv_s("OPENMW_P9_HITCH_TRANSPORT","stream");
#else
        setenv("OPENMW_P9_HITCH_TRANSPORT","stream",1);
#endif
    }
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
        { Scope assetArgument("asset_argument", {}, "legacy-asset.nif"); assetArgument.retain(); }
        std::uint64_t request = 0;
        { Scope source("request", CpuTiming::None); source.retain(); request = source.id();
          source.setMetric(0, "requested_bytes", 8192); }
        std::thread dependency([request] {
            Scope worker("dependency", CpuTiming::None); worker.retain(); worker.setLink(request);
            worker.setMetric(0, "returned_bytes", 4096);
        });
        dependency.join();
        {
            Scope envelope("wall_envelope", {}, {}, 122, CpuTiming::WallEnvelope);
            {
                Scope innerEnvelope("inner_wall_envelope", {}, {}, 122, CpuTiming::WallEnvelope);
                Scope root("root_through_envelope", {}, {}, 122);
                spin();
            }
        }
        {
            Scope root("cpu_root_above_envelope", {}, {}, 122);
            Scope envelope("nested_wall_envelope", {}, {}, 122, CpuTiming::WallEnvelope);
            Scope nested("root_below_cpu_root", {}, {}, 122);
            spin();
        }
        {
            Scope none("legacy_none_parent", {}, {}, 122, CpuTiming::None);
            Scope envelope("wall_below_legacy_none", {}, {}, 122, CpuTiming::WallEnvelope);
            Scope nested("root_below_legacy_none", {}, {}, 122);
            spin();
        }
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
