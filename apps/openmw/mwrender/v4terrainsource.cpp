#include "v4terrainsource.hpp"

#include "renderingmanager.hpp"
#include "terrainstorage.hpp"

#include "../mwworld/cell.hpp"

#include <osg/Array>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>

namespace MWRender
{
    std::string makeV4TerrainChunkIdentity(const MWWorld::Cell& cell)
    {
        return "terrain:" + cell.getWorldSpace().serializeText() + ":" + std::to_string(cell.getGridX()) + ","
            + std::to_string(cell.getGridY()) + ":lod0";
    }

    std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        const RenderingManager& rendering, const MWWorld::Cell& cell)
    {
        TerrainStorage* const storage = rendering.getTerrainStorage();
        if (!storage || !cell.isExterior())
            return std::nullopt;

        const ESM::RefId worldspace = cell.getWorldSpace();
        const float cellWorldSize = storage->getCellWorldSize(worldspace);
        if (!std::isfinite(cellWorldSize) || cellWorldSize <= 0.0f)
            return std::nullopt;

        osg::ref_ptr<osg::Vec3Array> positions(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec4ubArray> colours(new osg::Vec4ubArray);
        const osg::Vec2f center(static_cast<float>(cell.getGridX()) + 0.5f, static_cast<float>(cell.getGridY()) + 0.5f);
        storage->fillVertexBuffers(0, 1.0f, center, worldspace, *positions, *normals, *colours);
        if (positions->empty() || positions->size() != normals->size() || positions->size() != colours->size())
            return std::nullopt;
        const std::size_t side = static_cast<std::size_t>(std::sqrt(static_cast<double>(positions->size())));
        if (side < 2 || side * side != positions->size())
            return std::nullopt;

        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions.reserve(positions->size());
        mesh->normals.reserve(normals->size());
        mesh->colors.reserve(colours->size());
        RenderCore::AxisAlignedBounds bounds;
        bounds.minimum = glm::vec3(std::numeric_limits<float>::max());
        bounds.maximum = glm::vec3(std::numeric_limits<float>::lowest());
        constexpr float byteToFloat = 1.0f / 255.0f;
        for (std::size_t i = 0; i < positions->size(); ++i)
        {
            const osg::Vec3f& position = (*positions)[i];
            const osg::Vec3f& normal = (*normals)[i];
            const osg::Vec4ub& colour = (*colours)[i];
            const glm::vec3 p(position.x(), position.y(), position.z());
            mesh->positions.push_back(p);
            mesh->normals.emplace_back(normal.x(), normal.y(), normal.z());
            mesh->colors.emplace_back(
                colour.r() * byteToFloat, colour.g() * byteToFloat, colour.b() * byteToFloat, colour.a() * byteToFloat);
            bounds.minimum = glm::min(bounds.minimum, p);
            bounds.maximum = glm::max(bounds.maximum, p);
        }

        mesh->indices.reserve((side - 1) * (side - 1) * 6);
        for (std::size_t column = 0; column + 1 < side; ++column)
        {
            for (std::size_t row = 0; row + 1 < side; ++row)
            {
                const auto index
                    = [side](std::size_t x, std::size_t y) { return static_cast<std::uint32_t>(x * side + y); };
                if ((column + row) % 2 == 1)
                {
                    mesh->indices.push_back(index(column + 1, row));
                    mesh->indices.push_back(index(column + 1, row + 1));
                    mesh->indices.push_back(index(column, row + 1));
                    mesh->indices.push_back(index(column, row));
                    mesh->indices.push_back(index(column + 1, row));
                    mesh->indices.push_back(index(column, row + 1));
                }
                else
                {
                    mesh->indices.push_back(index(column, row));
                    mesh->indices.push_back(index(column + 1, row + 1));
                    mesh->indices.push_back(index(column, row + 1));
                    mesh->indices.push_back(index(column, row));
                    mesh->indices.push_back(index(column + 1, row));
                    mesh->indices.push_back(index(column + 1, row + 1));
                }
            }
        }
        mesh->surfaces.push_back(RenderCore::MeshSurface{
            .topology = RenderCore::PrimitiveTopology::Triangles,
            .firstIndex = 0,
            .indexCount = static_cast<std::uint32_t>(mesh->indices.size()),
            .materialSlot = 0,
        });

        RenderCore::TerrainChunkSource result;
        result.identity = makeV4TerrainChunkIdentity(cell);
        result.worldspaceIdentity = worldspace.serializeText();
        result.gridX = cell.getGridX();
        result.gridY = cell.getGridY();
        result.transform.translation = { center.x() * cellWorldSize, center.y() * cellWorldSize, 0.0 };
        result.localBounds = bounds;
        result.mesh = std::move(mesh);
        result.material.diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
        result.material.ambient = result.material.diffuse;
        result.material.vertexColorMode = RenderCore::VertexColorMode::AmbientDiffuse;
        result.material.cullMode = RenderCore::CullMode::Back;
        return result;
    }
}
