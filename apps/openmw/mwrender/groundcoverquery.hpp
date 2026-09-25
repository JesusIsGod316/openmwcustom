#ifndef OPENMW_MWRENDER_GROUNDCOVERQUERY_H
#define OPENMW_MWRENDER_GROUNDCOVERQUERY_H

#include "groundcoverdata.hpp"

namespace MWRender
{
    class Groundcover;

    // Neutral query seam used by non-OSG render backends. The implementation
    // delegates to the established winning-file merge/density/border selection
    // inside Groundcover without exposing OSG vectors or scene objects.
    [[nodiscard]] GroundcoverInstanceMap collectGroundcoverInstances(
        const Groundcover& groundcover, float size, float centerX, float centerY);
}

#endif
