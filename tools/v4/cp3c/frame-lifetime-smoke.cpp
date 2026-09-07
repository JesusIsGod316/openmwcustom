#include <components/render/backend/vsg/framecompletion.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C frame lifetime failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    RenderVsg::FrameCompletionTracker completion(2);
    if (!require(completion.canSubmit(RenderCore::FrameId{ 10 }), "preflight submit 10")
        || !require(completion.submit(RenderCore::FrameId{ 10 }), "submit 10")
        || !require(completion.submit(RenderCore::FrameId{ 11 }), "submit 11")
        || !require(completion.atCapacity(), "capacity")
        || !require(!completion.canSubmit(RenderCore::FrameId{ 12 }), "over-capacity preflight rejected")
        || !require(!completion.submit(RenderCore::FrameId{ 12 }), "over-capacity submit rejected")
        || !require(!completion.submit(RenderCore::FrameId{ 11 }), "duplicate submit rejected")
        || !require(!completion.completeThrough(RenderCore::FrameId{ 12 }), "future completion rejected"))
        return EXIT_FAILURE;

    RenderVsg::FrameRetirementQueue<std::shared_ptr<int>> retirements;
    const auto first = std::make_shared<int>(1);
    const auto second = std::make_shared<int>(2);
    if (!require(retirements.queue(RenderCore::FrameId{ 10 }, first), "queue first")
        || !require(retirements.queue(RenderCore::FrameId{ 11 }, second), "queue second")
        || !require(completion.completeThrough(RenderCore::FrameId{ 10 }), "complete 10"))
        return EXIT_FAILURE;

    auto retired = retirements.collect(*completion.lastCompleted());
    if (!require(retired.size() == 1 && *retired.front() == 1, "only completed object retired")
        || !require(retirements.size() == 1, "future object retained")
        || !require(completion.completeThrough(RenderCore::FrameId{ 11 }), "complete 11")
        || !require(retirements.collect(*completion.lastCompleted()).size() == 1, "second object retired"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C frame lifetime: PASS\n";
    return EXIT_SUCCESS;
}
