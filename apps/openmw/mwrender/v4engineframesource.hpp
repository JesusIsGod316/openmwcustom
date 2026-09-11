#ifndef OPENMW_MWRENDER_V4ENGINEFRAMESOURCE_H
#define OPENMW_MWRENDER_V4ENGINEFRAMESOURCE_H

#include <components/rendercore/frameproducer.hpp>

#include <vector>

namespace MWRender
{
    // Immutable backend-neutral main-view input captured after gameplay/world
    // update and before render submission. The current OSG camera controller is
    // only one possible producer of this contract.
    struct V4MainFrameSource
    {
        RenderCore::CameraState camera;
        double simulationTime = 0.0;
        double frameDelta = 0.0;
        float lodScale = 1.0f;
        RenderCore::FrameEnvironmentState environment;
        std::vector<RenderCore::DynamicTransformInput> dynamicTransforms;
        std::vector<RenderCore::SkeletonPoseInput> skeletonPoses;
        std::vector<RenderCore::MorphWeightInput> morphWeights;
        std::vector<RenderCore::ImmediateEffectDraw> immediateEffectDraws;
        bool invalidateHistory = false;
    };
}

#endif
