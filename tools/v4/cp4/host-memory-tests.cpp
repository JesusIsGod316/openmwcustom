#include <components/resource/hostmemorybudget.hpp>

#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Pressure = Resource::HostMemoryPressure;
    using Policy = Resource::HostMemoryPolicy;
    constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;
    void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
    Misc::HostMemoryStatus healthy()
    {
        return {32 * GiB, 12 * GiB, 8 * GiB, 16 * GiB, true, true, true};
    }
    std::atomic<unsigned> queries{0};
    Misc::HostMemoryStatus testQuery() noexcept { ++queries; return healthy(); }
}
int main()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, auto&& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("real OS probe reports valid Windows counters without header leakage", [] {
        const auto m = Misc::queryHostMemoryStatus();
#ifdef _WIN32
        require(m.physicalValid && m.processValid && m.commitValid, "Windows memory probe unavailable");
        require(m.physicalTotal > 0 && m.physicalAvailable <= m.physicalTotal, "incoherent physical sample");
#else
        require(!m.physicalValid && !m.processValid && !m.commitValid, "unsupported host invented counters");
#endif
    });
    test("32 GiB thresholds are explicit soft limits", [] {
        const auto b = Policy::limits(32 * GiB);
        require(b.reserve == 4 * GiB && b.criticalReserve == GiB && b.recoveryReserve == 5 * GiB,
            "physical thresholds changed");
        require(b.privateSoft == 24 * GiB && b.privateCritical == 28 * GiB && b.privateRecovery == 23 * GiB,
            "private thresholds changed");
    });
    test("normal useful cache residency does not cause trimming", [] {
        Policy p; require(p.update(healthy(), 0) == Pressure::Normal, "healthy pressure");
    });
    test("physical shortage triggers without process accounting", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB; m.processValid = false;
        require(p.update(m, 0) == Pressure::Trim, "missed physical shortage");
    });
    test("critical physical shortage escalates immediately", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB / 2;
        require(p.update(m, 0) == Pressure::Critical, "missed critical pressure");
    });
    test("private soft limit gates optional retention even with free RAM", [] {
        Policy p; auto m = healthy(); m.privateCommit = 24 * GiB;
        require(p.update(m, 0) == Pressure::Trim, "private budget ignored");
        m.privateCommit = 28 * GiB;
        require(p.update(m, 1000) == Pressure::Critical, "private critical budget ignored");
    });
    test("commit headroom is checked separately", [] {
        Policy p; auto m = healthy(); m.commitAvailable = 3 * GiB;
        require(p.update(m, 0) == Pressure::Trim, "commit shortage ignored");
        m.commitAvailable = GiB / 2;
        require(p.update(m, 1000) == Pressure::Critical, "critical commit shortage ignored");
    });
    test("missing counters are not zero-byte emergencies", [] {
        Policy p; require(p.update({}, 0) == Pressure::Normal, "invalid data invented pressure");
        auto m = healthy(); m.commitAvailable = 0; m.commitValid = false;
        m.privateCommit = 100 * GiB; m.processValid = false;
        require(p.update(m, 1000) == Pressure::Normal, "invalid secondary data used");
    });
    test("inverted physical sample is rejected", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 33 * GiB;
        require(p.update(m, 0) == Pressure::Normal, "inverted sample used");
    });
    test("five healthy seconds required before refilling", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB;
        require(p.update(m, 0) == Pressure::Trim, "setup");
        require(p.update(healthy(), 1000) == Pressure::Trim, "refilled immediately");
        require(p.update(healthy(), 5999) == Pressure::Trim, "refilled before hold elapsed");
        require(p.update(healthy(), 6000) == Pressure::Normal, "did not recover");
    });
    test("low-band oscillation cannot trigger refill", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB / 2; p.update(m, 0);
        m.physicalAvailable = 4 * GiB + GiB / 2;
        for (unsigned i = 1; i < 20; ++i)
            require(p.update(m, i * 1000) != Pressure::Normal, "refilled within hysteresis band");
    });
    test("invalid sample breaks healthy hold without disabling pressure", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB; p.update(m, 0);
        p.update(healthy(), 1000); p.update({}, 4000);
        require(p.update(healthy(), 7000) == Pressure::Trim, "missing data treated as recovery");
        require(p.update(healthy(), 12000) == Pressure::Normal, "valid recovery failed");
    });
    test("partial valid sample cannot satisfy recovery", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB; p.update(m, 0);
        m = healthy(); m.processValid = false;
        p.update(m, 1000);
        require(p.update(m, 100000) != Pressure::Normal, "partial data refilled caches");
    });
    test("backwards time restarts rather than underflows hold", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB; p.update(m, 0);
        p.update(healthy(), 10000);
        require(p.update(healthy(), 9000) == Pressure::Trim, "clock underflow recovered");
        require(p.update(healthy(), 14000) == Pressure::Normal, "restarted hold failed");
    });
    test("reported capture low headroom is critical", [] {
        Policy p; auto m = healthy(); m.physicalTotal = 33617727488ull;
        m.physicalAvailable = 384126976ull; m.privateCommit = 26952732672ull;
        require(p.update(m, 0) == Pressure::Critical, "observed memory pressure was missed");
    });
    test("disabled monitor does not query the OS", [] {
        queries = 0; Resource::HostMemoryBudget monitor(&testQuery);
        for (unsigned i = 0; i < 1000; ++i) require(monitor.pressure() == Pressure::Normal, "disabled policy ran");
        require(queries == 0, "disabled query overhead");
        for (unsigned i = 0; i < 10; ++i) require(monitor.reserveOptionalPreload().has_value(), "legacy preload denied");
        require(queries == 0 && monitor.preloadAdmissionStats().admitted == 0, "control reserved memory");
    });
    test("optional reservations cover queue concurrency and release exactly once", [] {
        Resource::PreloadAdmission admission; auto m = healthy(); auto limits = Policy::limits(m.physicalTotal);
        std::vector<Resource::PreloadAdmission::Reservation> leases;
        for (unsigned i = 0; i < 4; ++i)
        {
            auto lease = admission.reserve(m, false, limits.reserve, limits.privateSoft);
            require(lease.has_value(), "healthy admission failed"); leases.push_back(std::move(*lease));
        }
        require(!admission.reserve(m, false, limits.reserve, limits.privateSoft), "unbounded optional queue");
        require(admission.stats().reservedEstimate == GiB, "reservation estimate mismatch");
        leases.erase(leases.begin()); // move assignment must release replaced owner
        require(admission.stats().pending == 3 && admission.stats().released == 1, "moved lease lost or double refund");
        require(admission.reserve(m, false, limits.reserve, limits.privateSoft).has_value(), "refunded slot not reusable");
        leases.clear(); require(admission.stats().pending == 0 && admission.stats().reservedEstimate == 0, "reservation leaked");
    });
    test("pre-allocation headroom includes concurrent reservations", [] {
        Resource::PreloadAdmission admission; auto m = healthy(); auto limits = Policy::limits(m.physicalTotal);
        m.physicalAvailable = limits.reserve + Resource::PreloadAdmission::Allowance;
        auto first = admission.reserve(m, false, limits.reserve, limits.privateSoft);
        require(first.has_value(), "one allowance should fit");
        require(!admission.reserve(m, false, limits.reserve, limits.privateSoft), "headroom double-admitted");
        first.reset(); m = healthy(); m.privateCommit = limits.privateSoft;
        require(!admission.reserve(m, false, limits.reserve, limits.privateSoft), "private budget ignored");
        m = healthy(); m.commitAvailable = limits.reserve;
        require(!admission.reserve(m, false, limits.reserve, limits.privateSoft), "commit budget ignored");
        require(!admission.reserve(healthy(), true, limits.reserve, limits.privateSoft), "pressure hysteresis bypassed");
        require(admission.reserve({}, false, 0, 0).has_value(), "unavailable sample invented shortage");
    });
    test("reservation destruction remains safe after admission owner destruction", [] {
        std::optional<Resource::PreloadAdmission::Reservation> survivor;
        { Resource::PreloadAdmission admission; survivor = admission.reserve({}, false, 0, 0); }
        survivor.reset();
    });
    test("concurrent optional admission cannot overbook or lose cancellation refunds", [] {
        Resource::PreloadAdmission admission;
        std::atomic<unsigned> finished{0};
        std::atomic<bool> release{false};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 16; ++i)
            threads.emplace_back([&] {
                auto lease = admission.reserve(healthy(), false, 4 * GiB, 24 * GiB);
                ++finished;
                while (!release.load()) std::this_thread::yield();
            });
        while (finished.load() != 16) std::this_thread::yield();
        const auto during = admission.stats();
        release.store(true);
        for (auto& thread : threads) thread.join();
        require(during.pending == 4 && during.admitted == 4 && during.denied == 12,
            "concurrent admission exceeded capacity");
        require(admission.stats().pending == 0 && admission.stats().released == 4,
            "cancelled concurrent owners did not refund exactly once");
    });
    test("independently valid commit counters still gate partial OS samples", [] {
        Resource::PreloadAdmission admission; auto m = healthy();
        m.physicalValid = false; m.commitAvailable = 0;
        require(!admission.reserve(m, false, GiB, 24 * GiB), "valid exhausted commit was ignored");
        m.commitValid = false; m.privateCommit = 24 * GiB;
        require(!admission.reserve(m, false, GiB, 24 * GiB), "valid private limit was ignored");
    });
    test("concurrent monitor callers share one sample", [] {
        queries = 0; Resource::HostMemoryBudget monitor(&testQuery); monitor.setEnabled(true);
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 8; ++i)
            threads.emplace_back([&] { for (unsigned j = 0; j < 100; ++j) static_cast<void>(monitor.pressure()); });
        for (auto& thread : threads) thread.join();
        require(queries == 1, "query was not rate limited across workers");
        require(monitor.snapshot().physicalTotal == 32 * GiB, "snapshot publication failed");
    });
    std::cout << passed << '/' << passed + failed << " host memory tests passed\n";
    return failed ? 1 : 0;
}
