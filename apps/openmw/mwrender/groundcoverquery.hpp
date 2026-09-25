#ifndef OPENMW_MWRENDER_GROUNDCOVERQUERY_H
#define OPENMW_MWRENDER_GROUNDCOVERQUERY_H

#include "groundcoverdata.hpp"

namespace MWWorld
{
    class GroundcoverStore;
}

namespace MWRender
{
    // Renderer-neutral selection of winning groundcover references. This owns
    // file merge, deletion override, density thinning and chunk-border filtering
    // without constructing an OpenGL/OSG Groundcover renderer object.
    [[nodiscard]] GroundcoverInstanceMap collectGroundcoverInstances(const MWWorld::GroundcoverStore& store,
        float density, float size, float centerX, float centerY);
}

#endif
