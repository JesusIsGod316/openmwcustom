#ifndef OPENMW_MWRENDER_FOGSTATE_H
#define OPENMW_MWRENDER_FOGSTATE_H

namespace MWRender
{
    enum class FogDistanceMode
    {
        Planar,
        Radial,
    };

    enum class FogFalloffMode
    {
        Linear,
        Exponential,
    };

    struct FogColorState
    {
        float red = 0.f;
        float green = 0.f;
        float blue = 0.f;
        float alpha = 1.f;
    };

    // Renderer-neutral snapshot of the authoritative fog controller. Keeping
    // this POD free of OSG, GLM and VSG lets the engine publish one result to
    // either backend without exposing a renderer-owned manager.
    struct FogState
    {
        FogColorState color;
        float start = 0.f;
        float end = 0.f;
        FogDistanceMode distanceMode = FogDistanceMode::Planar;
        FogFalloffMode falloffMode = FogFalloffMode::Linear;
        bool enabled = false;
        bool underwater = false;
    };
}

#endif
