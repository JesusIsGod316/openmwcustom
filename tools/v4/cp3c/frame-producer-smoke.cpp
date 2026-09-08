#include <components/rendercore/frameproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C frame producer failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::SingleViewFrameProducer producer;
    RenderCore::SingleViewFrameInput input;
    input.renderExtent = { 1280, 720 };
    input.outputExtent = input.renderExtent;
    input.environment.skyEnabled = false;
    input.environment.fogEnabled = true;
    input.environment.fogStart = 2048.0f;
    input.environment.fogEnd = 4096.0f;
    input.environment.fogDistanceMode = RenderCore::FogDistanceMode::Radial;
    input.environment.fogFalloffMode = RenderCore::FogFalloffMode::Exponential;
    input.environment.interior = true;
    input.environment.sunLightEnabled = true;
    input.environment.sunVisible = false;

    auto first = producer.produce(world, input);
    if (!require(first && first->valid(), "first frame")
        || !require(first->frameId() == RenderCore::FrameId{ 1 }, "first frame id")
        || !require(first->environment().sunLightEnabled && !first->environment().sunVisible,
            "interior directional light remains independent from visible sun")
        || !require(!first->historyValid() && !first->views().front().historyValid, "cold history"))
        return EXIT_FAILURE;

    input.camera.worldPosition.x = 5.0;
    input.camera.view[3][0] = -5.0f;
    auto second = producer.produce(world, input);
    if (!require(second && second->frameId() == RenderCore::FrameId{ 2 }, "second frame id")
        || !require(second->historyValid() && second->views().front().historyValid, "continuous history")
        || !require(second->views().front().previous.worldPosition.x == 0.0, "previous camera retained"))
        return EXIT_FAILURE;

    input.outputExtent = { 1920, 1080 };
    auto resized = producer.produce(world, input);
    if (!require(resized && !resized->historyValid(), "extent change invalidates history")
        || !require(resized->historyEpoch() == RenderCore::HistoryEpoch{ 2 }, "history epoch advanced"))
        return EXIT_FAILURE;

    input.invalidateHistory = true;
    auto explicitCut = producer.produce(world, input);
    if (!require(explicitCut && !explicitCut->historyValid(), "explicit camera cut")
        || !require(explicitCut->historyEpoch() == RenderCore::HistoryEpoch{ 3 }, "camera-cut history epoch"))
        return EXIT_FAILURE;

    input.invalidateHistory = false;
    input.renderExtent = {};
    if (!require(!producer.produce(world, input), "invalid frame rejected without publication")
        || !require(producer.nextFrameId() == RenderCore::FrameId{ 5 }, "invalid frame does not consume id"))
        return EXIT_FAILURE;

    input.renderExtent = input.outputExtent;
    input.environment.fogEnd = input.environment.fogStart;
    if (!require(!producer.produce(world, input), "invalid enabled fog range rejected")
        || !require(producer.nextFrameId() == RenderCore::FrameId{ 5 }, "invalid fog does not consume id"))
        return EXIT_FAILURE;
    input.environment.fogEnd = 4096.0f;
    input.environment.fogDistanceMode = static_cast<RenderCore::FogDistanceMode>(255);
    if (!require(!producer.produce(world, input), "unknown fog distance mode rejected")
        || !require(producer.nextFrameId() == RenderCore::FrameId{ 5 }, "unknown fog mode does not consume id"))
        return EXIT_FAILURE;
    input.environment.fogDistanceMode = RenderCore::FogDistanceMode::Radial;
    input.environment.fogFalloffMode = static_cast<RenderCore::FogFalloffMode>(255);
    if (!require(!producer.produce(world, input), "unknown fog falloff mode rejected")
        || !require(producer.nextFrameId() == RenderCore::FrameId{ 5 }, "unknown fog falloff does not consume id"))
        return EXIT_FAILURE;
    input.environment.fogFalloffMode = RenderCore::FogFalloffMode::Exponential;
    if (!require(world.reset(), "world epoch reset"))
        return EXIT_FAILURE;
    auto newWorld = producer.produce(world, input);
    if (!require(newWorld && newWorld->frameId() == RenderCore::FrameId{ 5 }, "frame id after rejected input")
        || !require(!newWorld->historyValid(), "world epoch invalidates history")
        || !require(newWorld->historyEpoch() == RenderCore::HistoryEpoch{ 4 }, "world history epoch"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C frame producer: PASS\n";
    return EXIT_SUCCESS;
}
