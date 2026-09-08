#include <apps/openmw/mwrender/v4renderroutestatus.hpp>

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C render route status failure: " << message << '\n';
        return condition;
    }
}

int main()
{
    MWRender::V4RenderRouteStatus status;
    if (!require(status.healthy(), "new route is healthy")
        || !require(status.firstDiagnostic().empty(), "new route has no diagnostic"))
        return EXIT_FAILURE;

    status.fail("winning VFS model could not be published");
    if (!require(!status.healthy(), "publication failure closes route")
        || !require(status.firstDiagnostic() == "winning VFS model could not be published",
            "first publication diagnostic retained"))
        return EXIT_FAILURE;

    status.fail("later cleanup failure");
    if (!require(status.firstDiagnostic() == "winning VFS model could not be published",
            "cleanup cannot hide first publication failure"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C render route status: PASS\n";
    return EXIT_SUCCESS;
}
