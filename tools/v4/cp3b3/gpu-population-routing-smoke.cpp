// Exercise the production pre-seal seam; --gpu additionally executes the real
// P3A/P3B compute and draw commands on a headless Vulkan device. Never uses game
// files, saves, settings, or CPU visibility feedback in the production path.
#include <components/render/backend/vsg/gpupopulationcull.hpp>
#include <components/render/backend/vsg/staticassetconformance.hpp>
#include <components/render/backend/vsg/pipelineinventory.hpp>
#ifdef P3_GPU_PIXEL_TESTS
#include <components/render/backend/vsg/framecamera.hpp>
#include <components/render/backend/vsg/offscreenrendertarget.hpp>
#include <components/render/backend/vsg/openmwviewdependentstate.hpp>
#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#endif
#include <vsg/all.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    using namespace RenderCore;
    void require(bool value, const std::string& reason)
    { if (!value) throw std::runtime_error(reason); }
    void flag(const char* name, bool on)
    {
#ifdef _WIN32
        require(_putenv_s(name, on ? "1" : "") == 0, "set test environment");
#else
        require((on ? setenv(name, "1", 1) : unsetenv(name)) == 0, "set test environment");
#endif
    }
    // Deliberately the same Group-only topology that P3 is permitted to edit.
    // Inventory/ordered wrappers must NOT be opened by a mutation visitor.
    std::size_t editableDraws(const vsg::Group& root)
    {
        std::size_t count = 0;
        for (const auto& child : root.children)
        {
            if (dynamic_cast<const vsg::VertexIndexDraw*>(child.get())) ++count;
            else if (const auto* group = dynamic_cast<const vsg::Group*>(child.get())) count += editableDraws(*group);
        }
        return count;
    }
    struct Inventories : vsg::ConstVisitor
    {
        std::size_t count = 0;
        using vsg::ConstVisitor::apply;
        void apply(const vsg::Object& object) override
        {
            if (dynamic_cast<const RenderVsg::PipelineInventoryNode*>(&object)) ++count;
            object.traverse(*this);
        }
    };
    struct World
    {
        RenderWorld world;
        RenderVsg::StaticPopulationPlan plan;
        MaterialHandle material;
        World()
        {
            const auto mat = world.reserveMaterial(); require(mat.has_value(), "reserve material"); material = *mat;
            MaterialRecord record; record.sourceIdentity = "p3-test:opaque"; record.cullMode = CullMode::None;
            record.unlit = true; record.diffuse = {1,0,0,1}; record.emission = {1,0,0,1};
            require(world.commit(material, std::move(record)), "commit material");
            auto payload = std::make_shared<MeshPayload>();
            payload->positions = {{-.7f,-.7f,0},{.7f,-.7f,0},{.7f,.7f,0},{-.7f,.7f,0}};
            payload->normals.assign(4, {0,0,1}); payload->colors.assign(4, {1,1,1,1});
            payload->indices = {0,1,2,0,2,3};
            payload->surfaces = {{PrimitiveTopology::Triangles,0,6,0}};
            const auto mesh = world.reserveMesh(); require(mesh.has_value(), "reserve mesh");
            MeshRecord meshRecord; meshRecord.sourceIdentity = "p3-test:quad";
            meshRecord.surfaceCount = 1; meshRecord.payload = payload;
            require(world.commit(*mesh, std::move(meshRecord)), "commit mesh");
            auto modelPayload = std::make_shared<ModelPayload>();
            ModelNodeRecord geometry; geometry.kind = ModelNodeKind::Geometry; geometry.mesh = *mesh;
            geometry.materials = {material}; modelPayload->nodes = {geometry}; modelPayload->roots = {ModelNodeIndex{0}};
            const auto model = world.reserveModel(); require(model.has_value(), "reserve model");
            ModelRecord modelRecord; modelRecord.sourceIdentity = "p3-test:model";
            modelRecord.contentIdentity = "p3-test:model-v1"; modelRecord.payload = modelPayload;
            require(world.commit(*model, std::move(modelRecord)), "commit model");
            const auto asset = RenderVsg::buildStaticAssetPlan(world, *model);
            require(asset.has_value() && asset->draws.size() == 1, "plan one opaque draw");
            plan.model = *model; plan.asset = *asset;
            plan.coordinateOrigin = {0,0,0};
            for (double x : {0.,3.,20.})
            {
                PopulationInstanceRecord placement;
                placement.transform.translation = {x,0,-2};
                plan.placements.push_back(placement);
            }
        }
        RenderVsg::StaticRealizationResult build(bool resource, const RenderVsg::StaticGraphFinalizer& finalizer = {})
        {
            // Matches the host: conformance -> P3 finalizer -> pipeline seal ->
            // resource seal. Both production gates stay enabled during QA.
            auto result = RenderVsg::realizeStaticAssetConformant(world, plan.model, plan.asset, {}, {}, {},
                plan.placements, plan.coordinateOrigin, 1.0f, false, finalizer);
            require(result.valid(), "production conformant realization failed");
            if (resource) result.root = RenderVsg::sealPipelineInventory(result.root);
            return result;
        }
    };

    void checkSeam(bool pipeline, bool resource)
    {
        World input;
        auto control = input.build(resource);
        Inventories original; control.root->accept(original);
        require(original.count == static_cast<std::size_t>(pipeline) + static_cast<std::size_t>(resource),
            "control inventory policy changed");
        if (pipeline || resource) require(editableDraws(*control.root) == 0, "old sealed-graph failure not reproduced");
        std::size_t finalizations = 0;
        auto result = input.build(resource, [&](vsg::Group& graph) {
            ++finalizations;
            Inventories early; graph.accept(early);
            require(early.count == 0, "P3 finalizer ran after inventory sealing");
            require(editableDraws(graph) == input.plan.asset.draws.size(), "P3 finalizer cannot reach every draw");
        });
        require(finalizations == 1, "P3 finalizer was skipped or repeated");
        Inventories final; result.root->accept(final);
        require(final.count == original.count, "P3 bypassed inventory optimization");
        require(result.stats.drawCount == control.stats.drawCount, "pre-seal callback changed control geometry");
        std::cout << "PASS production pre-seal seam pipeline=" << pipeline << " resource=" << resource << '\n';
    }

#ifdef P3_GPU_PIXEL_TESTS
    constexpr unsigned Size = 64;
    using Pixels = std::vector<unsigned char>;
    struct Frame
    {
        FrameView view;
        RenderVsg::FrameCameraObjects camera;
        RenderVsg::OffscreenRenderTarget target;
        vsg::ref_ptr<vsg::View> graph;
        vsg::ref_ptr<RenderVsg::OpenMwViewDependentState> state;
        Frame(vsg::ref_ptr<vsg::Device> device, vsg::ref_ptr<vsg::Group> root)
        {
            view.extent = {Size,Size}; view.current.projection.nearPlane = .1; view.current.projection.farPlane = 100.;
            view.current.projection.matrix = glm::orthoRH_ZO(-4.f,4.f,-4.f,4.f,100.f,.1f);
            view.current.projection.matrix[1][1] *= -1;
            camera = RenderVsg::FrameCameraObjects::create(view);
            target = RenderVsg::createOffscreenRenderTarget(device,{Size,Size},RenderTargetFormat::Rgba8Srgb,RenderTargetFormat::Depth32Float);
            require(bool(target), "create offscreen target");
            graph = vsg::View::create(camera.camera,root,vsg::RECORD_LIGHTS);
            state = RenderVsg::OpenMwViewDependentState::create(graph.get());
            state->shaderSet = RenderVsg::createLegacyCompatibilityShaderSet();
            graph->viewDependentState = state; graph->bins = RenderVsg::createStaticConformanceBins();
            auto light = vsg::AmbientLight::create(); light->intensity = 1.f;
            graph->addChild(light); target.renderGraph->addChild(graph);
        }
        void move(double x)
        {
            view.current.worldPosition = {x,0,0};
            view.current.view = glm::translate(glm::mat4(1.f), glm::vec3(static_cast<float>(-x),0,0));
            camera.update(view);
            state->setEnvironment(FrameEnvironmentState{}, view.current.projection);
        }
    };
    Pixels readPixels(vsg::ref_ptr<vsg::Device> device, Frame& frame, unsigned family)
    {
        // Fixture-only pixel assertion, never reads GPU visibility/commands.
        auto buffer = vsg::createBufferAndMemory(device,Size*Size*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_SHARING_MODE_EXCLUSIVE,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        auto pool = vsg::CommandPool::create(device,family); auto fence = vsg::Fence::create(device);
        auto copy = vsg::Commands::create();
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        copy->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
            vsg::ImageMemoryBarrier::create(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,frame.target.color->image,range)));
        auto command = vsg::CopyImageToBuffer::create(); command->srcImage = frame.target.color->image;
        command->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; command->dstBuffer = buffer;
        VkBufferImageCopy region{}; region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent = {Size,Size,1};
        command->regions.push_back(region); copy->addChild(command);
        copy->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,
            vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,frame.target.color->image,range)));
        require(vsg::submitCommandsToQueue(pool,fence,10000000000ull,device->getQueue(family),
            [&](vsg::CommandBuffer& cb) {copy->record(cb);}) == VK_SUCCESS, "pixel copy submit");
        auto mapped = vsg::MappedData<vsg::ubyteArray>::create(buffer->getDeviceMemory(device->deviceID),
            buffer->getMemoryOffset(device->deviceID),0,vsg::Data::Properties{},Size*Size*4);
        Pixels bytes(Size*Size*4); std::memcpy(bytes.data(),mapped->dataPointer(),bytes.size()); return bytes;
    }
    std::vector<Pixels> render(vsg::ref_ptr<vsg::Device> device, bool resource, int mode)
    {
        World input;
        auto scene = vsg::Group::create(); Frame main(device,scene), secondary(device,scene);
        auto viewData = RenderVsg::createGpuPopulationCullViewData();
        RenderVsg::GpuPopulationCullBuild cull; std::string reason;
        auto asset = input.build(resource, [&](vsg::Group& graph) {
            if (mode) cull = RenderVsg::enableGpuPopulationCull(input.world,input.plan,graph,*device,main.graph->viewID,viewData,mode==2,reason);
        });
        if (mode)
        {
            require(cull.active, "real P3 builder inactive: " + reason);
            require(cull.stats.compacted == (mode==2), "real P3B builder did not compact: " + reason);
            require(cull.stats.placements == 3 && cull.stats.draws == 1 && cull.stats.indirectCommands == (mode==2?1u:3u),
                "P3 coverage counters do not describe the real draw graph");
            // Negative control: the old post-seal builder must explicitly reject
            // the finished graph, without mutating its live command topology.
            if (resource || std::getenv("OPENMW_V4_PIPELINE_INVENTORIES"))
            {
                std::string blocked;
                auto sealedDirect = input.build(resource);
                auto old = RenderVsg::enableGpuPopulationCull(input.world,input.plan,*sealedDirect.root,*device,main.graph->viewID,viewData,mode==2,blocked);
                require(!old.active && blocked.find("no rewritable draws") != std::string::npos,
                    "sealed negative control did not diagnose the old routing failure");
            }
        }
        scene->addChild(asset.root);
        auto viewer = vsg::Viewer::create();
        const auto family = device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);
        auto commands = vsg::CommandGraph::create(device,family);
        if (cull.active) commands->addChild(cull.compute);
        commands->addChild(main.target.renderGraph); commands->addChild(secondary.target.renderGraph);
        viewer->assignRecordAndSubmitTaskAndPresentation({commands});
        require(bool(viewer->compile()), "real P3 shader/draw compilation failed");
        std::vector<Pixels> output;
        unsigned tick = 0;
        // Exhaust visibility, repopulate, and cross the three-slot task ring.
        // The secondary camera must always draw its original direct population.
        for (double x : {0.,20.,100.,0.,20.,100.,0.,20.})
        {
            main.move(x); secondary.move(0);
            RenderVsg::updateGpuPopulationCullViewData(*viewData,main.view);
            require(viewer->advanceToNextFrame(double(++tick)), "advance"); viewer->update();
            for (auto& task : viewer->recordAndSubmitTasks)
            {
                for (auto& graph : task->commandGraphs) graph->reset();
                require(RenderVsg::submitTaskChecked(*task,vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp())) == VK_SUCCESS, "GPU submit");
            }
            viewer->deviceWaitIdle(); // Test readback only.
            output.push_back(readPixels(device,main,static_cast<unsigned>(family)));
            output.push_back(readPixels(device,secondary,static_cast<unsigned>(family)));
        }
        require(output.front() != output[4], "fixture did not distinguish visible from empty view");
        for (std::size_t i=1;i<output.size();i+=2) require(output[i] == output[1], "secondary-view fallback changed with main visibility");
        std::cout << "PASS real GPU route mode=" << mode << " frames=" << tick << " groups=" << cull.active
                  << " compact=" << cull.stats.compacted << " commands=" << cull.stats.indirectCommands << '\n';
        return output;
    }
    void checkGpu(bool resource)
    {
        auto instance = vsg::Instance::create(vsg::Names{},vsg::Names{"VK_LAYER_KHRONOS_validation"},VK_API_VERSION_1_2);
        auto physical = instance->getPhysicalDevices(); require(!physical.empty(), "no Vulkan ICD for required P3 fixture");
        auto selected = physical.front(); const auto supported = selected->getFeatures();
        require(supported.multiDrawIndirect && supported.drawIndirectFirstInstance, "indirect features unsupported");
        auto features = vsg::DeviceFeatures::create();
        features->get().samplerAnisotropy = supported.samplerAnisotropy;
        features->get().multiDrawIndirect = VK_TRUE; features->get().drawIndirectFirstInstance = VK_TRUE;
        const auto family = selected->getQueueFamily(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT);
        auto device = vsg::Device::create(selected,vsg::QueueSettings{{family,{1.f}}},vsg::Names{},vsg::Names{},features);
        std::cout << "DEVICE " << selected->getProperties().deviceName << '\n';
        const auto control = render(device,resource,0);
        require(render(device,resource,1) == control, "P3A pixels differ from exact direct control");
        require(render(device,resource,2) == control, "P3B pixels differ from exact direct control");
        std::cout << "PASS direct/P3A/P3B exact pixel parity for main and secondary views\n";
    }
#endif
}
int main(int argc,char** argv)
{
    std::cout << std::unitbuf;
    try
    {
        const std::string mode = argc>1?argv[1]:"both";
        require(mode=="none" || mode=="pipeline" || mode=="resource" || mode=="both", "invalid inventory fixture mode");
        const bool pipeline = mode=="pipeline" || mode=="both", resource = mode=="resource" || mode=="both";
        flag("OPENMW_V4_STARTUP_FLAG_CACHE",true);
        flag("OPENMW_V4_PIPELINE_INVENTORIES",pipeline); flag("OPENMW_VK_RESOURCE_INVENTORIES",resource);
        checkSeam(pipeline,resource);
#ifdef P3_GPU_PIXEL_TESTS
        if (argc>2 && std::string(argv[2])=="--gpu") checkGpu(resource);
#endif
        return 0;
    }
    catch (const vsg::Exception& e) {std::cerr << "FAIL VSG " << e.message << " result=" << e.result << '\n';return 1;}
    catch (const std::exception& e) {std::cerr << "FAIL " << e.what() << '\n';return 1;}
}
