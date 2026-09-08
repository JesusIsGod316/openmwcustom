#ifndef OPENMW_MWRENDER_V4RENDERROUTESTATUS_H
#define OPENMW_MWRENDER_V4RENDERROUTESTATUS_H

#include <string>
#include <string_view>

namespace MWRender
{
    // Main-thread status shared by the V4 application bridge and its scene
    // observer. The first source/publication failure is sticky: an engine-level
    // catch must not allow a later frame to present a partially published world.
    class V4RenderRouteStatus final
    {
    public:
        void fail(std::string_view diagnostic) noexcept
        {
            if (!mHealthy)
                return;
            mHealthy = false;
            try
            {
                mFirstDiagnostic = diagnostic;
            }
            catch (...)
            {
            }
        }

        [[nodiscard]] bool healthy() const noexcept { return mHealthy; }
        [[nodiscard]] const std::string& firstDiagnostic() const noexcept { return mFirstDiagnostic; }

    private:
        bool mHealthy = true;
        std::string mFirstDiagnostic;
    };
}

#endif
