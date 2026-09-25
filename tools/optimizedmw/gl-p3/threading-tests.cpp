#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <components/resource/speculativebudget.hpp>
#include <components/sceneutil/boundedtwowaywork.hpp>
#include <components/sceneutil/pagingwork.hpp>

namespace
{
    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            std::exit(1);
        }
    }

    Resource::OpenGlPressureSample sample(void* owner)
    {
        return *static_cast<Resource::OpenGlPressureSample*>(owner);
    }

    Resource::OpenGlPressureSample healthySample()
    {
        Resource::OpenGlPressureSample value;
        constexpr std::uint64_t gib = 1024ull * 1024 * 1024;
        value.memory.physicalValid = true;
        value.memory.physicalTotal = 32 * gib;
        value.memory.physicalAvailable = 16 * gib;
        value.memory.commitValid = true;
        value.memory.commitAvailable = 16 * gib;
        value.decision.state = Resource::OpenGlPressure::Normal;
        value.decision.limits.physicalReserve = 2 * gib;
        value.decision.limits.commitReserve = 2 * gib;
        value.decision.admissionsPerSample = 4;
        value.generation = 1;
        value.growthWatermarkValid = true;
        return value;
    }
}

int main()
{
    Resource::SpeculativeBudget::Config config;
    config.transientLimit = 1024 * Resource::SpeculativeBudget::MiB;
    config.maximumJobs = 4;
    Resource::SpeculativeBudget budget(config);
    Resource::OpenGlPressureSample pressure = healthySample();

    std::atomic<bool> cancel{ false };
    Resource::SpeculativeScope parent(&budget, &sample, &pressure);
    SceneUtil::PagingWorkScope paging(&cancel, SceneUtil::PagingWorkScope::Phase::OptionalOptimization);
    const auto speculativeContext = Resource::SpeculativeScope::capture();
    const auto pagingContext = SceneUtil::PagingWorkScope::capture();

    require(speculativeContext.budget == &budget, "speculative context did not capture budget");
    require(pagingContext.cancel == &cancel, "paging context did not capture cancellation");
    require(pagingContext.phase == SceneUtil::PagingWorkScope::Phase::OptionalOptimization,
        "paging phase was not captured");

    std::uint64_t helperRetained = 0;
    std::atomic<bool> helperSawScope{ false };
    std::atomic<bool> helperSawPaging{ false };
    const bool parallel = SceneUtil::BoundedTwoWayWork::run(32, 16,
        [&](std::size_t begin, std::size_t end, bool helper) {
            if (!helper)
            {
                for (std::size_t i = begin; i < end; ++i)
                    SceneUtil::PagingWorkScope::checkpoint();
                return;
            }

            Resource::SpeculativeScope child(speculativeContext);
            SceneUtil::PagingWorkScope childPaging(pagingContext);
            helperSawScope.store(Resource::SpeculativeScope::active(), std::memory_order_release);
            helperSawPaging.store(SceneUtil::PagingWorkScope::optionalOptimization(), std::memory_order_release);
            int identity = 0;
            Resource::SpeculativeScope::Stage stage(
                1 * Resource::SpeculativeBudget::MiB, 4096);
            auto charge = Resource::SpeculativeScope::track(
                &budget, { &identity, 77 }, 4096, true);
            require(static_cast<bool>(charge), "helper charge missing");
            for (std::size_t i = begin; i < end; ++i)
                SceneUtil::PagingWorkScope::checkpoint();
            helperRetained = child.retainedEstimate();
        });

    require(parallel, "coarse two-way work did not activate");
    require(helperSawScope.load(std::memory_order_acquire), "helper did not inherit speculative scope");
    require(helperSawPaging.load(std::memory_order_acquire), "helper did not inherit paging phase");
    require(helperRetained == 4096, "helper retained estimate was not captured");
    Resource::SpeculativeScope::creditRetainedEstimate(helperRetained);
    require(parent.retainedEstimate() == 4096, "helper retained bytes were not credited to parent");

    std::thread::id helperId1;
    std::thread::id helperId2;
    require(SceneUtil::BoundedTwoWayWork::run(32, 16,
        [&](std::size_t, std::size_t, bool helper) {
            if (helper) helperId1 = std::this_thread::get_id();
        }), "persistent helper first reuse probe failed");
    require(SceneUtil::BoundedTwoWayWork::run(32, 16,
        [&](std::size_t, std::size_t, bool helper) {
            if (helper) helperId2 = std::this_thread::get_id();
        }), "persistent helper second reuse probe failed");
    require(helperId1 != std::thread::id{} && helperId1 == helperId2,
        "coarse helper thread was recreated between runs");

    {
        std::atomic<bool> requiredCancel{ false };
        SceneUtil::PagingWorkScope required(
            &requiredCancel, SceneUtil::PagingWorkScope::Phase::RequiredReadiness);
        const auto requiredContext = SceneUtil::PagingWorkScope::capture();
        std::atomic<bool> helperSawRequired{ false };
        require(SceneUtil::BoundedTwoWayWork::run(32, 16,
            [&](std::size_t, std::size_t, bool helper) {
                if (!helper) return;
                SceneUtil::PagingWorkScope inherited(requiredContext);
                helperSawRequired.store(SceneUtil::PagingWorkScope::requiredReadiness(),
                    std::memory_order_release);
            }), "required-readiness helper probe failed");
        require(helperSawRequired.load(std::memory_order_acquire),
            "helper did not inherit required-readiness phase");
    }

    cancel.store(true, std::memory_order_release);
    std::atomic<bool> cancelled{ false };
    std::thread cancellationThread([&] {
        SceneUtil::PagingWorkScope childPaging(pagingContext);
        try { SceneUtil::PagingWorkScope::checkpoint(); }
        catch (const SceneUtil::PagingWorkCancelled&) { cancelled.store(true, std::memory_order_release); }
    });
    cancellationThread.join();
    require(cancelled.load(std::memory_order_acquire), "helper did not inherit cancellation");

    std::cout << "OptimizedMW GL-P3 threading context tests passed\n";
    return 0;
}
