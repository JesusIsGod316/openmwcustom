#include <components/sceneutil/pagingvertexcache.hpp>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <vector>

namespace
{
    using namespace SceneUtil::PagingVertexCache;
    unsigned checks = 0;
    std::uint64_t unchangedSeen = 0;
    void require(bool value, const char* description)
    {
        if (!value) throw std::runtime_error(description);
        ++checks;
    }

    void exact(const std::vector<unsigned>& input, unsigned vertices)
    {
        std::vector<unsigned> control{123456}, candidate{123456}, wrapped{123456};
        Statistics baseline, reduced, reset;
        const auto old = optimize(input, vertices, control, [] { return false; }, baseline);
        const auto current = optimize(input, vertices, candidate, [] { return false; }, reduced, true);
        require(old == current && control == candidate, "exact triangle order and status match control");
        const auto overflow = optimize(input, vertices, wrapped, [] { return false; }, reset, true,
            std::numeric_limits<std::uint64_t>::max());
        require(overflow == old && wrapped == control, "generation rollover preserves exact order");
        require(reduced.scoreUpdates <= baseline.scoreUpdates, "rescoring work cannot grow");
        require(reduced.heapComparisons <= baseline.heapComparisons, "heap work cannot grow");
        unchangedSeen += reduced.unchangedHeapUpdates;
        if (old == Result::Complete && control.size() > 3)
            require(reset.generationResets == 1, "rollover path actually exercised");
    }
}

int main()
{
    try
    {
        exact({}, 0);
        exact({0, 1}, 2);
        exact({0, 1, 3}, 3);
        exact({0, 0, 1, 0, 1, 1, 0, 1, 2, 2, 1, 0}, 3);
        std::mt19937 random(9371);
        for (unsigned trial = 0; trial < 700; ++trial)
        {
            const unsigned vertices = 3 + random() % 257;
            const unsigned triangles = random() % 900;
            std::vector<unsigned> input;
            input.reserve(triangles * 3);
            for (unsigned i = 0; i < triangles * 3; ++i) input.push_back(random() % vertices);
            exact(input, vertices);
        }
        std::vector<unsigned> islands;
        for (unsigned i = 0; i < 2000; ++i) islands.insert(islands.end(), {3 * i, 3 * i + 1, 3 * i + 2});
        exact(islands, 6000);
        for (unsigned trial = 0; trial < 35; ++trial)
        {
            std::vector<unsigned> components;
            for (unsigned island = 0; island < 12; ++island)
                for (unsigned triangle = 0; triangle < 100; ++triangle)
                    for (unsigned vertex = 0; vertex < 3; ++vertex)
                        components.push_back(island * 25 + random() % 25);
            exact(components, 300);
        }
        std::vector<unsigned> fan;
        // First two isolated triangles force heap activation before a connected,
        // high-valence region, exercising duplicate visits and unchanged updates.
        fan.insert(fan.end(), {0, 1, 2, 3, 4, 5});
        for (unsigned i = 7; i < 2500; ++i) fan.insert(fan.end(), {6, i, i + 1});
        exact(fan, 2501);
        std::vector<unsigned> old, reduced;
        Statistics original, candidate;
        require(optimize(fan, 2501, old, [] { return false; }, original) == Result::Complete,
            "high-valence baseline completes");
        require(optimize(fan, 2501, reduced, [] { return false; }, candidate, true) == Result::Complete,
            "high-valence candidate completes");
        require(candidate.scoreReuses > 0 && candidate.scoreUpdates < original.scoreUpdates,
            "candidate actually eliminates rescores");
        require(candidate.heapComparisons < original.heapComparisons,
            "candidate actually eliminates duplicate heap repairs");
        require(unchangedSeen > 0, "mixed disconnected components exercise unchanged score updates");
        for (unsigned cancelAt : {1u, 5u, 500u, 10000u, 20000u, 100000u})
        {
            unsigned calls = 0;
            Statistics stats;
            std::vector<unsigned> unchanged{71, 72};
            require(optimize(fan, 2501, unchanged, [&] { return ++calls >= cancelAt; }, stats, true)
                    == Result::Cancelled && unchanged == std::vector<unsigned>({71, 72}),
                "cancellation remains transactional through scoring and selection");
        }
        std::cout << checks << " exact paging checks passed; high-valence score updates "
                  << original.scoreUpdates << " -> " << candidate.scoreUpdates
                  << ", heap comparisons " << original.heapComparisons << " -> " << candidate.heapComparisons
                  << "; synthetic algorithm work counts, not gameplay performance\n";
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return EXIT_FAILURE;
    }
}
