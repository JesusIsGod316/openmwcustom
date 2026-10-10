#include <components/nifrender/geometrysanitization.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    using namespace RenderCore;
    unsigned checks = 0;
    void check(bool result, const char* message)
    {
        ++checks;
        if (!result)
            throw std::runtime_error(message);
    }
    MeshPayload triangle()
    {
        MeshPayload p;
        p.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } };
        p.normals = { { 0, 0, 2 }, { 0, 0, 1 }, { 0, 0, 1 } };
        p.indices = { 0, 1, 2 };
        p.surfaces = { { PrimitiveTopology::Triangles, 0, 3 } };
        return p;
    }
}

int main()
{
    try
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        auto p = triangle();
        check(NifRender::repairNonFiniteNormals(p) == 0 && p.normals[0].z == 2, "valid normals changed");
        p.normals[1] = glm::vec3(nan);
        p.normals[2].y = inf;
        const auto positions = p.positions;
        const auto indices = p.indices;
        check(!validMeshPayload(p), "NaN normals unexpectedly accepted by strict validator");
        check(NifRender::repairNonFiniteNormals(p) == 2 && validMeshPayload(p), "repair failed");
        check(p.normals[1] == glm::vec3(0, 0, 1) && p.normals[2] == glm::vec3(0, 0, 1), "wrong face normals");
        check(p.normals[0].z == 2 && p.positions == positions && p.indices == indices, "geometry changed");
        check(NifRender::repairNonFiniteNormals(p) == 0, "repair not idempotent");

        p = triangle();
        p.indices = { 2, 1, 0 };
        p.normals[0].x = nan;
        check(NifRender::repairNonFiniteNormals(p) == 1 && p.normals[0].z == -1, "triangle winding lost");

        p.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 1, 1, 0 } };
        p.normals.assign(4, glm::vec3(nan));
        p.indices = { 0, 1, 2, 3 };
        p.surfaces = { { PrimitiveTopology::TriangleStrip, 0, 4 } };
        check(NifRender::repairNonFiniteNormals(p) == 4 && validMeshPayload(p), "strip repair failed");
        check(p.normals[2].z == 1 && p.normals[3].z == 1, "odd strip winding lost");

        p = triangle();
        p.positions[2] = p.positions[0];
        p.normals[0] = glm::vec3(nan);
        check(NifRender::repairNonFiniteNormals(p) == 1 && validMeshPayload(p), "degenerate repair failed");
        check(p.normals[0] == glm::vec3(0, 0, 1), "degenerate fallback not finite unit normal");

        p = triangle();
        p.normals[0] = glm::vec3(nan);
        p.positions[1].x = inf;
        check(NifRender::repairNonFiniteNormals(p) == 1 && !validMeshPayload(p), "invalid positions hidden");
        p = triangle();
        p.normals[0] = glm::vec3(nan);
        p.indices[2] = 300;
        check(NifRender::repairNonFiniteNormals(p) == 1 && !validMeshPayload(p), "invalid indices hidden");
        p = triangle();
        p.normals[0] = glm::vec3(nan);
        p.surfaces[0].indexCount = 300;
        check(NifRender::repairNonFiniteNormals(p) == 1 && !validMeshPayload(p), "invalid surface hidden");
        p = triangle();
        p.normals.resize(1);
        p.normals[0] = glm::vec3(nan);
        check(NifRender::repairNonFiniteNormals(p) == 0 && !validMeshPayload(p), "attribute mismatch hidden");
        p = triangle();
        p.normals.clear();
        check(NifRender::repairNonFiniteNormals(p) == 0 && validMeshPayload(p), "absent normals changed");

        p = triangle();
        p.positions[1].x = std::numeric_limits<float>::max();
        p.positions[2].y = std::numeric_limits<float>::max();
        p.normals[0] = glm::vec3(nan);
        check(NifRender::repairNonFiniteNormals(p) == 1 && p.normals[0].z == 1 && validMeshPayload(p),
            "large finite geometry overflowed normal reconstruction");
        std::cout << "Geometry sanitization: " << checks << " checks passed\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
