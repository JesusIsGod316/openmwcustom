#ifndef OPENMW_MWRENDER_V4RUNTIMEOPTIONS_H
#define OPENMW_MWRENDER_V4RUNTIMEOPTIONS_H

#include <components/render/backend/vsg/vsgruntimebootstrap.hpp>

namespace MWRender
{
    // Captures the established video settings without exposing Settings types
    // to the backend. The Vulkan bootstrap can therefore create the one
    // authoritative application window with the same user policy as OpenGL.
    [[nodiscard]] RenderVsg::VsgRuntimeBootstrapOptions makeV4RuntimeBootstrapOptions();
}

#endif
