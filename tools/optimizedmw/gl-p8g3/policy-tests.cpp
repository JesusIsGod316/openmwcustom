#include <components/sceneutil/groundcoverpolicy.hpp>
#include <components/sceneutil/prepjobservice.hpp>

#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

namespace P = SceneUtil::GroundcoverPolicy;
static unsigned int checks = 0;
void require(bool condition, const char* name)
{
    ++checks;
    if (!condition) throw std::runtime_error(name);
}

int main()
{
    try
    {
        P::Options options;
        std::vector<P::Instance> instances(4096);
        for (std::size_t i = 0; i < instances.size(); ++i)
        {
            instances[i].identity = i + 42;
            instances[i].rank = P::rank(i + 42);
            instances[i].position = { static_cast<float>(i % 64) * 64, static_cast<float>(i / 64) * 64, 0 };
        }
        const auto leaves = P::partition(instances, options, 2, true);
        require(leaves.size() == 8, "dense group subdivides within eight-leaf cap");
        std::set<std::size_t> seen;
        for (const auto& leaf : leaves)
        {
            require(leaf.size() >= options.minInstances, "minimum batch size");
            for (const auto id : leaf) require(seen.insert(id).second, "no duplicate instances across tiles");
        }
        require(seen.size() == instances.size(), "partition preserves complete population");
        require(P::partition(instances, options, 2, false).size() == 1, "hierarchy-off preserves one group");
        require(P::partition(instances, options, 16, true).size() <= 2, "material-heavy models respect draw budget");
        require(P::partition(std::span(instances).first(64), options, 1, true).size() == 1, "small groups not split");
        auto compact = instances;
        for (auto& value : compact) value.position = { 0, 0, 0 };
        require(P::partition(compact, options, 1, true).size() == 1, "coincident groups not split");
        require(leaves == P::partition(instances, options, 2, true), "partition deterministic");

        P::sortRanks(instances);
        require(P::tierCount(instances, 0) == instances.size(), "full prefix includes all");
        const auto mid = P::tierCount(instances, 1), far = P::tierCount(instances, 2);
        require(far < mid && mid < instances.size(), "nested prefixes reduce actual count");
        require(far > 1900 && far < 2350, "far subset representative");
        for (float distance : { 0.f, 4999.f, 5000.f, 6000.f, 9000.f, 13000.f, 25000.f })
            for (float projected : { 0.f, .005f, .01f, .025f, .1f })
            {
                const float d = P::density(distance, projected, options);
                const auto tier = P::desiredTier(d);
                const auto count = P::tierCount(instances, tier);
                for (std::size_t i = count; i < instances.size(); ++i)
                    require(P::opacity(instances[i].rank, d) == 0.f, "removed prefix tail must be fully invisible");
                for (unsigned int previous = 0; previous < 3; ++previous)
                {
                    const auto chosen = P::hystereticTier(previous, tier, d);
                    require(chosen <= tier, "hysteresis can only retain higher detail");
                }
            }
        require(P::density(4999.f, 0.f, options) == 1.f, "near grass full density");
        require(P::density(50000.f, .1f, options) == 1.f, "large projected plants preserved");
        require(P::desiredTier(std::numeric_limits<float>::quiet_NaN()) == 0, "invalid density full detail");
        require(P::hystereticTier(2, 0, 1.f) == 0, "camera cut refines immediately");
        require(P::hystereticTier(0, 2, .45f) == 2, "far tier not held permanently by hysteresis");
        for (float z : { 0.f, 0.5f, -1.f, 2.1f })
        {
            const auto p = P::rotate({ 2, 3, 4 }, { 0, 0, z });
            require(std::abs(p[0] - (2 * std::cos(z) + 3 * std::sin(z))) < .00001f, "literal shader rotation x");
            require(std::abs(p[1] - (-2 * std::sin(z) + 3 * std::cos(z))) < .00001f, "literal shader rotation y");
            require(p[2] == 4, "z-only height preserved");
        }
        for (float wind : { -70.f, -5.f, -1.f, 0.f, .5f, 1.f, 4.f, 70.f })
        {
            const float bound = P::windMargin(wind);
            for (int t = 0; t < 200; ++t)
            {
                const double v = std::sqrt(2.0 * wind * wind + 1.0), time = t * .183;
                const double x = (2 * wind + .1) * ((1 - .10 * v) * std::sin(time)
                    + (1 - .04 * v) * std::cos(2 * time) + (1 + .14 * v) * std::sin(3 * time)
                    + (1 + .28 * v) * std::sin(5 * time));
                require(std::abs(x) + 60 < bound, "analytic margin covers four harmonics plus stomp");
                const double pv = std::sqrt(2.0 * (wind + .3) * (wind + .3) + 1.0);
                const double c = std::max(0.0, std::cos(3.7 * time));
                const double gust = .8 * c * c + .4 * std::sin(3.7 * time);
                for (bool fast : { false, true })
                {
                    const double h = (1 - .13 * pv) * std::sin(2.3 * time)
                        + (fast ? 0.0 : (1 - .17 * pv) * std::cos(4.4 * time))
                        + (1 + .41 * pv) * gust;
                    const double displacement = (2.0 * (wind + .3) + .1) * h + 40.0 * wind;
                    require(std::abs(displacement) + 60 < bound, "deployed PBR gust and long lean bounded");
                }
            }
        }
        require(!std::isfinite(P::windMargin(std::numeric_limits<float>::infinity())), "invalid wind fails open");
        auto invalid = instances[0]; invalid.scale = -1;
        require(!P::valid(invalid), "negative scale falls back");

        std::vector<float> serial(4096), parallel(4096);
        for (std::size_t i = 0; i < serial.size(); ++i) serial[i] = P::rank(i);
        const auto caller = std::this_thread::get_id();
        std::thread::id worker;
        auto& jobs = SceneUtil::PrepJobService::instance();
        const bool paired = jobs.runPair(SceneUtil::PrepJobService::Lane::Background,
            [&] { worker = std::this_thread::get_id(); for (std::size_t i = 0; i < 2048; ++i) parallel[i] = P::rank(i); },
            [&] { for (std::size_t i = 2048; i < 4096; ++i) parallel[i] = P::rank(i); });
        require(paired && worker != caller && parallel == serial, "real reserved-worker numeric parity");
        bool innerAccepted = true;
        jobs.runPair(SceneUtil::PrepJobService::Lane::Background, [] {}, [&] {
            innerAccepted = jobs.runPair(SceneUtil::PrepJobService::Lane::Background, [] {}, [] {});
        });
        require(!innerAccepted, "occupied lane falls back without queueing");
        std::cout << checks << " P8G3 policy checks passed\n";
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return EXIT_FAILURE; }
}
