#include <components/resource/speculativebudget.hpp>
#include <components/resource/preloadestimate.hpp>
#include <components/resource/cachemaintenance.hpp>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>
#include <atomic>
#include <functional>

namespace
{
    unsigned passed = 0;
    void check(bool value, const char* name)
    { if (!value) throw std::runtime_error(name); ++passed; }
    template<class F> bool deferred(F&& f)
    { try { f(); } catch (const Resource::SpeculativeDeferred&) { return true; } return false; }
    constexpr auto M = Resource::SpeculativeBudget::MiB;
    Resource::OpenGlPressureSample healthy(std::uint64_t watermark = 0)
    {
        Resource::OpenGlPressureSample s;
        s.generation = 1; s.growthWatermarkValid = true; s.accountedGrowth = watermark;
        s.memory.physicalValid = s.memory.commitValid = true;
        s.memory.physicalTotal = 32 * 1024 * M; s.memory.physicalAvailable = 8 * 1024 * M;
        s.memory.commitAvailable = 20 * 1024 * M;
        s.decision.state = Resource::OpenGlPressure::Normal;
        s.decision.admissionsPerSample = 4;
        s.decision.limits.physicalReserve = 4 * 1024 * M;
        s.decision.limits.physicalCritical = 2 * 1024 * M;
        s.decision.limits.physicalRecovery = 5 * 1024 * M;
        s.decision.limits.commitReserve = 1024 * M;
        s.decision.limits.commitCritical = 256 * M;
        s.decision.limits.commitRecovery = 1536 * M;
        return s;
    }
    Resource::OpenGlPressureSample sampleFn(void* p) { return *static_cast<Resource::OpenGlPressureSample*>(p); }
    Misc::HostMemoryStatus probe() noexcept { return healthy().memory; }
}
int main()
{
    using namespace Resource;
    try
    {
        auto s = healthy(); int a = 1, b = 2;
        SpeculativeBudget ledger;
        {
            auto lease = ledger.reserve(s, 100 * M);
            check(ledger.stats().futureBytes == 100 * M, "future reserved");
            auto charge = ledger.track({&a, 1}, 40 * M, true, &lease);
            check(ledger.stats().futureBytes == 60 * M, "reservation transfers on publication");
            check(ledger.stats().knownOwnerBytes == 40 * M, "known owner bytes");
            auto alias = ledger.track({&a, 1}, 40 * M, true);
            check(alias == charge && ledger.stats().knownOwnerBytes == 40 * M, "shared payload unique");
            check(ledger.stats().sharedClaims == 1, "alias count");
            charge.reset(); check(ledger.stats().knownOwnerBytes == 40 * M, "one alias preserves charge");
            alias->pending(); check(ledger.stats().pendingOwnerBytes == 40 * M, "pending ownership counted");
            alias.reset(); check(ledger.stats().knownOwnerBytes == 0 && ledger.stats().pendingOwnerBytes == 0, "release owner");
            check(ledger.stats().observedGrowth == 40 * M, "release does not refund stale OS headroom");
        }
        check(ledger.stats().futureBytes == 0, "cancel reservation RAII");
        {
            auto estimated = ledger.track({&b, 2}, 12 * M, false);
            check(ledger.stats().estimatedOwnerBytes == 12 * M, "estimated distinct from measured");
            int c = 0; auto unknown = ledger.track({&c, 2}, 0, false);
            check(ledger.stats().unknownOwners == 1, "unknown not silently exact zero");
        }
        check(ledger.stats().unknownOwners == 0, "unknown release");
        check(deferred([&]{ auto r = ledger.reserve(s, 2048 * M); }), "oversized stage denied");
        check(deferred([&]{ auto r = ledger.reserve(s, UINT64_MAX); }), "overflow denied");
        auto invalid = s; invalid.growthWatermarkValid = false;
        check(deferred([&]{ auto r = ledger.reserve(invalid, M); }), "no unmatched OS watermark credit");
        invalid = s; invalid.memory.physicalValid = false;
        check(deferred([&]{ auto r = ledger.reserve(invalid, M); }), "invalid memory denies optional");
        invalid = s; invalid.memory.lowMemoryValid = invalid.memory.lowMemory = true;
        check(deferred([&]{ auto r = ledger.reserve(invalid, M); }), "low memory denies optional");
        invalid = s; invalid.memory.systemCommitValid = true; invalid.memory.systemCommitAvailable = 0;
        check(deferred([&]{ auto r = ledger.reserve(invalid, M); }), "independent commit pressure");
        {
            SpeculativeBudget tiny({8 * M, 4});
            auto old = healthy(); old.memory.physicalAvailable = old.decision.limits.physicalReserve + 5 * M;
            auto lease = tiny.reserve(old, 4 * M);
            auto charge = tiny.track({&a,1},4 * M,true,&lease);
            check(deferred([&]{auto next = tiny.reserve(old, 2 * M);}), "surviving result consumes old headroom");
            charge.reset();
            check(deferred([&]{auto next = tiny.reserve(old, 2 * M);}), "fast completion no stale refill");
            old.generation = 2; old.accountedGrowth = tiny.watermark()->load();
            auto next = tiny.reserve(old, 2 * M);
            check(tiny.stats().futureBytes == 2 * M, "fresh sample reconciles growth exactly once");
        }
        {
            SpeculativeBudget test;
            auto lease = test.reserve(s, 3 * M);
            auto charge = test.track({&a,1}, 2 * M, true, &lease);
            auto current = healthy(test.watermark()->load()); current.generation = 2;
            auto next = test.reserve(current, 4 * M);
            check(test.stats().futureBytes == 5 * M, "sampled allocations not recharged as futures");
            check(test.stats().knownOwnerBytes == 2 * M, "retained remains after reconciliation");
        }
        {
            std::vector<SpeculativeBudget::Job> jobs;
            for (int i=0;i<4;++i) jobs.push_back(ledger.tryJob(s));
            check(ledger.stats().jobs == 4, "four job cap");
            check(!ledger.tryJob(s), "fifth job denied");
            jobs.clear(); check(ledger.stats().jobs == 0, "job cancellation releases count");
            for (int i=0;i<10;++i) { auto j = ledger.tryJob(s); check(bool(j), "steady not four-per-second throttle"); }
            auto ramp = s; ramp.generation = 2; ramp.decision.admissionsPerSample = 1;
            { auto j = ledger.tryJob(ramp); check(bool(j), "ramp first"); }
            check(!ledger.tryJob(ramp), "ramp rate guard");
            ramp.generation = 3; {auto j = ledger.tryJob(ramp); check(bool(j), "ramp next sample");}
        }
        {
            SpeculativeBudget prioritized({1024 * M, 4, 1});
            std::vector<SpeculativeBudget::Job> background;
            for (int i = 0; i < 3; ++i)
                background.push_back(prioritized.tryJob(s));
            check(background[0] && background[1] && background[2], "priority lane keeps three background jobs");
            check(!prioritized.tryJob(s), "priority lane reserves fourth slot");
            auto near = prioritized.tryJob(s, SpeculativePriority::NearFuture);
            check(bool(near) && prioritized.stats().priorityJobs == 1, "near-future job uses reserved slot");
            check(!prioritized.tryJob(s, SpeculativePriority::NearFuture), "one reserved near-future slot");
        }
        {
            SpeculativeBudget prioritized({1024 * M, 4, 1});
            auto caution = healthy();
            caution.decision.state = OpenGlPressure::Caution;
            caution.decision.admissionsPerSample = 0;
            caution.memory.physicalAvailable = 3 * 1024 * M;
            caution.memory.commitAvailable = 2 * 1024 * M;
            check(!prioritized.tryJob(caution), "background denied during caution");
            auto near = prioritized.tryJob(caution, SpeculativePriority::NearFuture);
            check(bool(near), "near-future admitted during caution");
            auto lease = prioritized.reserve(caution, 128 * M, SpeculativePriority::NearFuture);
            check(lease.future() == 128 * M, "near-future uses critical-floor headroom");
            check(deferred([&] {
                auto tooTight = caution;
                tooTight.generation = 2;
                tooTight.memory.physicalAvailable = tooTight.decision.limits.physicalCritical + 64 * M;
                auto rejected = prioritized.reserve(tooTight, 128 * M, SpeculativePriority::NearFuture);
            }), "near-future denied before critical floor");
        }
        {
            SpeculativeBudget prioritized({1024 * M, 4, 1});
            auto recovering = healthy();
            recovering.decision.state = OpenGlPressure::Recovering;
            recovering.decision.admissionsPerSample = 0;
            { auto near = prioritized.tryJob(recovering, SpeculativePriority::NearFuture);
              check(bool(near), "near-future admitted during recovery"); }
            check(!prioritized.tryJob(recovering, SpeculativePriority::NearFuture),
                "near-future recovery ramp one per sample");
            recovering.generation = 2;
            { auto near = prioritized.tryJob(recovering, SpeculativePriority::NearFuture);
              check(bool(near), "near-future recovery next sample"); }
            auto critical = recovering;
            critical.generation = 3;
            critical.decision.state = OpenGlPressure::Critical;
            check(!prioritized.tryJob(critical, SpeculativePriority::NearFuture),
                "near-future denied under critical pressure");
            auto degraded = recovering;
            degraded.generation = 4;
            degraded.decision.state = OpenGlPressure::Degraded;
            check(!prioritized.tryJob(degraded, SpeculativePriority::NearFuture),
                "near-future denied under degraded pressure");
        }
        {
            auto claim = ledger.claim("17|nif|mesh");
            check(deferred([&]{auto duplicate = ledger.claim("17|nif|mesh");}), "optional duplicate defers instead of blocking");
            auto independent = ledger.claim("18|nif|mesh");
            check(ledger.stats().duplicateRequests == 1, "generation independent claim");
        }
        { auto again = ledger.claim("17|nif|mesh"); check(true,"claim released on unwind"); }
        {
            SpeculativeBudget nested;
            auto local = healthy();
            SpeculativeScope scope(&nested, &sampleFn, &local);
            SpeculativeBudget::ChargePtr output;
            {
                SpeculativeScope::Stage parent(10 * M, 5 * M);
                {
                    SpeculativeScope::Stage child(4 * M, 2 * M);
                    output = SpeculativeScope::track(&nested, {&a,2},0,false);
                    check(nested.stats().futureBytes == 12 * M,"nested stage transfer only child");
                }
                check(nested.stats().futureBytes == 10 * M,"child scratch released");
            }
            check(nested.stats().futureBytes == 0 && nested.stats().estimatedOwnerBytes == 2 * M,"output survives whole job");
            check(scope.retainedEstimate() == 2 * M,"worker output receipt");
        }
        check(!SpeculativeScope::active(),"scope restored on exit");
        {
            SpeculativeScope::Stage disabled(UINT64_MAX,UINT64_MAX);
            check(true,"demand does not acquire optional reservation");
        }
        {
            auto ptr = std::make_unique<SpeculativeBudget>();
            auto charge = ptr->track({&a,1},M,true); ptr.reset();
            charge.reset(); check(true,"owner ticket can safely outlive coordinator");
        }
        {
            SpeculativeBudget concurrent;
            std::atomic<bool> go{false}; std::atomic<unsigned> active{0}, peak{0};
            std::vector<std::thread> threads;
            for (int n=0;n<12;++n) threads.emplace_back([&]{
                while(!go.load(std::memory_order_acquire)) std::this_thread::yield();
                for(int i=0;i<100;++i) {
                    auto job = concurrent.tryJob(s); if(!job) continue;
                    const auto x = ++active; auto prev=peak.load();
                    while(x>prev && !peak.compare_exchange_weak(prev,x)) {}
                    auto stage = concurrent.reserve(s,M);
                    std::this_thread::yield(); --active;
                }
            });
            go.store(true,std::memory_order_release); for(auto& t:threads)t.join();
            check(peak <= 4 && concurrent.stats().jobs == 0 && concurrent.stats().futureBytes == 0,"concurrent leases");
        }
        {
            SpeculativeBudget sampled;
            OpenGlPressureMonitor monitor({},probe,OpenGlPressureMonitor::milliseconds,false,sampled.watermark());
            auto charge = sampled.track({&a,1},M,true);
            check(monitor.read().accountedGrowth == 0,"query start watermark not retroactively changed");
            monitor.refresh(); check(monitor.read().accountedGrowth == M,"next sample absorbs prior growth");
        }
        {
            std::array<unsigned char,148> h{};
            auto put=[&](int at,std::uint32_t x){for(int i=0;i<4;++i)h[at+i]=static_cast<unsigned char>(x>>(i*8));};
            put(0,0x20534444);put(4,124);put(12,1024);put(16,2048);
            auto e=estimateImage(h,2*M);check(e.knownDimensions && e.retained == 32*M,"DDS dimensions estimate");
            put(112,0x200); auto cube=estimateImage(h,2*M); check(cube.retained == 6*e.retained,"cube estimate");
            put(16,0xffffffff);put(12,0xffffffff);check(estimateImage(h,M).peak == UINT64_MAX,"malicious header overflow saturates");
            std::istringstream stream("abcdef");stream.seekg(2);auto before=stream.tellg();
            auto estimate=estimateStream(stream,false);(void)estimate;
            check(stream.tellg()==before && stream.get()=='c',"stream position restored");
            auto unknown=estimateImage({},M);check(!unknown.knownDimensions && unknown.peak>128*M,"unknown decoder conservative fallback");
        }
        {
            CacheMaintenanceBudget budget(2,1,std::chrono::seconds(1));
            CacheMaintenanceScope scope(budget);
            check(budget.scan() && budget.scan() && !budget.scan(),"global scan ceiling");
            check(budget.release() && !budget.release(),"global release ceiling");
            check(CacheMaintenanceScope::current()==&budget,"maintenance scope");
        }
        check(CacheMaintenanceScope::current()==nullptr,"maintenance restored");
        {
            CacheMaintenanceBudget budget(10,10,std::chrono::seconds(1));
            check(budget.release(128*M) && !budget.release(1),"oversized first retirement makes progress then stops");
        }
        std::cout << "PASS " << passed << " P1B budget, lifecycle, concurrency and estimate checks\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
