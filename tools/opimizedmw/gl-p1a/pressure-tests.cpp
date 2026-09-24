#include <components/resource/hostmemorybudget.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using Policy = Resource::OpenGlPressurePolicy;
    using Monitor = Resource::OpenGlPressureMonitor;
    using State = Resource::OpenGlPressure;
    using Admission = Resource::PreloadAdmission;
    constexpr std::uint64_t GiB = 1024ull * 1024 * 1024;
    constexpr std::uint64_t MiB = GiB / 1024;
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    Misc::HostMemoryStatus healthy()
    {
        auto m = Misc::HostMemoryStatus{32 * GiB, 8 * GiB, 25 * GiB, 40 * GiB, true, true, true};
        m.systemCommitAvailable = 40 * GiB;
        m.systemCommitValid = true;
        m.lowMemoryValid = true;
        return m;
    }
    std::atomic<std::uint64_t> now{1000};
    std::atomic<unsigned> queries{0};
    std::atomic<bool> queryEntered{false}, blockQuery{false}, releaseQuery{false};
    std::uint64_t clockNow() noexcept { return now.load(); }
    Misc::HostMemoryStatus query() noexcept
    {
        ++queries;
        if (blockQuery.load())
        {
            queryEntered = true;
            while (!releaseQuery.load()) std::this_thread::yield();
        }
        return healthy();
    }
    Resource::OpenGlPressureDecision recover(Policy& p, const Misc::HostMemoryStatus& m,
        std::uint64_t start = 1000)
    {
        Resource::OpenGlPressureDecision result;
        for (auto t = start; t <= start + 5000; t += 1000) result = p.update(m, t);
        return result;
    }
}

int main()
{
    unsigned passed = 0, failed = 0;
    const auto test = [&](const char* name, auto&& body) {
        try { body(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };
    test("private commit 25 GiB with 8 GiB available is healthy", [] {
        Policy p; const auto d = p.update(healthy(), 0);
        require(d.state == State::Normal && d.admissionsPerSample == 4, "private commit treated as resident RAM");
    });
    test("invalid private counter cannot prevent physical recovery", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 0);
        m = healthy(); m.processValid = false;
        require(recover(p, m).state == State::Normal, "private counter made critical sticky");
    });
    test("valid zero commit escalates despite invalid physical counter", [] {
        Policy p; auto m = healthy(); m.physicalValid = false; m.commitAvailable = 0;
        require(p.update(m, 0).state == State::Critical, "physical failure hid commit exhaustion");
    });
    test("valid system commit exhaustion is independent", [] {
        Policy p; auto m = healthy(); m.physicalValid = false; m.commitValid = false; m.systemCommitAvailable = 0;
        require(p.update(m, 0).state == State::Critical, "system commit ignored");
    });
    test("low memory event overrides high numerical counters", [] {
        Policy p; auto m = healthy(); m.lowMemory = true;
        require(p.update(m, 0).state == State::Critical, "OS low memory event ignored");
    });
    test("invalid low memory flag is not a true event", [] {
        Policy p; auto m = healthy(); m.lowMemory = true; m.lowMemoryValid = false;
        require(p.update(m, 0).state == State::Normal, "invalid event invented shortage");
    });
    test("OS event alone may escalate but not prove recovery", [] {
        Policy p; Misc::HostMemoryStatus m; m.lowMemoryValid = m.lowMemory = true;
        require(p.update(m, 0).state == State::Critical, "event-only critical missed");
        m.lowMemory = false;
        require(p.update(m, 1000).state == State::Degraded, "clear event alone proved abundant headroom");
    });
    test("automatic 32 GiB physical bands and independent commit bands", [] {
        const auto b = Policy{}.limits(32 * GiB);
        require(b.physicalReserve == 4 * GiB && b.physicalCritical == 2 * GiB
            && b.physicalRecovery == 5 * GiB, "physical bands");
        require(b.commitReserve == GiB && b.commitCritical == 256 * MiB
            && b.commitRecovery == GiB + GiB / 2, "commit bands coupled to RAM");
    });
    test("physical thresholds scale with usable RAM", [] {
        require(Policy{}.limits(16 * GiB).physicalReserve == 2 * GiB, "16 GiB scaling");
        require(Policy{}.limits(0).physicalReserve == 512 * MiB, "unknown-total fallback");
    });
    test("reserve override and pathological inputs have safe arithmetic", [] {
        Resource::OpenGlPressureConfig c; c.physicalReserve = 2 * GiB; c.commitReserve = 2 * GiB;
        auto b = Policy{c}.limits(32 * GiB);
        require(b.physicalReserve == 2 * GiB && b.physicalRecovery == 5 * GiB / 2, "override ignored");
        c.physicalReserve = c.commitReserve = std::numeric_limits<std::uint64_t>::max();
        b = Policy{c}.limits(std::numeric_limits<std::uint64_t>::max());
        require(b.physicalRecovery > b.physicalReserve && b.commitRecovery > b.commitReserve, "overflow");
    });
    test("caution stops new speculation before physical critical band", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 3 * GiB;
        const auto d = p.update(m, 0);
        require(d.state == State::Caution && d.admissionsPerSample == 0, "caution still admits");
    });
    test("physical critical band escalates immediately", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB;
        require(p.update(m, 0).state == State::Critical, "critical missed");
    });
    test("caution and critical commit bands are independent", [] {
        Policy p; auto m = healthy(); m.commitAvailable = 512 * MiB;
        require(p.update(m, 0).state == State::Caution, "commit caution missed");
        m.commitAvailable = 128 * MiB;
        require(p.update(m, 1000).state == State::Critical, "commit critical missed");
    });
    test("all missing counters degrade without destructive pressure", [] {
        Policy p; const auto d = p.update({}, 0);
        require(d.state == State::Degraded && !d.admissionsPerSample, "unknown was normal or destructive");
    });
    test("inverted physical counter still respects other critical signals", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = 33 * GiB;
        require(p.update(m, 0).state == State::Degraded, "inverted sample accepted");
        m.systemCommitAvailable = 0;
        require(p.update(m, 1000).state == State::Critical, "valid system signal discarded");
    });
    test("optional system probe failure does not block process-commit recovery", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 0);
        m = healthy(); m.systemCommitValid = false;
        require(recover(p, m).state == State::Normal, "optional system API required");
    });
    test("system headroom can cover a missing process-commit probe", [] {
        Policy p; auto m = healthy(); m.commitValid = false;
        require(p.update(m, 0).state == State::Normal, "system headroom not usable");
    });
    test("no commit headroom counter enters degraded not unlimited", [] {
        Policy p; auto m = healthy(); m.commitValid = m.systemCommitValid = false;
        require(p.update(m, 0).state == State::Degraded, "missing commit allowed new work");
    });
    test("recovery needs five continuously sampled healthy seconds then ramps", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 0);
        for (std::uint64_t t = 1000; t < 6000; t += 1000)
        {
            auto d = p.update(healthy(), t);
            require(d.state == State::Recovering && !d.admissionsPerSample, "recovered too early");
        }
        require(p.update(healthy(), 6000).admissionsPerSample == 1, "no slow start");
        for (std::uint64_t t = 7000; t < 11000; t += 1000)
            require(p.update(healthy(), t).admissionsPerSample == 1, "refill storm during ramp");
        require(p.update(healthy(), 11000).admissionsPerSample == 4, "ramp never ended");
    });
    test("low recovery band never refills optional work", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 0);
        m.physicalAvailable = 4 * GiB + GiB / 2;
        for (unsigned i = 1; i <= 20; ++i)
            require(!p.update(m, i * 1000).admissionsPerSample, "refilled below recovery reserve");
    });
    test("missing data clears sticky critical but still denies growth", [] {
        Policy p; auto m = healthy(); m.lowMemory = true; p.update(m, 0);
        require(p.update({}, 1000).state == State::Degraded, "critical stuck on failed probe");
        require(recover(p, healthy(), 2000).state == State::Normal, "cannot recover from degraded");
    });
    test("long sample gap restarts recovery hold", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 0);
        p.update(healthy(), 1000);
        require(p.update(healthy(), 10000).state == State::Recovering, "stale gap counted healthy");
    });
    test("clock reversal restarts recovery without underflow", [] {
        Policy p; auto m = healthy(); m.physicalAvailable = GiB; p.update(m, 10000);
        require(p.update(healthy(), 9000).state == State::Recovering, "clock reversal refilled");
        require(recover(p, healthy(), 10000).state == State::Normal, "clock recovery stuck");
    });
    test("normal monitor reads never call OS probe", [] {
        now = 1000; queries = 0; Monitor m({}, &query, &clockNow, false);
        for (unsigned i = 0; i < 10000; ++i) require(m.read().generation == 1, "lost coherent sample");
        require(queries == 1, "reader queried OS");
    });
    test("stale monitor snapshot denies growth without fake zero-byte pressure", [] {
        now = 1000; Monitor m({}, &query, &clockNow, false);
        now = 4001; const auto s = m.read();
        require(s.generation == 0 && s.decision.state == State::Degraded
            && s.decision.admissionsPerSample == 0, "stale snapshot usable");
    });
    test("published generation advances only after completed sample", [] {
        now = 1000; Monitor m({}, &query, &clockNow, false);
        now = 2000; m.refresh();
        const auto s = m.read();
        require(s.generation == 2 && s.sampledAtMs == 2000 && s.memory.privateCommit == 25 * GiB,
            "incoherent publication");
    });
    test("blocked OS query cannot block cached frame reads", [] {
        now = 1000; blockQuery = false; releaseQuery = false; queryEntered = false;
        Monitor m({}, &query, &clockNow, false);
        blockQuery = true;
        std::thread writer([&] { m.refresh(); });
        while (!queryEntered.load()) std::this_thread::yield();
        auto reader = std::async(std::launch::async, [&] {
            for (unsigned i = 0; i < 1000; ++i) if (m.read().generation != 1) return false;
            return true;
        });
        const auto status = reader.wait_for(std::chrono::seconds(1));
        releaseQuery = true; writer.join(); blockQuery = false;
        const bool coherent = reader.get();
        require(status == std::future_status::ready && coherent, "read waited behind OS query");
    });
    test("many fast jobs cannot recycle the same sample allowance", [] {
        Admission a;
        for (unsigned i = 0; i < 4; ++i)
            require(a.reserveOpenGl(healthy(), 1, 4, 4 * GiB, GiB).has_value(), "healthy allowance denied");
        require(a.stats().pending == 0 && a.stats().released == 4, "short jobs did not finish");
        require(!a.reserveOpenGl(healthy(), 1, 4, 4 * GiB, GiB), "stale sample reused after completion");
        require(a.reserveOpenGl(healthy(), 2, 4, 4 * GiB, GiB).has_value(), "fresh sample not usable");
    });
    test("older sample cannot reset admission generation", [] {
        Admission a;
        require(a.reserveOpenGl(healthy(), 2, 4, 4 * GiB, GiB).has_value(), "setup");
        require(!a.reserveOpenGl(healthy(), 1, 4, 4 * GiB, GiB), "generation rolled backward");
    });
    test("recovery grants only one job per sample", [] {
        Admission a;
        require(a.reserveOpenGl(healthy(), 1, 1, 4 * GiB, GiB).has_value(), "ramp cannot progress");
        require(!a.reserveOpenGl(healthy(), 1, 1, 4 * GiB, GiB), "ramp admitted multiple jobs");
    });
    test("invalid sample does not grant admission", [] {
        Admission a;
        require(!a.reserveOpenGl({}, 0, 4, 0, 0), "empty sample admitted");
        require(!a.reserveOpenGl(healthy(), 1, 0, 4 * GiB, GiB), "recovery hold bypassed");
    });
    test("high private commit is not a hidden admission limit", [] {
        Admission a; auto m = healthy(); m.privateCommit = 100 * GiB;
        require(a.reserveOpenGl(m, 1, 4, 4 * GiB, GiB).has_value(), "private soft cap still active");
    });
    test("physical and commit burst margins checked independently", [] {
        Admission a; auto m = healthy(); m.physicalAvailable = 4 * GiB + 128 * MiB;
        require(!a.reserveOpenGl(m, 1, 4, 4 * GiB, GiB), "physical margin oversubscribed");
        m = healthy(); m.commitAvailable = GiB + 128 * MiB;
        require(!a.reserveOpenGl(m, 1, 4, 4 * GiB, GiB), "process commit margin oversubscribed");
        m = healthy(); m.systemCommitAvailable = GiB + 128 * MiB;
        require(!a.reserveOpenGl(m, 1, 4, 4 * GiB, GiB), "system commit margin oversubscribed");
    });
    test("concurrent reservations cannot exceed four even across fresh samples", [] {
        Admission a; std::atomic<unsigned> arrived{0}; std::atomic<bool> done{false};
        std::vector<std::thread> jobs;
        for (unsigned i = 0; i < 16; ++i) jobs.emplace_back([&] {
            auto r = a.reserveOpenGl(healthy(), 1, 4, 4 * GiB, GiB); ++arrived;
            while (!done.load()) std::this_thread::yield();
        });
        while (arrived.load() != 16) std::this_thread::yield();
        const auto stats = a.stats();
        const bool extra = a.reserveOpenGl(healthy(), 2, 4, 4 * GiB, GiB).has_value();
        done = true; for (auto& j : jobs) j.join();
        require(stats.pending == 4 && stats.admitted == 4 && !extra, "concurrent oversubscription");
        require(a.stats().pending == 0 && a.stats().released == 4, "reservation refund error");
    });
    test("GL lease can outlive admission owner", [] {
        std::optional<Admission::Reservation> lease;
        { Admission a; lease = a.reserveOpenGl(healthy(), 1, 4, 4 * GiB, GiB); }
        lease.reset();
    });
    test("disabled coordinator allocates no sampler and performs no query", [] {
        queries = 0; Resource::HostMemoryBudget b(&query);
        for (unsigned i = 0; i < 1000; ++i)
        {
            require(b.pressure() == Resource::HostMemoryPressure::Normal, "disabled pressure changed");
            require(b.reserveOptionalPreload().has_value(), "disabled admission changed");
        }
        require(!b.openGlEnabled() && queries == 0 && b.preloadAdmissionStats().admitted == 0,
            "disabled control has sampler/accounting work");
    });
    test("sampler shutdown wakes sleeping thread and joins owner", [] {
        now = 1000; queries = 0;
        { Monitor m({}, &query, &clockNow, true); require(m.read().generation == 1, "no startup sample"); }
        require(queries == 1, "shutdown failed to stop sampler");
    });
    test("native OpenGL OS probe has coherent independent validity flags", [] {
        const auto m = Misc::queryOpenGlHostMemoryStatus();
#ifdef _WIN32
        require(m.physicalValid && m.commitValid && m.processValid, "Windows numerical probe failed");
        require(m.systemCommitValid && m.lowMemoryValid, "Windows independent probes failed");
        require(m.physicalAvailable <= m.physicalTotal, "physical sample invalid");
#else
        require(!m.physicalValid && !m.commitValid && !m.systemCommitValid && !m.lowMemoryValid,
            "unsupported platform invented data");
#endif
    });
    std::cout << passed << '/' << passed + failed << " GL-P1A tests passed\n";
    return failed ? 1 : 0;
}
