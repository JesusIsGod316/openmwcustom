#include "v4terrainsource.hpp"

#include "renderingmanager.hpp"
#include "terrainstorage.hpp"

#include "../mwworld/cell.hpp"

#include <osg/Array>
#include <osg/PrimitiveSet>

#include <components/terrain/buffercache.hpp>

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
        return makeV4TerrainChunkRequest(cell, cell.getGridX(), cell.getGridY(), true).identity;
    }

    RenderCore::TerrainPreparationRequest makeV4TerrainChunkRequest(const MWWorld::Cell& cell, std::int32_t gridX,
        std::int32_t gridY, bool required, std::uint32_t lodLevel, std::uint8_t stitchMask)
    {
        RenderCore::TerrainPreparationRequest result;
        result.worldspaceIdentity = cell.getWorldSpace().serializeText();
        result.gridX = gridX;
        result.gridY = gridY;
        result.identity = "terrain:" + result.worldspaceIdentity + ":" + std::to_string(gridX) + ","
            + std::to_string(gridY) + ":lod" + std::to_string(lodLevel) + ":stitch" + std::to_string(stitchMask);
        result.lodLevel = lodLevel;
        result.stitchMask = stitchMask;
        result.required = required;
        return result;
    }

    std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        const RenderingManager& rendering, const MWWorld::Cell& cell)
    {
        TerrainStorage* const storage = rendering.getTerrainStorage();
        if (!storage || !cell.isExterior())
            return std::nullopt;
        return makeV4TerrainChunkSource(
            *storage, makeV4TerrainChunkRequest(cell, cell.getGridX(), cell.getGridY(), true));
    }

    std::optional<RenderCore::TerrainChunkSource> makeV4TerrainChunkSource(
        TerrainStorage& storage, const RenderCore::TerrainPreparationRequest& request)
    {
        const ESM::RefId worldspace = ESM::RefId::deserializeText(request.worldspaceIdentity);
        const float cellWorldSize = storage.getCellWorldSize(worldspace);
        if (!std::isfinite(cellWorldSize) || cellWorldSize <= 0.0f)
            return std::nullopt;

        osg::ref_ptr<osg::Vec3Array> positions(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec3Array> normals(new osg::Vec3Array);
        osg::ref_ptr<osg::Vec4ubArray> colours(new osg::Vec4ubArray);
        const osg::Vec2f center(static_cast<float>(request.gridX) + 0.5f, static_cast<float>(request.gridY) + 0.5f);
        storage.fillVertexBuffers(
            static_cast<int>(request.lodLevel), 1.0f, center, worldspace, *positions, *normals, *colours);
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

        unsigned int lodFlags = 0;
        for (unsigned int edge = 0; edge < 4; ++edge)
            if ((request.stitchMask & (1u << edge)) != 0)
                lodFlags |= 1u << (4 * edge);
        static Terrain::BufferCache indexCache;
        const osg::ref_ptr<osg::DrawElements> indices
            = indexCache.getIndexBuffer(static_cast<unsigned int>(side), lodFlags);
        if (!indices || indices->getNumIndices() == 0)
            return std::nullopt;
        mesh->indices.reserve(indices->getNumIndices());
        for (unsigned int i = 0; i < indices->getNumIndices(); ++i)
            mesh->indices.push_back(indices->index(i));
        mesh->surfaces.push_back(RenderCore::MeshSurface{
            .topology = RenderCore::PrimitiveTopology::Triangles,
            .firstIndex = 0,
            .indexCount = static_cast<std::uint32_t>(mesh->indices.size()),
            .materialSlot = 0,
        });

        RenderCore::TerrainChunkSource result;
        result.identity = request.identity;
        result.worldspaceIdentity = request.worldspaceIdentity;
        result.gridX = request.gridX;
        result.gridY = request.gridY;
        result.lodLevel = request.lodLevel;
        result.stitchMask = request.stitchMask;
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
