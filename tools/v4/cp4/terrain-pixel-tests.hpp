#pragma once

#include <components/rendercore/terrainchunkproducer.hpp>
#include <components/render/backend/vsg/statictexturedecode.hpp>
#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <cstdlib>

// Exercise the neutral producer, real decoder, planner and production material
// pipeline together, rather than substituting a test-only terrain shader.
template<class Render, class Pixel>
void checkTerrainPixels(vsg::ref_ptr<vsg::Group> root, vsg::ref_ptr<vsg::View> view,
    vsg::ref_ptr<vsg::Viewer> viewer, Render render, Pixel pixel)
{
    using namespace RenderCore;
    auto mesh = std::make_shared<MeshPayload>();
    mesh->positions = {{-1,-1,-5},{1,-1,-5},{1,1,-5},{-1,1,-5}};
    mesh->normals.resize(4,{0,0,1});
    mesh->colors.resize(4,{1,1,1,1});
    mesh->texCoordSets = {{{0,0},{8,0},{8,8},{0,8}},{{0,0},{1,0},{1,1},{0,1}}};
    mesh->indices = {0,1,2,0,2,3};
    mesh->surfaces = {{PrimitiveTopology::Triangles,0,6,0}};
    TerrainChunkSource source;
    source.identity = "terrain:pixel-regression";
    source.worldspaceIdentity = "test";
    source.mesh = mesh;
    source.localBounds = {{-1,-1,-5},{1,1,-5}};
    source.material.unlit = true;
    source.material.cullMode = CullMode::None;
    auto texture = [](std::string id, TextureRole role, std::vector<std::uint8_t> bytes) {
        TerrainTextureSource result;
        result.texture.sourceIdentity = result.texture.contentIdentity = std::move(id);
        result.texture.width = result.texture.height = 2;
        result.texture.mipmapped = false;
        auto pixels = std::make_shared<TexturePixels>();
        pixels->rgba8 = std::move(bytes);
        result.texture.pixels = std::move(pixels);
        result.binding.role = role;
        result.binding.colorSpace = role == TextureRole::Diffuse ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
        result.binding.transform.uvSet = role == TextureRole::Blend ? 1 : 0;
        result.binding.sampler.wrapU = result.binding.sampler.wrapV = TextureWrap::Clamp;
        result.binding.sampler.mipmapMode = TextureMipmapMode::None;
        return result;
    };
    for (unsigned i=0; i<2; ++i)
    {
        TerrainLayerSource layer;
        layer.material = source.material;
        layer.material.terrainLayer = TerrainLayerSemantic{i==0,true,true};
        layer.material.alphaBlendEnabled = true;
        layer.material.alphaMode = AlphaMode::Blend;
        layer.material.transparentSort = TransparentSortPolicy::Unsorted;
        layer.material.destinationBlend = i==0 ? BlendFactor::Zero : BlendFactor::One;
        // Diffuse alpha is zero: using it as opacity would make LAND disappear.
        std::vector<std::uint8_t> diffuse, mask, normal;
        for (unsigned j=0; j<4; ++j)
        {
            diffuse.insert(diffuse.end(), {std::uint8_t(i==0?255:0),std::uint8_t(i==1?255:0),0,0});
            // Asymmetric in V as well as U: row zero is sampled at v=0.
            const auto alpha = std::uint8_t((((j%2) ^ (1-j/2))==i)?255:0);
            mask.insert(mask.end(), {255,255,255,alpha});
            normal.insert(normal.end(), {128,128,255,128});
        }
        layer.textures.push_back(texture("land:color:"+std::to_string(i),TextureRole::Diffuse,std::move(diffuse)));
        layer.textures.push_back(texture("land:mask:"+std::to_string(i),TextureRole::Blend,std::move(mask)));
        layer.textures.push_back(texture("land:normal",TextureRole::Normal,std::move(normal)));
        source.layers.push_back(std::move(layer));
    }
    require(terrainChunkPayloadBytes(source).value() == terrainMeshPayloadBytes(*mesh).value()+96,
        "LAND preparation budget omitted procedural pixels");
    auto invalid = source;
    invalid.layers[1].material.terrainLayer->first = true;
    require(!validTerrainChunkSource(invalid), "LAND accepted a second base pass");
    if (std::getenv("OPENMW_V4_GEOMETRY_ONLY_TERRAIN_CONTROL")) source.layers.clear();

    TerrainChunkSource foreground = source;
    foreground.identity = "terrain:occluder";
    foreground.gridX = 1;
    foreground.layers.clear();
    auto frontMesh = std::make_shared<MeshPayload>(*mesh);
    for (auto& p : frontMesh->positions) { p.x *= .15f; p.y *= .15f; p.z = -4; }
    foreground.mesh = frontMesh;
    foreground.localBounds = {{-.15f,-.15f,-4},{.15f,.15f,-4}};
    foreground.material.diffuse = {0,0,1,1};
    RenderWorld world;
    RenderWorldPublisher publisher(world);
    TerrainChunkProducer producer(world,publisher);
    std::vector<TerrainChunkSource> sources{source,foreground};
    require(producer.synchronize(sources) == TerrainChunkPublishStatus::Applied, "LAND layer publication failed");
    require(producer.synchronize(sources) == TerrainChunkPublishStatus::AlreadyPresent, "LAND re-publication not stable");
    if (!source.layers.empty())
    {
        auto badSource = source;
        badSource.identity = "terrain:invalid-replacement";
        badSource.layers[1].textures[1].texture.pixels = std::make_shared<TexturePixels>();
        require(producer.synchronize(std::optional<TerrainChunkSource>(badSource))
                == TerrainChunkPublishStatus::PublishRejected,
            "LAND accepted a malformed generated mask");
        require(producer.synchronize(sources) == TerrainChunkPublishStatus::AlreadyPresent,
            "failed LAND replacement retired the previously published terrain");
    }
    auto badRecord = texture("bad-pixels",TextureRole::Blend,{}).texture;
    auto badHandle = world.reserveTexture();
    require(badHandle && !world.commit(*badHandle,badRecord), "invalid procedural image was published");
    world.cancel(*badHandle);

    root->children.clear();
    auto decoder = std::make_shared<RenderVsg::StaticTextureDecoder>();
    RenderVsg::StaticTextureResolver resolver = [decoder](const TextureRecord& record,const TextureRealizationKey& key) {
        return decoder->decode(record,key,{});
    };
    world.forEachModel([&](ModelHandle handle,const ModelRecord&) {
        auto plan = RenderVsg::buildStaticAssetPlan(world,handle);
        require(plan.has_value(), "LAND static plan failed");
        auto realized = RenderVsg::realizeStaticAssetConformant(world,handle,*plan,resolver);
        require(realized.valid() && realized.stats.unsupportedTextureBindings==0
            && realized.stats.runtimeContextEffects==0, "LAND production realization failed");
        root->addChild(realized.root);
    });
    FrameView frame;
    frame.extent = {128,128};
    frame.current.projection.matrix = glm::orthoRH_ZO(-1.f,1.f,-1.f,1.f,100.f,.1f);
    frame.current.projection.matrix[1][1] *= -1;
    frame.current.view = glm::mat4(1);
    auto camera = RenderVsg::FrameCameraObjects::create(frame);
    view->camera = camera.camera;
    view->bins = RenderVsg::createStaticConformanceBins();
    require(static_cast<bool>(RenderVsg::compileForViewer(*viewer,root)), "LAND pipeline compilation failed");
    auto pixels = render(8);
    auto left=pixel(pixels,16,16),right=pixel(pixels,112,16),middle=pixel(pixels,64,16),front=pixel(pixels,64,64);
    auto bottomLeft=pixel(pixels,16,112),bottomRight=pixel(pixels,112,112);
    std::cout << "LAND pixels left=" << unsigned(left.r) << ',' << unsigned(left.g)
        << " right=" << unsigned(right.r) << ',' << unsigned(right.g)
        << " blend=" << unsigned(middle.r) << ',' << unsigned(middle.g)
        << " foreground=" << unsigned(front.r) << ',' << unsigned(front.g) << ',' << unsigned(front.b) << '\n';
    require(left.r>240 && left.g<10 && right.g>240 && right.r<10, "LAND diffuse/blend mask coverage is wrong");
    require(bottomLeft.g>240 && bottomLeft.r<10 && bottomRight.r>240 && bottomRight.g<10,
        "LAND generated blend-mask row orientation is wrong");
    require(middle.r>170 && middle.r<205 && middle.g>170 && middle.g<205, "LAND weighted color is not linear-light");
    require(front.b>240 && front.r<10 && front.g<10, "LAND layer drew over foreground depth");
    require(producer.synchronize(std::span<const TerrainChunkSource>{}) == TerrainChunkPublishStatus::Applied,
        "LAND layer retirement failed");
    unsigned remaining=0;
    world.forEachTexture([&](TextureHandle,const TextureRecord&){++remaining;});
    world.forEachMaterial([&](MaterialHandle,const MaterialRecord&){++remaining;});
    require(remaining==0, "LAND texture/material handles leaked on retirement");
    std::cout << "PASS LAND production pixels: weighted layers, alpha semantics, normal/height variant, depth, retirement\n";
}
