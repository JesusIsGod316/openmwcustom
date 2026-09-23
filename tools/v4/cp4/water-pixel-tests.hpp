#pragma once

#include <components/render/backend/vsg/framecamera.hpp>
#include <components/render/backend/vsg/watersurface.hpp>
#include <components/render/backend/vsg/skybackdrop.hpp>
#include <components/render/backend/vsg/uipipeline.hpp>
#include <components/render/backend/vsg/staticassetconformance.hpp>
#include <components/render/backend/vsg/openmwviewdependentstate.hpp>
#include <vsg/all.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cstring>
#include "terrain-pixel-tests.hpp"
#include <components/render/backend/vsg/statictexturedecode.hpp>
#include <fstream>

// Offscreen pixel regression using the actual production water/sky pipelines.
// No gameplay settings, saves, or screenshot automation are involved.
inline void checkWaterPixels(vsg::Device* device)
{
    using namespace RenderCore;
    auto target = RenderVsg::createOffscreenRenderTarget(device, {128, 128},
        RenderTargetFormat::Rgba8Srgb, RenderTargetFormat::Depth32Float);
    require(static_cast<bool>(target), "water pixel target");
    auto image = [](vsg::ubvec4 top, vsg::ubvec4 bottom) {
        auto data = vsg::ubvec4Array2D::create(2, 2, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        (*data)(0, 0) = (*data)(1, 0) = top;
        (*data)(0, 1) = (*data)(1, 1) = bottom;
        return vsg::ImageView::create(vsg::Image::create(data));
    };
    // Reflection changes from green at the top to blue at the bottom.
    // At the bottom pixel, correct screen-coordinate sampling must select blue.
    auto water = RenderVsg::WaterSurface::create(
        image({0,255,0,255}, {0,0,255,255}), image({255,0,0,255}, {255,0,0,255}));
    auto sky = RenderVsg::SkyBackdrop::create();
    auto root = vsg::Group::create();
    root->addChild(sky.node()); root->addChild(water.node());
    FrameView frame;
    frame.extent = {128,128};
    frame.current.worldPosition = {0,0,2};
    frame.current.projection.farPlane = 1000;
    frame.current.projection.matrix = glm::perspectiveRH_ZO(glm::radians(60.0f), 1.0f, 1000.0f, 0.1f);
    frame.current.projection.matrix[1][1] *= -1;
    frame.current.view = glm::lookAtRH(glm::vec3(0,0,2), glm::vec3(0,1,2), glm::vec3(0,0,1));
    auto camera = RenderVsg::FrameCameraObjects::create(frame);
    auto view = vsg::View::create(camera.camera, root);
    view->viewDependentState = RenderVsg::OpenMwViewDependentState::create(view.get());
    target.renderGraph->addChild(view);
    const auto family = device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    auto commandGraph = vsg::CommandGraph::create(vsg::ref_ptr<vsg::Device>(device), family);
    commandGraph->addChild(target.renderGraph);
    auto viewer = vsg::Viewer::create();
    viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});
    auto firstCompile = viewer->compile();
    if (!firstCompile) throw std::runtime_error("water pixel compilation: " + firstCompile.message + " result=" + std::to_string(firstCompile.result));

    FrameEnvironmentState environment;
    environment.waterEnabled = true; environment.waterHeight = 0;
    environment.skyEnabled = true; environment.interior = false;
    environment.sunVisible = false;
    environment.fogColor = environment.skyColor = {0,0,0,1};
    auto buffer = vsg::createBufferAndMemory(device, 128*128*4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    auto pool = vsg::CommandPool::create(device, family);
    auto readback = [&] {
        // submitCommandsToQueue does not reset a signalled fence. Each readback
        // owns a fresh fence so the wait cannot finish before this copy does.
        auto fence = vsg::Fence::create(device);
        auto commands = vsg::Commands::create();
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        commands->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, vsg::ImageMemoryBarrier::create(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                target.color->image, range)));
        auto copy = vsg::CopyImageToBuffer::create();
        copy->srcImage = target.color->image; copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy->dstBuffer = buffer;
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        region.imageExtent = {128,128,1};
        copy->regions.push_back(region); commands->addChild(copy);
        require(vsg::submitCommandsToQueue(pool, fence, 10000000000ull, device->getQueue(family),
            [&](vsg::CommandBuffer& commandBuffer) { commands->record(commandBuffer); }) == VK_SUCCESS,
            "water pixel readback submission");
        auto mapped = vsg::MappedData<vsg::ubyteArray>::create(buffer->getDeviceMemory(device->deviceID),
            buffer->getMemoryOffset(device->deviceID), 0, vsg::Data::Properties{}, 128*128*4);
        std::vector<unsigned char> pixels(128*128*4);
        std::memcpy(pixels.data(), mapped->dataPointer(), pixels.size());
        return pixels;
    };
    auto render = [&](double time) {
        water.update(environment, frame, time); sky.update(environment, frame);
        require(viewer->advanceToNextFrame(time), "water pixel frame advance");
        viewer->update();
        for (const auto& task : viewer->recordAndSubmitTasks)
        {
            for (auto& graph : task->commandGraphs) graph->reset();
            require(task->submit(vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp())) == VK_SUCCESS,
                "water pixel frame submit");
        }
        viewer->deviceWaitIdle();
        return readback();
    };
    auto pixel = [](const auto& pixels, unsigned x, unsigned y) {
        const auto i = (y*128+x)*4;
        return vsg::ubvec4(pixels[i], pixels[i+1], pixels[i+2], pixels[i+3]);
    };
    auto wet = render(0);
    auto upper = pixel(wet,64,16), lower = pixel(wet,64,112);
    std::cout << "water pixels above=" << unsigned(upper.r) << ',' << unsigned(upper.g) << ',' << unsigned(upper.b)
              << " below=" << unsigned(lower.r) << ',' << unsigned(lower.g) << ',' << unsigned(lower.b) << '\n';
    require(upper.r < 4 && upper.g < 4 && upper.b < 4, "water covered the sky above the horizon");
    require(lower.r > 80 && lower.b > 80 && lower.g < 10, "water did not sample reflection/refraction at screen UV");
    environment.waterEnabled = false;
    auto dry = render(1);
    lower = pixel(dry,64,112);
    require(lower.r < 4 && lower.g < 4 && lower.b < 4, "disabled water still draws");
    environment.skyColor = environment.fogColor = {0,1,0,1};
    auto green = render(2);
    upper = pixel(green,64,16);
    std::cout << "updated sky=" << unsigned(upper.r) << ',' << unsigned(upper.g) << ',' << unsigned(upper.b) << '\n';
    require(upper.g > 240 && upper.r < 4 && upper.b < 4, "sky uniform update did not reach GPU");
    environment.skyColor = environment.fogColor = {0,0,0,1};
    environment.waterEnabled = true; environment.underwater = true;
    auto submerged = render(3);
    lower = pixel(submerged,64,112);
    require(lower.r < pixel(wet,64,112).r - 15, "water uniform update did not reach GPU");
    environment.underwater = false;
    environment.interior = true; environment.skyEnabled = false;
    auto cave = render(4);
    require(pixel(cave,64,112).r > 80 && pixel(cave,64,16).r < 4, "cave water coverage");
    environment.waterHeight = 4;
    auto raised = render(5);
    require(pixel(raised,64,16).r > 80 && pixel(raised,64,112).r < 4,
        "water height did not move the plane above the camera");
    environment.waterHeight = 0;
    // Reversed depth at its nearest value occludes the entire water plane.
    target.setClearValues({{0,0,0,1}}, {1.0f,0});
    auto occluded = render(6);
    lower = pixel(occluded,64,112);
    require(lower.r < 4 && lower.g < 4 && lower.b < 4, "water ignored scene depth");
    std::cout << "PASS water/sky pixels: horizon, screen UV, reflection/refraction, dynamic uniforms, depth\n";

    // Asymmetric left/right reflection catches the mirrored camera handedness
    // mismatch that the old vertical-only fixture could not detect.
    auto horizontal = image({0,0,255,255}, {0,0,255,255});
    auto horizontalData = horizontal->image->data.cast<vsg::ubvec4Array2D>();
    (*horizontalData)(0,0) = (*horizontalData)(0,1) = vsg::ubvec4(0,255,0,255);
    water = RenderVsg::WaterSurface::create(horizontal, image({255,0,0,255}, {255,0,0,255}));
    root->children.clear(); root->addChild(sky.node()); root->addChild(water.node());
    require(static_cast<bool>(RenderVsg::compileForViewer(*viewer, root)), "asymmetric water compilation");
    target.setClearValues({{0,0,0,1}}, {0.0f,0});
    auto asymmetric = render(0);
    const auto left = pixel(asymmetric,8,112), right = pixel(asymmetric,120,112);
    require(left.b > left.g + 60 && right.g > right.b + 60, "water reflection was not horizontally corrected");
    target.setClearValues({{0,1,0,1}}, {0.0f,0});
    auto greenBackground = render(0);
    require(pixel(asymmetric,8,112) == pixel(greenBackground,8,112),
        "refraction composition counted main background twice");
    target.setClearValues({{0,0,0,1}}, {0.0f,0});
    std::cout << "PASS water pixels: reflected left/right alignment, opaque refraction composition\n";

    // Exercise the normal-map descriptor and view-dependent Fresnel on the
    // production shader, rather than only the procedural fallback branch.
    auto flatNormal = vsg::vec4Array2D::create(1, 1, vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
    (*flatNormal)(0,0) = vsg::vec4(0.5f,0.5f,1.0f,1.0f);
    water = RenderVsg::WaterSurface::create(image({0,0,255,255},{0,0,255,255}),
        image({255,0,0,255},{255,0,0,255}), flatNormal);
    root->children.clear(); root->addChild(water.node());
    require(static_cast<bool>(RenderVsg::compileForViewer(*viewer, root)), "normal-map water compilation");
    auto native = render(0);
    const auto nearWater = pixel(native,64,120), grazingWater = pixel(native,64,72);
    require(nearWater.r > grazingWater.r + 20 && grazingWater.b > nearWater.b + 20,
        "normal-map water lacks view-dependent Fresnel");
    auto tiltedNormal = vsg::vec4Array2D::create(1,1,vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
    (*tiltedNormal)(0,0) = vsg::vec4(0.5f,0.9f,0.8f,1.0f);
    water = RenderVsg::WaterSurface::create(image({0,0,255,255},{0,0,255,255}),
        image({255,0,0,255},{255,0,0,255}), tiltedNormal);
    root->children.clear(); root->addChild(water.node());
    require(static_cast<bool>(RenderVsg::compileForViewer(*viewer, root)), "tilted normal-map compilation");
    const auto tilted = pixel(render(0),64,120);
    require(std::abs(int(tilted.b) - int(nearWater.b)) > 20, "water ignored the supplied normal map");
    std::cout << "PASS water pixels: normal-map descriptor and view-dependent Fresnel\n";

    if (const char* normalPath = std::getenv("OPENMW_V4_TEST_WATER_NORMAL"))
    {
        TextureRecord nativeTexture;
        nativeTexture.sourceIdentity = "textures/omw/water_nm.png";
        nativeTexture.contentIdentity = "water-pixel-fixture";
        TextureRealizationKey key{{TextureHandle::fromParts(0,1), TextureColorSpace::Data,
            TextureFormatClass::Normal}, nativeTexture.revision};
        auto report = std::make_shared<RenderVsg::StaticTextureDecodeReport>();
        RenderVsg::StaticTextureDecoder decoder({}, report);
        auto nativeMap = decoder.decode(nativeTexture, key, [normalPath](std::string_view) -> Files::IStreamPtr {
            return std::make_unique<std::ifstream>(normalPath, std::ios::binary);
        });
        require(nativeMap && report->warningFallbacks == 0 && nativeMap->width() > 1,
            "native water map failed production decoding");
        water = RenderVsg::WaterSurface::create(horizontal, image({255,0,0,255},{255,0,0,255}), nativeMap);
        root->children.clear(); root->addChild(water.node());
        require(static_cast<bool>(RenderVsg::compileForViewer(*viewer, root)), "native water texture compilation");
        const auto firstWave = render(0);
        const auto secondWave = render(5);
        unsigned changed = 0;
        for (unsigned y = 80; y < 125; ++y)
            for (unsigned x = 8; x < 120; ++x)
                changed += pixel(firstWave,x,y) != pixel(secondWave,x,y);
        require(changed > 100, "native water wave detail did not animate");
        std::cout << "PASS native water asset: decoded " << nativeMap->width() << 'x' << nativeMap->height()
                  << ", animated pixels=" << changed << '\n';
    }

    // Same depth-disabled UI shader for both layers: prove the production GUI
    // routing wins over world geometry deferred into a later traversal bin.
    auto worldUi = RenderVsg::createUiPipeline(128,128);
    auto overlayUi = RenderVsg::createUiPipeline(128,128);
    auto greenTexture = vsg::ubvec4Array2D::create(1,1, vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
    (*greenTexture)(0,0) = vsg::ubvec4(0,255,0,255);
    worldUi.whiteTexture = greenTexture;
    root->children.clear();
    root->addChild(vsg::Layer::create(RenderVsg::StaticBackToFrontBinNumber, 0.0,
        RenderVsg::buildUiTestOverlay(worldUi)));
    root->addChild(RenderVsg::createUiOverlayLayer(RenderVsg::buildUiTestOverlay(overlayUi)));
    view->bins = RenderVsg::createStaticConformanceBins();
    view->bins.push_back(vsg::Bin::create(RenderVsg::UiOverlayBinNumber, vsg::Bin::NO_SORT));
    require(static_cast<bool>(RenderVsg::compileForViewer(*viewer, root)), "GUI ordering pixel compilation");
    auto gui = render(7);
    const auto button = pixel(gui,12,118);
    require(button.r > 200 && button.r > button.g + 50,
        "deferred world geometry painted over GUI");
    std::cout << "PASS GUI pixels: overlay recorded after deferred world bins\n";
    target.setClearValues({{0,0,0,1}}, {0.0f,0});
    checkTerrainPixels(root, view, viewer, render, pixel);
}
