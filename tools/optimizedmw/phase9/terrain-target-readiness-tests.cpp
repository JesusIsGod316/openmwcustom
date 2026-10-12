#include <components/loadinglistener/loadinglistener.hpp>
#include <components/sceneutil/terraintargetreadiness.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }
    template <class Predicate>
    void waitUntil(Predicate ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!ready())
        {
            require(std::chrono::steady_clock::now() < deadline, "fixture worker timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

int main()
try
{
    Loading::Listener listener;
    // This is the production coordinator used by TerrainPreloadItem, including
    // its real Loading::Reporter wait boundary and publication status.
    SceneUtil::TerrainTargetReadiness targets(4);
    std::array<int, 4> privateOutput{};
    int completedMetadata = 0;
    std::vector<std::size_t> order;
    std::atomic_bool futureStarted{ false }, releaseFuture{ false }, demandDone{ false }, demandFailed{ false };
    std::jthread worker([&] {
        targets.run(
            0,
            [&](Loading::Reporter& reporter) {
                reporter.addTotal(1);
                privateOutput[0] = 42;
                reporter.addProgress(1);
                order.push_back(0);
                return true;
            },
            [&](bool ready) noexcept { completedMetadata = ready ? 1234 : -1; });
        targets.run(1, [&](Loading::Reporter&) {
            futureStarted.store(true, std::memory_order_release);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!releaseFuture.load(std::memory_order_acquire))
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            privateOutput[1] = 43;
            order.push_back(1);
            return true;
        });
        while (const auto index = targets.nextPending())
            targets.run(*index, [&](Loading::Reporter&) {
                privateOutput[*index] = static_cast<int>(42 + *index);
                order.push_back(*index);
                return true;
            });
        targets.cancelPending();
    });
    targets.wait(0, listener);
    waitUntil([&] { return futureStarted.load(std::memory_order_acquire); });
    require(privateOutput[0] == 42 && completedMetadata == 1234 && targets.completed() == std::vector<std::size_t>{ 0 },
        "first target wait joined delayed future work or published unfinished targets");
    std::jthread demander([&] {
        Loading::Listener demandListener;
        try
        {
            targets.wait(3, demandListener);
        }
        catch (...)
        {
            demandFailed.store(true, std::memory_order_release);
        }
        demandDone.store(true, std::memory_order_release);
    });
    waitUntil([&] { return targets.demanded(3); });
    require(!demandDone.load() && targets.canServe(3), "pending demand was advertised ready or discarded");
    releaseFuture.store(true, std::memory_order_release);
    worker.join();
    demander.join();
    require(!demandFailed.load() && privateOutput[3] == 45 && order == std::vector<std::size_t>{ 0, 1, 3, 2 },
        "demand promotion failed to prioritize the next complete private view");
    require(targets.completed() == std::vector<std::size_t>({ 0, 1, 2, 3 }),
        "complete target set publication lost an owner");

    SceneUtil::TerrainTargetReadiness cancelled(3);
    cancelled.run(0, [](Loading::Reporter&) { return true; });
    cancelled.run(1, [](Loading::Reporter&) { return false; });
    cancelled.cancelPending();
    require(cancelled.completed() == std::vector<std::size_t>{ 0 } && !cancelled.canServe(1) && !cancelled.canServe(2),
        "cancelled/deferred private views were published or remained pending forever");
    cancelled.wait(0, listener);
    for (const std::size_t index : { 1u, 2u })
    {
        bool unavailable = false;
        try
        {
            cancelled.wait(index, listener);
        }
        catch (const SceneUtil::TerrainTargetUnavailable&)
        {
            unavailable = true;
        }
        require(unavailable, "cancelled demanded target did not request the required fallback");
    }

    SceneUtil::TerrainTargetReadiness failure(3);
    failure.run(0, [](Loading::Reporter&) { return true; });
    require(!failure.run(1, [](Loading::Reporter&) -> bool { throw std::runtime_error("future view failure"); }),
        "failed target was successful");
    failure.cancelPending();
    failure.wait(0, listener);
    bool propagated = false;
    try
    {
        failure.wait(1, listener);
    }
    catch (const std::runtime_error& error)
    {
        propagated = std::string(error.what()) == "future view failure";
    }
    require(propagated && failure.completed() == std::vector<std::size_t>{ 0 },
        "future failure poisoned a complete current target or lost its own cause");

    SceneUtil::TerrainTargetReadiness firstFailure(2);
    firstFailure.run(0, [](Loading::Reporter&) -> bool { throw std::runtime_error("required view failure"); });
    firstFailure.cancelPending();
    propagated = false;
    try
    {
        firstFailure.wait(0, listener);
    }
    catch (const std::runtime_error& error)
    {
        propagated = std::string(error.what()) == "required view failure";
    }
    require(propagated && firstFailure.completed().empty(), "required view failure falsely published readiness");

    std::cout << "PASS: first complete view before delayed future, immutable completed subsets, pending demand "
                 "promotion, cancellation/defer fallback and per-target exception propagation\n";
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
