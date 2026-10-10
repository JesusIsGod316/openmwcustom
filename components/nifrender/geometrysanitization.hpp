#ifndef OPENMW_COMPONENTS_NIFRENDER_GEOMETRYSANITIZATION_H
#define OPENMW_COMPONENTS_NIFRENDER_GEOMETRYSANITIZATION_H

#include <components/rendercore/records.hpp>

#include <cmath>
#include <cstddef>
#include <vector>

namespace NifRender
{
    // Some otherwise usable NIFs contain NaN/Inf normals (e.g. normals exported
    // for degenerate faces). NifOsg accepts them, but the neutral/GPU contract
    // correctly requires finite attributes. Repair only these normals at import;
    // never relax payload validation or alter source positions/indices/UVs.
    [[nodiscard]] inline std::size_t repairNonFiniteNormals(RenderCore::MeshPayload& payload)
    {
        if (payload.normals.size() != payload.positions.size())
            return 0;
        std::size_t repaired = 0;
        for (const glm::vec3& normal : payload.normals)
            repaired += !RenderCore::semantic_detail::finite(normal);
        if (repaired == 0)
            return 0;

        std::vector<glm::dvec3> accumulated(payload.positions.size(), glm::dvec3(0.0));
        const auto addTriangle = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            if (a >= payload.positions.size() || b >= payload.positions.size() || c >= payload.positions.size())
                return;
            const glm::dvec3 face = glm::cross(glm::dvec3(payload.positions[b]) - glm::dvec3(payload.positions[a]),
                glm::dvec3(payload.positions[c]) - glm::dvec3(payload.positions[a]));
            if (!std::isfinite(face.x) || !std::isfinite(face.y) || !std::isfinite(face.z))
                return;
            for (const std::uint32_t vertex : { a, b, c })
                if (!RenderCore::semantic_detail::finite(payload.normals[vertex]))
                    accumulated[vertex] += face;
        };
        for (const RenderCore::MeshSurface& surface : payload.surfaces)
        {
            if (surface.firstIndex > payload.indices.size()
                || surface.indexCount > payload.indices.size() - surface.firstIndex)
                continue; // Validation still rejects malformed primitive ranges.
            const std::size_t begin = surface.firstIndex;
            const std::size_t count = surface.indexCount;
            if (surface.topology == RenderCore::PrimitiveTopology::Triangles)
            {
                for (std::size_t i = 0; i + 2 < count; i += 3)
                    addTriangle(payload.indices[begin + i], payload.indices[begin + i + 1],
                        payload.indices[begin + i + 2]);
            }
            else if (surface.topology == RenderCore::PrimitiveTopology::TriangleStrip)
            {
                for (std::size_t i = 0; i + 2 < count; ++i)
                    addTriangle(payload.indices[begin + i + (i % 2)],
                        payload.indices[begin + i + 1 - (i % 2)], payload.indices[begin + i + 2]);
            }
        }
        for (std::size_t i = 0; i < payload.normals.size(); ++i)
        {
            if (RenderCore::semantic_detail::finite(payload.normals[i]))
                continue;
            const double length = glm::length(accumulated[i]);
            // Isolated/degenerate/line vertices have no usable face normal.
            // Use the renderer's ordinary missing-normal direction in that case.
            payload.normals[i] = std::isfinite(length) && length > 0.0
                ? glm::vec3(accumulated[i] / length)
                : glm::vec3(0.0f, 0.0f, 1.0f);
        }
        return repaired;
    }
}

#endif
