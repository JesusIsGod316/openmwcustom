#include "v4terrainsource.hpp"

#include "renderingmanager.hpp"
#include "terrainstorage.hpp"

#include "../mwworld/cell.hpp"

#include <osg/Array>
#include <osg/PrimitiveSet>
#include <osg/Image>
#include <osg/Matrixf>

#include <components/terrain/buffercache.hpp>
#include <components/nifrender/vfsidentity.hpp>
#include <components/esm/util.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <sstream>
#include <stdexcept>

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
        result.mesh = mesh;
        result.material.diffuse = { 1.0f, 1.0f, 1.0f, 1.0f };
        result.material.ambient = result.material.diffuse;
        result.material.vertexColorMode = RenderCore::VertexColorMode::AmbientDiffuse;
        result.material.cullMode = RenderCore::CullMode::Back;
        if (std::getenv("OPENMW_V4_GEOMETRY_ONLY_TERRAIN_CONTROL"))
            return result;

        std::vector<Terrain::LayerInfo> layers;
        std::vector<osg::ref_ptr<osg::Image>> blendmaps;
        storage.getBlendmaps(1.f, center, blendmaps, layers, worldspace);
        const int tiles = storage.getTextureTileCount(1.f, worldspace);
        if (tiles <= 0 || layers.empty() || (!blendmaps.empty() && blendmaps.size() != layers.size()))
            throw std::runtime_error("Invalid LAND layer/blendmap set for " + request.identity);

        // Same UV buffer and blendmap texmat as Terrain::ChunkManager/createPasses.
        // Bake these immutable coordinates, not a per-frame texture-transform effect.
        auto coordinates = indexCache.getUVBuffer(static_cast<unsigned int>(side));
        osg::Matrixf blendMatrix;
        if (!ESM::isEsm4Ext(worldspace))
        {
            const float count = static_cast<float>(tiles);
            const float scale = count / (count + 1.f);
            blendMatrix.preMultTranslate(osg::Vec3f(.5f, .5f, 0.f));
            blendMatrix.preMultScale(osg::Vec3f(scale, scale, 1.f));
            blendMatrix.preMultTranslate(osg::Vec3f(-.5f, -.5f, 0.f));
            blendMatrix.preMultTranslate(osg::Vec3f(1.f / count / 4.f, -1.f / count / 4.f, 0.f));
        }
        mesh->texCoordSets.resize(2);
        for (const auto& coordinate : *coordinates)
        {
            mesh->texCoordSets[0].emplace_back(coordinate.x() * tiles, coordinate.y() * tiles);
            const auto blendUv = osg::Vec3f(coordinate.x(), coordinate.y(), 0.f) * blendMatrix;
            mesh->texCoordSets[1].emplace_back(blendUv.x(), blendUv.y());
        }

        const auto externalTexture = [&](VFS::Path::NormalizedView path, RenderCore::TextureRole role) {
            RenderCore::TerrainTextureSource source;
            const auto identity = storage.resolveV4TextureIdentity(path);
            source.texture.sourceIdentity = std::string(identity.canonicalPath.value());
            source.texture.contentIdentity = identity.valid() ? identity.contentIdentity
                : "missing-land-texture:" + source.texture.sourceIdentity;
            source.binding.role = role;
            source.binding.colorSpace = role == RenderCore::TextureRole::Normal
                ? RenderCore::TextureColorSpace::Linear : RenderCore::TextureColorSpace::Srgb;
            source.binding.formatClass = role == RenderCore::TextureRole::Normal
                ? RenderCore::TextureFormatClass::Normal : RenderCore::TextureFormatClass::Color;
            return source;
        };
        for (std::size_t i = 0; i < layers.size(); ++i)
        {
            const auto& layer = layers[i];
            RenderCore::TerrainLayerSource source;
            source.material = result.material;
            source.material.terrainLayer = RenderCore::TerrainLayerSemantic{i == 0, layer.mSpecular, layer.mParallax};
            source.textures.push_back(externalTexture(layer.mDiffuseMap, RenderCore::TextureRole::Diffuse));
            if (!layer.mNormalMap.empty())
                source.textures.push_back(externalTexture(layer.mNormalMap, RenderCore::TextureRole::Normal));
            if (!blendmaps.empty())
            {
                const auto& image = blendmaps[i];
                if (!image || image->s() <= 0 || image->t() <= 0 || image->getPixelFormat() != GL_ALPHA
                    || image->getDataType() != GL_UNSIGNED_BYTE)
                    throw std::runtime_error("Invalid LAND blend mask for " + request.identity);
                RenderCore::TerrainTextureSource blend;
                blend.texture.sourceIdentity = request.identity + ":blend:" + std::to_string(i);
                blend.texture.width = static_cast<std::uint32_t>(image->s());
                blend.texture.height = static_cast<std::uint32_t>(image->t());
                blend.texture.mipmapped = false;
                auto pixels = std::make_shared<RenderCore::TexturePixels>();
                pixels->rgba8.reserve(std::size_t(image->s()) * image->t() * 4);
                for (int y = 0; y < image->t(); ++y)
                    for (int x = 0; x < image->s(); ++x)
                        pixels->rgba8.insert(pixels->rgba8.end(), {255,255,255,*image->data(x,y)});
                std::istringstream bytes(std::string(reinterpret_cast<const char*>(pixels->rgba8.data()),
                    pixels->rgba8.size()), std::ios::in | std::ios::binary);
                blend.texture.contentIdentity = "land-rgba8:" + std::to_string(image->s()) + "x"
                    + std::to_string(image->t()) + ":" + NifRender::encodeOpenMwContentHash(
                        Files::getHash(blend.texture.sourceIdentity, bytes));
                blend.texture.pixels = std::move(pixels);
                blend.binding.role = RenderCore::TextureRole::Blend;
                blend.binding.colorSpace = RenderCore::TextureColorSpace::Linear;
                blend.binding.formatClass = RenderCore::TextureFormatClass::Scalar;
                blend.binding.transform.uvSet = 1;
                blend.binding.sampler.wrapU = blend.binding.sampler.wrapV = RenderCore::TextureWrap::Clamp;
                blend.binding.sampler.mipmapMode = RenderCore::TextureMipmapMode::None;
                source.textures.push_back(std::move(blend));
                source.material.alphaBlendEnabled = true;
                source.material.alphaMode = RenderCore::AlphaMode::Blend;
                source.material.sourceBlend = RenderCore::BlendFactor::SourceAlpha;
                source.material.destinationBlend = i == 0 ? RenderCore::BlendFactor::Zero : RenderCore::BlendFactor::One;
                source.material.transparentSort = RenderCore::TransparentSortPolicy::Unsorted;
            }
            result.layers.push_back(std::move(source));
        }
        return result;
    }
}
