// Headless pixel tests of the production pipelines. No game window, save, mod
// configuration or benchmark is involved. A Vulkan implementation is required.
#include <components/render/backend/vsg/nativesky.hpp>
#include <components/render/backend/vsg/nativepostprocess.hpp>
#include <components/render/backend/vsg/nativevisibility.hpp>
#include <components/render/backend/vsg/effectvisibility.hpp>
#include <components/render/backend/vsg/persistentdrawscene.hpp>
#include <components/render/backend/vsg/isolatedscene.hpp>
#include <components/render/backend/vsg/offscreenrendertarget.hpp>
#include <components/render/backend/vsg/openmwviewdependentstate.hpp>
#include <components/render/backend/vsg/statictexturedecode.hpp>
#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <components/render/backend/vsg/uipipeline.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <components/render/backend/vsg/parallelrecordtask.hpp>
#include <components/render/backend/vsg/watersurface.hpp>
#include <components/render/backend/vsg/waterinputprobe.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <vsg/all.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace RepairFixtures { RenderCore::ImmediateEffectDraw captureAuthoredMaterial(); }

namespace
{
    using namespace RenderCore;
    void require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
    using Pixels = std::vector<unsigned char>;
    vsg::ubvec4 pixel(const Pixels& p, unsigned x=64, unsigned y=64)
    {
        const auto i=(y*128+x)*4;
        return {p[i],p[i+1],p[i+2],p[i+3]};
    }
    int encoded(float linear)
    {
        linear=std::clamp(linear,0.f,1.f);
        return int(std::lround(255.f*(linear<=.0031308f?linear*12.92f:1.055f*std::pow(linear,1.f/2.4f)-.055f)));
    }
    void close(int actual, int expected, const std::string& label, int tolerance=4)
    {
        require(std::abs(actual-expected)<=tolerance,label+": actual="+std::to_string(actual)+" expected="+std::to_string(expected));
    }
    struct Fixture
    {
        vsg::ref_ptr<vsg::Device> device;
        RenderVsg::OffscreenRenderTarget target;
        FrameView frame;
        FrameEnvironmentState environment;
        RenderVsg::FrameCameraObjects camera;
        vsg::ref_ptr<vsg::Group> root=vsg::Group::create();
        vsg::ref_ptr<vsg::View> view;
        vsg::ref_ptr<RenderVsg::OpenMwViewDependentState> state;
        vsg::ref_ptr<vsg::Viewer> viewer=vsg::Viewer::create();
        vsg::ref_ptr<vsg::CommandGraph> commands;
        std::vector<vsg::ref_ptr<vsg::View>> parallelViews;
        vsg::ref_ptr<vsg::SharedObjects> shared=vsg::SharedObjects::create();
        std::uint64_t tick=0;
        explicit Fixture(vsg::ref_ptr<vsg::Device> input, vsg::ViewFeatures features = vsg::RECORD_ALL,
            unsigned int shadowCascades = 0, RenderTargetFormat format = RenderTargetFormat::Rgba8Srgb,
            bool independentRecording = false) : device(input)
        {
            frame.extent={128,128}; frame.current.projection.nearPlane=.1; frame.current.projection.farPlane=10000.;
            frame.current.projection.matrix=glm::perspectiveRH_ZO(glm::radians(60.f),1.f,10000.f,.1f);
            frame.current.projection.matrix[1][1]*=-1.f;
            camera=RenderVsg::FrameCameraObjects::create(frame);
            target=RenderVsg::createOffscreenRenderTarget(device,{128,128},format,RenderTargetFormat::Depth32Float,
                format==RenderTargetFormat::Rgba16Float);
            require(bool(target),"headless target");
            require(target.renderGraph->clearValues.size()==2 && target.renderGraph->clearValues[1].depthStencil.depth==0.f,
                "default reverse-depth clear missing or typed as colour");
            view=vsg::View::create(camera.camera,root,features);
            state=RenderVsg::OpenMwViewDependentState::create(view.get());
            state->shaderSet=RenderVsg::createLegacyCompatibilityShaderSet();
            view->viewDependentState=state;
            view->bins=RenderVsg::createStaticConformanceBins();
            view->bins.push_back(vsg::Bin::create(RenderVsg::UiOverlayBinNumber,vsg::Bin::NO_SORT));
            target.renderGraph->addChild(view);
            commands=vsg::CommandGraph::create(device,device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT));
            commands->addChild(target.renderGraph);
            viewer->assignRecordAndSubmitTaskAndPresentation({commands});
            if (independentRecording && std::getenv("OPENMW_V4_PARALLEL_VIEW_RECORD"))
            {
                auto task = RenderVsg::ParallelRecordTask::create(*viewer->recordAndSubmitTasks.front());
                for (int order : {-4, -3})
                {
                    auto extra = RenderVsg::createOffscreenRenderTarget(device,{128,128},format,RenderTargetFormat::Depth32Float);
                    auto extraView = vsg::View::create(camera.camera, root, vsg::RECORD_LIGHTS);
                    auto extraState = RenderVsg::OpenMwViewDependentState::create(extraView.get());
                    extraState->shaderSet = RenderVsg::createLegacyCompatibilityShaderSet();
                    extraView->viewDependentState = extraState;
                    extraView->bins = RenderVsg::createStaticConformanceBins();
                    extraView->bins.push_back(vsg::Bin::create(RenderVsg::UiOverlayBinNumber,vsg::Bin::NO_SORT));
                    extra.renderGraph->addChild(extraView);
                    auto graph = vsg::CommandGraph::create(device, commands->queueFamily);
                    graph->submitOrder = order; graph->addChild(extra.renderGraph);
                    task->commandGraphs.push_back(graph);
                    parallelViews.push_back(extraView);
                }
                viewer->recordAndSubmitTasks.front() = task;
            }
            auto hints=vsg::ResourceHints::create();
            if (shadowCascades)
            {
                hints->numShadowMapsRange={shadowCascades,shadowCascades};
                hints->shadowMapSize={64,64};
            }
            auto result=viewer->compile(hints);
            require(bool(result),"initial target compilation "+result.message);
        }
        bool compile(vsg::ref_ptr<vsg::Node> node)
        {
            for (const auto& extra : parallelViews)
                if (!RenderVsg::compileForViewerView(*viewer,*extra,node)) return false;
            auto result=RenderVsg::compileForViewerView(*viewer,*view,node);
            if (!result) std::cerr<<"compile: "<<result.message<<" code="<<result.result<<'\n';
            return bool(result);
        }
        Pixels render()
        {
            camera.update(frame);
            state->setEnvironment(environment,frame.current.projection);
            for (const auto& extra : parallelViews)
                static_cast<RenderVsg::OpenMwViewDependentState*>(extra->viewDependentState.get())
                    ->setEnvironment(environment,frame.current.projection);
            require(viewer->advanceToNextFrame(double(++tick)),"advance frame");
            viewer->update();
            for (auto& task:viewer->recordAndSubmitTasks)
            {
                for (auto& graph:task->commandGraphs) graph->reset();
                require(RenderVsg::submitTaskChecked(*task,vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp()))==VK_SUCCESS,"pixel submit");
                if (std::getenv("OPENMW_V4_SUBMIT_BREAKDOWN"))
                {
                    auto* measured=dynamic_cast<RenderVsg::SubmitTaskDiagnostics*>(task->instrumentation.get());
                    require(measured && measured->observedScopes()>=(parallelViews.empty()?3:2)*tick,
                        "installed VSG did not provide submission scopes");
                }
            }
            viewer->deviceWaitIdle(); // Test readback only, not production synchronization.
            auto buffer=vsg::createBufferAndMemory(device,128*128*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_SHARING_MODE_EXCLUSIVE,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            const auto family=device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT);
            auto pool=vsg::CommandPool::create(device,family); auto fence=vsg::Fence::create(device);
            auto copy=vsg::Commands::create();
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            copy->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
                vsg::ImageMemoryBarrier::create(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,target.color->image,range)));
            auto command=vsg::CopyImageToBuffer::create(); command->srcImage=target.color->image;
            command->srcImageLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; command->dstBuffer=buffer;
            VkBufferImageCopy region{}; region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent={128,128,1};
            command->regions.push_back(region); copy->addChild(command);
            copy->addChild(vsg::PipelineBarrier::create(VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,
                vsg::ImageMemoryBarrier::create(VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED,VK_QUEUE_FAMILY_IGNORED,target.color->image,range)));
            require(vsg::submitCommandsToQueue(pool,fence,10000000000ull,device->getQueue(family),
                [&](vsg::CommandBuffer& commandBuffer){copy->record(commandBuffer);})==VK_SUCCESS,"readback submit");
            auto mapped=vsg::MappedData<vsg::ubyteArray>::create(buffer->getDeviceMemory(device->deviceID),
                buffer->getMemoryOffset(device->deviceID),0,vsg::Data::Properties{},128*128*4);
            Pixels result(128*128*4); std::memcpy(result.data(),mapped->dataPointer(),result.size());return result;
        }
    };
    std::shared_ptr<MeshPayload> quad(float z=-5.f)
    {
        auto mesh=std::make_shared<MeshPayload>();
        mesh->positions={{-4,-4,z},{4,-4,z},{4,4,z},{-4,4,z}};
        mesh->normals.assign(4,{0,0,1}); mesh->colors.assign(4,{1,1,1,1});
        mesh->texCoordSets={{{0,0},{1,0},{1,1},{0,1}}};
        mesh->indices={0,1,2,0,2,3}; mesh->surfaces={{PrimitiveTopology::Triangles,0,6,0}};
        return mesh;
    }
    EffectTextureSnapshot texture(const std::string& id,vsg::ubvec4 rgba,TextureRole role=TextureRole::Diffuse)
    {
        EffectTextureSnapshot result;
        result.texture.sourceIdentity=result.texture.contentIdentity=id;
        result.texture.width=result.texture.height=1; result.texture.mipmapped=false;
        auto pixels=std::make_shared<TexturePixels>(); pixels->rgba8={rgba.r,rgba.g,rgba.b,rgba.a};result.texture.pixels=pixels;
        result.binding.role=role; result.binding.colorSpace=role==TextureRole::Normal?TextureColorSpace::Data:TextureColorSpace::Srgb;
        result.binding.formatClass=role==TextureRole::Normal?TextureFormatClass::Normal:TextureFormatClass::Color;
        result.binding.sampler.minFilter=result.binding.sampler.magFilter=TextureFilter::Nearest;
        result.binding.sampler.wrapU=result.binding.sampler.wrapV=TextureWrap::Clamp;
        result.binding.sampler.mipmapMode=TextureMipmapMode::None;
        return result;
    }
    RenderVsg::StaticTextureResolver resolver()
    {
        auto decoder=std::make_shared<RenderVsg::StaticTextureDecoder>();
        return [decoder](const TextureRecord& record,const TextureRealizationKey& key){return decoder->decode(record,key,{});};
    }
    vsg::ref_ptr<vsg::Node> uiQuad(const RenderVsg::UiPipeline& ui,vsg::ref_ptr<vsg::ImageView> image,
        bool premultiplied,bool flip,bool invertedInput=false,std::uint32_t color=0xffffffffu)
    {
        struct Vertex {float x,y,z;std::uint32_t color;float u,v;};
        auto values=vsg::ubyteArray::create(sizeof(Vertex)*6);
        const float top=invertedInput?1.f:0.f,bottom=1.f-top;
        Vertex vertices[]={{-1,-1,0,color,0,bottom},{1,-1,0,color,1,bottom},{1,1,0,color,1,top},
            {-1,-1,0,color,0,bottom},{1,1,0,color,1,top},{-1,1,0,color,0,top}};
        std::memcpy(values->dataPointer(),vertices,sizeof(vertices));
        auto root=vsg::StateGroup::create();root->add(ui.bindPipeline);
        root->add(RenderVsg::createUiTextureBinding(ui,vsg::ImageInfo::create(ui.sampler,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),premultiplied,flip));
        root->addChild(vsg::BindVertexBuffers::create(0,vsg::DataList{values})); root->addChild(vsg::Draw::create(6,1,0,0));
        return root;
    }
    void checkPersistentDrawScene(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture main(device), auxiliary(device);
        PersistentDrawWorld world; PersistentDrawHandle handle;
        RenderVsg::PersistentDrawScene scene;
        main.root->addChild(scene.root()); auxiliary.root->addChild(scene.root());
        ImmediateEffectDraw draw; draw.identity="persistent-pixel"; draw.mesh=*quad();
        draw.material.sourceIdentity=draw.identity; draw.material.unlit=true;
        draw.material.cullMode=CullMode::None; draw.material.diffuse={1,0,0,1};
        draw.material.vertexColorMode=VertexColorMode::Ignore;
        auto resolve=resolver(); std::string diagnostic; unsigned compilations=0;
        const auto compile=[&](auto node) { ++compilations; return main.compile(node) && auxiliary.compile(node); };
        const auto sync=[&](auto frame) {
            require(scene.synchronize(frame,{},resolve,compile,diagnostic),diagnostic);
            require(RenderVsg::graphicsPipelinesRealizedForView(*scene.root(),*main.view)
                && RenderVsg::graphicsPipelinesRealizedForView(*scene.root(),*auxiliary.view),"persistent view audit");
        };
        world.begin(1); world.update(handle,draw,true); auto first=world.finish(); sync(first);
        auto reference=main.render(); require(pixel(reference).r>240,"persistent initial pixels");
        world.begin(1); draw.worldTransform[3].x=100; world.update(handle,draw,false); sync(world.finish());
        require(pixel(main.render()).r<4,"persistent placement/frustum update");
        auxiliary.frame.current.view=glm::translate(glm::mat4(1),glm::vec3(-100,0,0));
        require(pixel(auxiliary.render()).r>240,"persistent auxiliary visibility used main result");
        require(compilations==1,"placement recompiled asset");
        world.begin(1); draw.worldTransform=glm::mat4(1); world.update(handle,draw,false); sync(world.finish());
        require(main.render()==reference,"persistent movement changed pixels");
        world.begin(1); world.hide(handle); sync(world.finish());
        require(pixel(main.render()).r<4,"persistent hidden draw remained");
        world.begin(1); draw.material.diffuse={0,1,0,1}; world.update(handle,draw,true); sync(world.finish());
        require(pixel(main.render()).g>240 && compilations==2,"persistent material update/unhide");
        world.begin(1); sync(world.finish()); require(pixel(main.render()).g<4,"persistent unload left ghost");
        // A fresh backend/epoch can consume a complete retained snapshot.
        sync(first); require(main.render()==reference,"persistent rewind/recovery pixels");
        struct ReleaseMain : vsg::Visitor
        {
            unsigned viewId;
            explicit ReleaseMain(unsigned id) : viewId(id) {}
            void apply(vsg::Object& object) override { object.traverse(*this); }
            void apply(vsg::BindGraphicsPipeline& bind) override { bind.pipeline->release(viewId); }
        } releaseMain(main.view->viewID);
        // render() has completed readback/fence waiting; release is safe here.
        scene.root()->accept(releaseMain);
        require(!RenderVsg::graphicsPipelinesRealizedForView(*scene.root(),*main.view)
            && RenderVsg::graphicsPipelinesRealizedForView(*scene.root(),*auxiliary.view),
            "persistent inventory cached a released view implementation");
        require(main.compile(scene.root()) && main.render()==reference,"persistent pipeline recompile pixels");
        std::cout<<"PASS persistent scene pixels: create, unchanged resource, movement, per-view cull, hide, rebind, unload, recovery\n";
    }
    void checkNativeVisibilityRouting(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device);
        ImmediateEffectDraw draw;draw.identity="visibility-view-routing";draw.mesh=*quad();
        draw.material.sourceIdentity="visibility-view-routing";draw.material.unlit=true;
        draw.material.cullMode=CullMode::None;draw.material.diffuse={1,0,0,1};
        draw.material.vertexColorMode=VertexColorMode::Ignore;
        auto realized=RenderVsg::realizeImmediateEffectDraw(draw,resolver(),f.shared);
        require(realized.valid(),"visibility routing geometry");
        auto gate=RenderVsg::MainViewVisibility::create();
        gate->mainViewId=f.view->viewID;gate->visible=false;gate->addChild(realized.root);
        f.root->children={gate};require(f.compile(f.root),"invisible candidate must still compile");
        require(pixel(f.render()).r<4,"main-view gate failed to reject draw");
        gate->mainViewId=f.view->viewID+1;
        require(pixel(f.render()).r>240,"other-view traversal inherited main-view rejection");
        gate->mainViewId=f.view->viewID;gate->visible=true;
        require(pixel(f.render()).r>240,"visible main-view candidate missing");
        std::cout<<"PASS native visibility GPU routing: compile hidden, reject main, preserve other view, restore visible\n";
    }
    struct RecordCounter : vsg::Inherit<vsg::Group, RecordCounter>
    {
        mutable unsigned records = 0;
        void accept(vsg::RecordTraversal& visitor) const override
        { ++records; vsg::Group::accept(visitor); }
    };
    void checkEffectFrustum(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture main(device), auxiliary(device);
        ImmediateEffectDraw draw; draw.identity="effect-frustum"; draw.mesh=*quad();
        draw.material.sourceIdentity=draw.identity; draw.material.unlit=true;
        draw.material.cullMode=CullMode::None; draw.material.diffuse={1,0,0,1};
        draw.material.vertexColorMode=VertexColorMode::Ignore;
        auto realized=RenderVsg::realizeImmediateEffectDraw(draw,resolver(),main.shared);
        require(realized.valid(),"frustum geometry");
        auto counter=RecordCounter::create(); counter->addChild(realized.root);
        auto bound=RenderVsg::effectCullBound(draw); require(bool(bound),"frustum bound");
        auto cull=vsg::CullGroup::create(); cull->bound=*bound; cull->addChild(counter);
        auto placed=vsg::MatrixTransform::create(); placed->addChild(cull);
        main.root->children={placed}; auxiliary.root->children={placed};
        require(main.compile(placed) && auxiliary.compile(placed),"both views compile shared graph");
        auto reference=main.render(); require(pixel(reference).r>240 && counter->records>0,"visible effect missing");
        placed->matrix=vsg::translate(100.0,0.0,0.0); counter->records=0;
        require(pixel(main.render()).r<4 && counter->records==0,"offscreen effect reached record traversal");
        auxiliary.frame.current.view=glm::translate(glm::mat4(1),glm::vec3(-100,0,0));
        require(pixel(auxiliary.render()).r>240 && counter->records>0,"auxiliary view inherited main rejection");
        placed->matrix=vsg::dmat4(); counter->records=0;
        require(main.render()==reference && counter->records>0,"movement did not restore identical pixels");
        placed->matrix=vsg::scale(-2.0,0.5,1.0);
        require(pixel(main.render()).r>240,"nonuniform mirrored placement culled");
        // A population gate owns world bounds OUTSIDE its placement. The same
        // gate must reject an entire DAG in one view and retain it in another.
        auto gate=RenderVsg::MainViewVisibility::create();
        gate->perViewFrustum=true;gate->bounded=true;gate->mainViewId=main.view->viewID;
        gate->minimum={99,-1,-.1};gate->maximum={101,1,.1};
        placed->matrix=vsg::translate(100.,0.,0.);gate->addChild(placed);
        main.root->children={gate};auxiliary.root->children={gate};
        require(main.compile(gate)&&auxiliary.compile(gate),"population multi-view compilation");
        counter->records=0;require(pixel(main.render()).r<4&&counter->records==0,"offscreen population traversed");
        gate->visible=false;
        require(pixel(auxiliary.render()).r>240&&counter->records>0,"population used another view's occlusion or frustum");
        gate->visible=true;gate->minimum={-1,-1,-.1};gate->maximum={1,1,.1};placed->matrix=vsg::dmat4();
        require(main.render()==reference,"population movement did not restore pixels");
        std::cout<<"PASS evaluated-effect frustum: real record rejection, independent auxiliary view, movement, mirrored scale\n";
    }
    void checkNativePostProcess(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device);
        auto scene=RenderVsg::createOffscreenRenderTarget(device,{128,128},
            RenderTargetFormat::Rgba16Float,RenderTargetFormat::Depth32Float,true);
        require(bool(scene),"native HDR + sampled depth target");
        scene.setClearValues({{.125f,.25f,.5f,1.f}},{.25f,0});
        f.commands->children.insert(f.commands->children.begin(),scene.renderGraph);
        require(f.compile(scene.renderGraph),"postprocess scene clear compilation");
        for(auto mode : {RenderVsg::NativePostProcessMode::Copy,RenderVsg::NativePostProcessMode::EdgeAA})
        {
            f.root->children={RenderVsg::createNativePostProcess(scene.color,scene.depth,mode)};
            require(f.compile(f.root),"native postprocess compilation");
            for(int i=0;i<5;++i)
            {
                auto c=pixel(f.render());
                close(c.r,encoded(.125f),"native HDR R / flat edge AA");
                close(c.g,encoded(.25f),"native HDR G / no double gamma");
                close(c.b,encoded(.5f),"native HDR B");
            }
        }
        f.root->children={RenderVsg::createNativePostProcess(scene.color,scene.depth,RenderVsg::NativePostProcessMode::Depth)};
        require(f.compile(f.root),"native depth input compilation");
        close(pixel(f.render()).r,encoded(.25f),"native sampled reversed depth");
        // Crisp UI must be drawn after the effect, not filtered or depth-visualized.
        auto ui=RenderVsg::createUiPipeline(128,128);
        auto red=vsg::ubvec4Array2D::create(1,1,vsg::ubvec4(255,0,0,255),vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        f.root->addChild(RenderVsg::createUiOverlayLayer(uiQuad(ui,vsg::ImageView::create(vsg::Image::create(red)),false,false)));
        require(f.compile(f.root),"postprocess GUI compilation");
        auto overlay=pixel(f.render());require(overlay.r>250 && overlay.g<4,"GUI was processed or overdrawn");
        // Explicit asymmetric source checks row orientation through the fullscreen pass.
        auto rows=vsg::vec4Array2D::create(1,2,vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
        (*rows)(0,0)=vsg::vec4(1,0,0,1);(*rows)(0,1)=vsg::vec4(0,0,1,1);
        auto image=vsg::ImageView::create(vsg::Image::create(rows));
        f.root->children={RenderVsg::createNativePostProcess(image,scene.depth,RenderVsg::NativePostProcessMode::Copy)};
        require(f.compile(f.root),"postprocess orientation compilation");auto oriented=f.render();
        require(pixel(oriented,64,4).r>240 && pixel(oriented,64,124).b>240,"native postprocess Y orientation");
        std::cout<<"PASS native postprocess: linear HDR, sampled depth, flat AA, repeated reuse, UI ordering, orientation\n";
    }
    void checkSky(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device); RenderVsg::NativeSky sky; auto resolve=resolver();
        f.root->addChild(sky.node()); NativeSkySnapshot snapshot;
        SkyDrawSnapshot atmosphere; atmosphere.identity="atmosphere"; atmosphere.mesh=quad();
        atmosphere.cullMode=CullMode::None; atmosphere.diffuseColor={.1f,.2f,.3f,1.f};snapshot.draws={atmosphere};
        std::uint64_t frame=0;
        auto render=[&]{std::string diagnostic;auto id=FrameId(++frame);
            require(sky.prepare(&snapshot,f.environment,f.frame,id,frame>1?std::optional(FrameId(frame-1)):std::nullopt,
                resolve,f.shared,[&](auto node){return f.compile(node);},diagnostic,true),diagnostic);
            auto pixels=f.render(); require(sky.markSubmitted(id),"sky submission watermark");return pixels;};
        auto daytime=pixel(render());close(daytime.r,encoded(.1f),"native atmosphere R");close(daytime.g,encoded(.2f),"native atmosphere G");
        SkyDrawSnapshot night=atmosphere;night.identity="night";night.pass=SkyPass::Night;night.opacity=.5f;
        night.textures={texture("stars",{255,0,0,255})};snapshot.draws.push_back(night);
        auto stars=pixel(render());close(stars.r,encoded(.55f),"night alpha");close(stars.g,encoded(.1f),"night background blend");
        SkyDrawSnapshot cloud=night; cloud.identity="cloud";cloud.pass=SkyPass::Clouds;cloud.diffuseColor={.2f,.4f,.6f,1};
        cloud.textures={texture("cloud",{255,255,255,255})}; cloud.opacity=.75f;snapshot.draws={atmosphere,cloud};
        auto clouds=pixel(render());close(clouds.r,encoded(.1f*.25f+.2f*.75f),"cloud tint and opacity");
        SkyDrawSnapshot moon=night;moon.identity="moon";moon.pass=SkyPass::Moon;moon.sourceBlend=BlendFactor::One;
        moon.moonBlend={.5f,.25f,0,1};moon.atmosphereFade={0,0,.25f,.5f};moon.textures={texture("phase",{255,255,255,255}),texture("mask",{255,255,255,255})};
        snapshot.draws={atmosphere,moon};auto phase=pixel(render());
        close(phase.r,encoded(.1f*.5f+.5f*.5f),"moon premultiplied phase");close(phase.b,encoded(.3f*.5f+.25f*.5f),"moon mask");
        auto sun=night;sun.identity="sun";sun.pass=SkyPass::Sun;sun.opacity=.25f;sun.textures={texture("sun",{0,255,0,255})};
        snapshot.draws={atmosphere,sun};close(pixel(render()).g,encoded(.2f*.75f+.25f),"solar opacity");
        snapshot.draws={atmosphere};auto fixed=pixel(render());
        f.frame.current.worldPosition={1024,2048,4096};f.frame.current.view=glm::translate(glm::mat4(1.f),glm::vec3(-1024,-2048,-4096));
        require(pixel(render())==fixed,"camera-relative sky translated with gameplay position");
        for(unsigned i=0;i<12;++i) {snapshot.draws[0].diffuseColor.r=float(i)/20.f;close(pixel(render()).r,encoded(float(i)/20.f),"sky live colour update");}
        require(sky.residentVersions()<=3,"sky completed versions grew unbounded");
        auto malformed=snapshot;malformed.draws[0].pass=static_cast<SkyPass>(255);require(!validNativeSky(malformed),"unknown sky pass accepted");
        malformed=snapshot;malformed.draws.push_back(malformed.draws.front());require(!validNativeSky(malformed),"duplicate sky identity accepted");
        std::cout<<"PASS native sky pixels: atmosphere, night, clouds, moon, sun, camera-relative placement, live uniforms, bounded reuse\n";
    }
    void checkPreview(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device);
        auto target=RenderVsg::createOffscreenRenderTarget(device,{128,128});
        require(bool(target),"preview floating point target");target.setClearValues({{0,0,0,0}},{0,0});
        auto previewRoot=vsg::Group::create();auto previewView=vsg::View::create(f.camera.camera,previewRoot);
        auto state=RenderVsg::OpenMwViewDependentState::create(previewView.get());state->shaderSet=RenderVsg::createLegacyCompatibilityShaderSet();
        previewView->viewDependentState=state;previewView->bins=RenderVsg::createStaticConformanceBins();
        target.renderGraph->addChild(previewView); f.commands->children.insert(f.commands->children.begin(),target.renderGraph);
        IsolatedSceneSnapshot snapshot; snapshot.identity=1;snapshot.revision=1;snapshot.viewportExtent={128,128};
        ImmediateEffectDraw draw;draw.identity="preview:translucent";draw.mesh=*quad();draw.material.sourceIdentity="preview-material";
        draw.material.unlit=true;draw.material.cullMode=CullMode::None;draw.material.alphaBlendEnabled=true;
        draw.material.alphaMode=AlphaMode::Blend;draw.material.separateAlphaBlend=true;
        draw.material.sourceAlphaBlend=BlendFactor::One;draw.material.destinationAlphaBlend=BlendFactor::OneMinusSourceAlpha;
        draw.material.vertexColorMode=VertexColorMode::Ignore; draw.material.diffuse={1,1,1,1};draw.material.alpha=1;
        draw.textures={texture("translucent-red",{255,0,0,128})};snapshot.draws={draw};
        std::string diagnostic;auto graph=RenderVsg::realizeIsolatedScene(snapshot,resolver(),f.shared,diagnostic);
        require(bool(graph),diagnostic);previewRoot->addChild(graph);
        auto ui=RenderVsg::createUiPipeline(128,128);f.root->addChild(uiQuad(ui,target.color,true,true,true));
        auto compile=f.viewer->compile();require(bool(compile),"preview/UI pipeline compile: "+compile.message);
        auto p=pixel(f.render());close(p.r,encoded(128.f/255.f),"premultiplied preview was multiplied by alpha twice");close(p.g,0,"preview background leaked");
        f.root->children={uiQuad(ui,target.color,true,true,true,0x80ffffffu)}; require(f.compile(f.root),"preview widget fade compile");
        close(pixel(f.render()).r,encoded((128.f/255.f)*(128.f/255.f)),"preview widget opacity");
        // Ordinary UI still uses straight-alpha input. Its colour must not be
        // changed by introducing premultiplied render-target inputs elsewhere.
        auto straight=vsg::ubvec4Array2D::create(1,1,vsg::ubvec4(255,0,0,128),vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        auto image=vsg::ImageView::create(vsg::Image::create(straight));
        f.root->children={uiQuad(ui,image,false,false)};require(f.compile(f.root),"ordinary UI compile");
        close(pixel(f.render()).r,encoded(128.f/255.f),"ordinary straight-alpha UI changed");
        auto rows=vsg::ubvec4Array2D::create(1,2,vsg::Data::Properties(VK_FORMAT_R8G8B8A8_UNORM));
        (*rows)(0,0)=vsg::ubvec4(255,0,0,255);(*rows)(0,1)=vsg::ubvec4(0,0,255,255);
        image=vsg::ImageView::create(vsg::Image::create(rows));f.root->children={uiQuad(ui,image,true,true,true)};
        require(f.compile(f.root),"preview orientation compile");auto oriented=f.render();
        require(pixel(oriented,64,4).r>240 && pixel(oriented,64,124).b>240,"RTT facade UV inversion not corrected");
        auto additive=vsg::vec4Array2D::create(1,1,vsg::vec4(.5f,0,0,0),vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
        image=vsg::ImageView::create(vsg::Image::create(additive));f.root->children={uiQuad(ui,image,true,false)};
        require(f.compile(f.root),"additive RTT compile");close(pixel(f.render()).r,encoded(.5f),"additive alpha-zero RTT discarded");
        std::cout<<"PASS preview pixels: isolated evaluated scene, transparent target, premultiplied sampling, widget alpha, Y convention, ordinary UI, additive alpha-zero\n";
    }
    void checkUnshadowedLighting(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device,vsg::RECORD_LIGHTS,0,RenderTargetFormat::Rgba8Srgb,true);
        auto ambient=vsg::AmbientLight::create();ambient->color={.1f,.2f,.3f};ambient->intensity=1.f;
        auto sun=vsg::DirectionalLight::create();sun->color={.4f,.3f,.2f};sun->direction={0,0,-1};sun->intensity=1.f;
        IsolatedSceneSnapshot scene;scene.identity=7;scene.revision=1;scene.viewportExtent={128,128};
        ImmediateEffectDraw draw;draw.identity="lit-preview";draw.mesh=*quad();
        draw.material.sourceIdentity="lit-preview-material";draw.material.cullMode=CullMode::None;
        draw.material.ambient={1,1,1,1};draw.material.diffuse={1,1,1,1};draw.material.specular={0,0,0,0};
        draw.material.vertexColorMode=VertexColorMode::Ignore;scene.draws={draw};
        std::string diagnostic;auto graph=RenderVsg::realizeIsolatedScene(scene,resolver(),f.shared,diagnostic);
        require(bool(graph),diagnostic);f.root->children={ambient,sun,graph};require(f.compile(f.root),"lit preview compile");
        auto lit=pixel(f.render());
        close(lit.r,encoded(.5f),"unshadowed preview ambient/directional R");
        close(lit.g,encoded(.5f),"unshadowed preview ambient/directional G");
        close(lit.b,encoded(.5f),"unshadowed preview ambient/directional B");
        require(f.state->shadowMaps.empty(),"unshadowed repair allocated shadow maps");
        sun->intensity=0.f;ambient->color={.3f,.1f,.2f};
        auto updated=pixel(f.render());close(updated.r,encoded(.3f),"live ambient update R");
        close(updated.g,encoded(.1f),"live ambient update G");close(updated.b,encoded(.2f),"live ambient update B");
        sun->intensity=1.f;sun->direction={0,0,1};
        close(pixel(f.render()).r,encoded(.3f),"directional eye-space/sign convention");
        f.root->children={graph};require(f.compile(f.root),"light removal compile");
        close(pixel(f.render()).r,0,"removed lights remained in unshadowed buffer");
        std::cout<<"PASS unshadowed lighting pixels: ambient, directional, live colours, ray sign, removed lights, zero shadow maps\n";
    }
    void checkShadowViewFeatures(vsg::ref_ptr<vsg::Device> device)
    {
        // Reproduce runtime feature combinations: a shadowed main world creates
        // INHERIT_VIEWPOINT-only depth views, while a separate preview remains lit.
        Fixture world(device, vsg::RECORD_ALL, 3,RenderTargetFormat::Rgba8Srgb,true);
        auto ambient=vsg::AmbientLight::create();ambient->color={.4f,.4f,.4f};
        auto sun=vsg::DirectionalLight::create();sun->direction={0,0,-1};sun->color={.1f,.1f,.1f};
        sun->shadowSettings=vsg::HardShadows::create(3);
        ImmediateEffectDraw draw;draw.identity="shadow-view-routing";draw.mesh=*quad();
        draw.material.sourceIdentity="shadow-view-routing-material";draw.material.cullMode=CullMode::None;
        draw.material.ambient={1,1,1,1};draw.material.diffuse={1,1,1,1};draw.material.specular={0,0,0,0};
        draw.material.vertexColorMode=VertexColorMode::Ignore;
        auto graph=RenderVsg::realizeImmediateEffectDraw(draw,resolver(),world.shared);
        require(graph.valid(),graph.diagnostic);world.root->children={ambient,sun,graph.root};
        // World drawables must be compiled for every live view, including
        // generated depth passes; the ordinary Fixture::compile is UI/view-only.
        const auto compiled=RenderVsg::compileForViewer(*world.viewer,world.root);
        require(bool(compiled),"shadowed world compile: "+compiled.message);
        require(RenderVsg::graphicsPipelinesRealizedForView(*graph.root,*world.view),
            "world pipeline missing before recording");
        for (const auto& shadow:world.state->shadowMaps)
            require(RenderVsg::graphicsPipelinesRealizedForView(*graph.root,*shadow.view),
                "shadow pipeline missing before recording");
        require(world.state->shadowMaps.size()==3,"three actual VSG cascades were not compiled");
        for (unsigned frame=0;frame<3;++frame)
        {
            const auto result=pixel(world.render());
            require(result.r>100 && result.g>100 && result.b>100,"shadowed world lost ambient illumination");
            require((*world.state->lightData)[0].x==1.f && (*world.state->lightData)[0].y==1.f,
                "main shadowed view no longer packs ambient/sun data");
            unsigned recorded=0;
            for (std::size_t i=0;i<world.state->shadowMaps.size();++i)
            {
                const auto& shadow=world.state->shadowMaps[i];
                require(shadow.view->features==vsg::INHERIT_VIEWPOINT,"generated shadow features changed");
                const auto state=shadow.view->viewDependentState.cast<RenderVsg::OpenMwViewDependentState>();
                require(state && state->lightData && state->lightData->size()==1,
                    "depth-only shadow buffers were enlarged to hide the routing defect");
                require((*state->lightData)[0]==vsg::vec4(0,0,0,0),"shadow camera received color-light counts");
                if (world.state->preRenderSwitch->children[i].mask!=vsg::MASK_OFF)
                {
                    ++recorded;
                    require(!state->ambientLights.empty() && !state->directionalLights.empty(),
                        "shadow regression never encountered parent light nodes");
                }
            }
            require(recorded>0,"shadow maps were silently disabled rather than repaired");
        }
        // The functional black-preview repair must survive the tighter feature gate.
        checkUnshadowedLighting(device);
        std::cout<<"PASS shadow view feature routing: three generated cascades, repeated frames, lit world and preview, unchanged depth buffer sizes\n";
    }
    void checkSunSpecular(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device);
        auto sun=vsg::DirectionalLight::create();sun->direction={0,0,-1};sun->color={1,1,1};sun->intensity=1.f;
        ImmediateEffectDraw draw;draw.identity="sun-specular";draw.mesh=*quad();
        draw.material.sourceIdentity="sun-specular-material";draw.material.cullMode=CullMode::None;
        draw.material.diffuse={0,0,0,1};draw.material.ambient={0,0,0,1};
        draw.material.specular={1,1,1,1};draw.material.shininess=1.f;draw.material.vertexColorMode=VertexColorMode::Ignore;
        auto realized=RenderVsg::realizeImmediateEffectDraw(draw,resolver(),f.shared);
        require(realized.valid(),realized.diagnostic);f.root->children={sun,realized.root};require(f.compile(f.root),"sun-specular compile");
        f.environment.sunSpecular={0,0,0,0};
        auto disabled=pixel(f.render());close(disabled.r,0,"zero sun specular still produces a highlight");
        close(disabled.g,0,"zero sun specular green");close(disabled.b,0,"zero sun specular blue");
        f.environment.sunSpecular={.1f,.3f,.6f,1.f};auto coloured=pixel(f.render());
        close(coloured.r,encoded(.1f),"independent sun specular red");
        close(coloured.g,encoded(.3f),"independent sun specular green");
        close(coloured.b,encoded(.6f),"independent sun specular blue");
        std::cout<<"PASS sun-specular pixels: zero disables highlights, independent RGB updates without material recompilation\n";
    }
    void checkWaterProbe(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device,vsg::RECORD_ALL,0,RenderTargetFormat::Rgba16Float);
        f.target.setClearValues({{.25f,.5f,.75f,1.f}},{0.f,0});
        const auto sample = [&](std::uint64_t id) {
            RenderVsg::WaterInputProbe probe(device,f.target,FrameId{id},"test-refraction",true);
            require(!probe.read(FrameId{id-1}),"probe exposed staging before completion");
            f.commands->children.resize(1); f.commands->addChild(probe.commands);
            f.camera.update(f.frame); f.state->setEnvironment(f.environment,f.frame.current.projection);
            require(f.viewer->advanceToNextFrame(double(id)),"probe advance"); f.viewer->update();
            for(auto& task:f.viewer->recordAndSubmitTasks)
            {
                for(auto& graph:task->commandGraphs) graph->reset();
                require(task->submit(vsg::ref_ptr<vsg::FrameStamp>(f.viewer->getFrameStamp()))==VK_SUCCESS,"probe submit");
            }
            // Fixture only: the game uses its normal completion tracker.
            require(vkDeviceWaitIdle(device->vk())==VK_SUCCESS,"probe fixture completion");
            auto result=probe.read(FrameId{id}); require(result.has_value(),"completed probe absent");
            return *result;
        };
        auto clear=sample(2);
        require(clear.colorNonclear==0 && clear.depthNonclear==0 && clear.nonfinite==0,"clear-only probe misclassified");
        ImmediateEffectDraw d;d.identity="probe-geometry";d.mesh=*quad();d.material.diffuse={.8f,.1f,.2f,1};
        d.material.unlit=true; d.material.cullMode=CullMode::None;
        auto draw=RenderVsg::realizeImmediateEffectDraw(d,resolver(),f.shared);require(draw.valid(),draw.diagnostic);
        f.root->addChild(draw.root);require(f.compile(f.root),"probe geometry compile");
        auto scene=sample(3);
        require(scene.colorNonclear>0 && scene.depthNonclear>0 && scene.nonfinite==0 && scene.checksum!=clear.checksum,
            "probe failed to distinguish real geometry/depth from clear");
        std::cout<<"PASS water input probe: RGBA16F+D32 production copies, clear vs geometry, fence gate, linear/depth decode\n";
    }

    void checkEnvironmentMaterial(vsg::ref_ptr<vsg::Device> device)
    {
        // Synthetic equivalent of the office bottle: base + authored sphere
        // environment + legacy bump, then normal/gloss/specular concurrently.
        // No mod files are bundled. Constant image colours make this an oracle,
        // not a screenshot-looks-plausible check.
        Fixture f(device); auto ambient=vsg::AmbientLight::create(); ambient->intensity=.25f;
        ImmediateEffectDraw d; d.identity="synthetic-kurst"; d.mesh=*quad();
        d.material.sourceIdentity="sphere-bump"; d.material.cullMode=CullMode::None;
        d.material.diffuse={0,0,0,1}; d.material.ambient={0,0,0,1}; d.material.specular={0,0,0,1};
        d.material.environmentMapMode=EnvironmentMapMode::SphereMap;
        d.material.environmentMapStrength=.4f; d.material.environmentMapColor={1,.5f,.25f,1};
        d.textures={texture("sphere-white",{255,255,255,255},TextureRole::Environment)};
        auto render=[&] {
            const auto r=RenderVsg::realizeImmediateEffectDraw(d,resolver(),f.shared);
            require(r.valid(),r.diagnostic); f.root->children={ambient,r.root};
            require(f.compile(f.root),"environment pipeline compile"); return pixel(f.render());
        };
        auto post=render(); close(post.r,encoded(.4f),"sphere post-light red");
        close(post.g,encoded(.2f),"sphere post-light green");
        d.material.environmentMapPreLight=true; auto pre=render();
        require(pre.r+40<post.r,"pre-light environment ordering lost");
        d.material.environmentMapPreLight=false;
        d.material.bumpParametersEnabled=true;
        d.material.bumpMapMatrix={.25f,.1f,-.2f,.5f}; d.material.environmentMapLumaBias={0.f,.25f};
        auto bump=texture("bump-data",{32,64,255,255},TextureRole::Bump);
        bump.binding.colorSpace=TextureColorSpace::Data; bump.binding.formatClass=TextureFormatClass::Height;
        d.textures.push_back(bump);
        auto bumped=render(); close(bumped.r,encoded(.1f),"authored bump luminance");
        auto gloss=texture("gloss-data",{128,128,128,255},TextureRole::Gloss); gloss.binding.colorSpace=TextureColorSpace::Data;
        d.textures.push_back(gloss);
        d.textures.push_back(texture("normal-data",{128,128,255,255},TextureRole::Normal));
        d.textures.push_back(texture("specular-black",{0,0,0,255},TextureRole::Specular));
        auto combined=render(); close(combined.r,encoded(.1f*128.f/255.f),"sphere+bump+gloss+normal+specular");
        // Use the actual evaluated OSG capture, neutral owned publication and
        // production shader together, not just hand-built neutral records.
        auto captured = RepairFixtures::captureAuthoredMaterial();
        // Persistent capture intentionally moves mesh data into an immutable
        // owner. Inspect the semantic accessor in both modes, not its scratch buffer.
        require(captured.textures.size()==6 && captured.meshData().texCoordSets.size()==1,
            "combined captured material lost stages or did not preserve aliased UVs");
        require(!std::getenv("OPENMW_V4_PERSISTENT_CAPTURE") || bool(captured.meshSnapshot),
            "combined material did not exercise persistent mesh ownership");
        for (auto& stage : captured.textures)
        {
            // Deterministic decoded image fixture; all roles, bindings and VFS
            // identities above come from the real capture function unchanged.
            vsg::ubvec4 rgba{255,255,255,255};
            if (stage.binding.role==TextureRole::Normal) rgba=vsg::ubvec4{128,128,255,255};
            if (stage.binding.role==TextureRole::Gloss) rgba=vsg::ubvec4{128,128,128,255};
            if (stage.binding.role==TextureRole::Specular) rgba=vsg::ubvec4{0,0,0,255};
            auto data=std::make_shared<TexturePixels>(); data->rgba8={rgba.r,rgba.g,rgba.b,rgba.a};
            stage.texture.pixels=data;
        }
        RenderWorld captureWorld; SingleViewFrameProducer captureProducer; SingleViewFrameInput captureInput;
        const std::array captureDraws{captured};
        captureInput.ownedImmediateEffects=std::make_shared<const OwnedImmediateEffects>(captureDraws);
        captureInput.renderExtent=captureInput.outputExtent={128,128};
        const auto published=captureProducer.prepare(captureWorld,captureInput);
        require(published && published->valid(),"captured authored material failed neutral publication");
        const auto capturedGraph=RenderVsg::realizeImmediateEffectDraw(published->immediateEffectDraws().front(),resolver(),f.shared);
        require(capturedGraph.valid(),capturedGraph.diagnostic); f.root->children={ambient,capturedGraph.root};
        require(f.compile(f.root),"captured authored material production shader compile");
        close(pixel(f.render()).r,combined.r,"capture to owned frame to authored material pixels");
        // The descriptor array remains exclusive to the exact enchanted mode.
        d.material.environmentMapMode=EnvironmentMapMode::EnchantedSequence;
        for(unsigned i=1;i<32;++i)
            d.textures.push_back(texture("caustic-"+std::to_string(i),{255,255,255,255},TextureRole::Environment));
        close(render().r,combined.r,"enchanted bump path changed");
        d.textures.pop_back();
        require(!RenderVsg::realizeImmediateEffectDraw(d,resolver(),f.shared).valid(),"31-frame malformed sequence accepted");
        std::cout<<"PASS material sphere/bump pixels: post/pre-light ordering, combined normal/specular/gloss, exact caustic array and malformed rejection\n";
    }
    float reverseDepth(float distance,float near,float far) {return near*(far/distance-1)/(far-near);}
    void checkWaterOptics(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device); f.frame.current.worldPosition={0,0,100};
        f.frame.current.view=glm::lookAtRH(glm::vec3(0,0,100),glm::vec3(0,1,100),glm::vec3(0,0,1));
        auto refraction=RenderVsg::createOffscreenRenderTarget(device,{128,128},RenderTargetFormat::Rgba16Float,
            RenderTargetFormat::Depth32Float,true);
        require(bool(refraction),"sampled depth target");
        require((refraction.depth->image->usage&VK_IMAGE_USAGE_SAMPLED_BIT)!=0,"depth image missing sampled usage");
        f.commands->children.insert(f.commands->children.begin(),refraction.renderGraph);
        auto reflectionData=vsg::vec4Array2D::create(1,1,vsg::vec4(0,0,0,1),vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
        auto reflection=vsg::ImageView::create(vsg::Image::create(reflectionData));
        auto normal=vsg::vec4Array2D::create(1,1,vsg::vec4(.5f,.5f,1,1),vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));
        auto water=RenderVsg::WaterSurface::create(reflection,refraction.color,normal,refraction.depth);
        f.root->addChild(water.node());f.environment.waterEnabled=true;f.environment.ambient={.2f,.2f,.2f,1};
        f.environment.sunSpecular={0,0,0,0};
        auto c=f.viewer->compile();require(bool(c),"depth optics compile "+c.message);
        auto render=[&](float depth){refraction.setClearValues({{1,1,1,1}},
            {reverseDepth(depth,.1f,10000.f),0});water.update(f.environment,f.frame,0);return f.render();};
        auto shallow=pixel(render(270.f),64,112);auto deep=pixel(render(6000.f),64,112);
        std::cerr<<"WATER shallow "<<unsigned(shallow.r)<<","<<unsigned(shallow.g)<<","<<unsigned(shallow.b)<<" deep "<<unsigned(deep.r)<<","<<unsigned(deep.g)<<","<<unsigned(deep.b)<<"\n";
        require(shallow.r>deep.r+90,"native depth absorption did not distinguish shore and deep water");
        require(deep.r<90 && deep.b<100,"deep refraction remained white");
        f.environment.fogEnabled=true;f.environment.fogStart=0;f.environment.fogEnd=100;f.environment.fogColor={0,0,1,1};
        auto fog=pixel(render(6000.f),64,112);require(fog.b>250 && fog.r<4,"water ignores native fog distance");
        std::cout<<"PASS water optics pixels: stored refraction depth, shallow/deep absorption, native fog; shallow="<<unsigned(shallow.r)<<" deep="<<unsigned(deep.r)<<'\n';
    }
    void checkNormalMapping(vsg::ref_ptr<vsg::Device> device)
    {
        Fixture f(device);auto light=vsg::DirectionalLight::create();light->direction={0,0,-1};
        ImmediateEffectDraw draw;draw.identity="normal-quad";draw.mesh=*quad();draw.material.sourceIdentity="normal-mat";
        draw.material.cullMode=CullMode::Back;draw.material.diffuse={1,1,1,1};draw.material.ambient={0,0,0,1};
        draw.material.specular={0,0,0,1};draw.material.vertexColorMode=VertexColorMode::Ignore;
        draw.textures={texture("rg-normal",{128,128,0,255},TextureRole::Normal)};
        auto rg=vsg::ubvec2Array2D::create(1,1,vsg::ubvec2(128,128),vsg::Data::Properties(VK_FORMAT_R8G8_UNORM));
        vsg::ref_ptr<vsg::Data> normal=rg;
        RenderVsg::StaticTextureResolver resolve=[&](const auto&,const auto&){return normal;};
        auto render=[&]{auto result=RenderVsg::realizeImmediateEffectDraw(draw,resolve,f.shared);require(result.valid(),result.diagnostic);
            f.root->children={light,result.root};require(f.compile(f.root),"normal-map compile");return pixel(f.render());};
        auto flat=render();require(flat.r>245 && flat.g>245 && flat.b>245,"object RG normals did not reconstruct Z");
        auto tilted=vsg::vec4Array2D::create(1,1,vsg::vec4(.8f,.5f,.9f,1),vsg::Data::Properties(VK_FORMAT_R32G32B32A32_SFLOAT));normal=tilted;
        light->direction={-1,0,0};auto positive=render();require(positive.r>150,"positive tangent normal faces away from positive-U light");
        for (auto& uv:draw.mesh.texCoordSets[0]) uv.x=1-uv.x;
        auto mirrored=render();require(mirrored.r<5,"mirrored U chart did not reverse tangent normal");
        for (auto& uv:draw.mesh.texCoordSets[0]) uv={0,0};light->direction={0,0,-1};
        require(render().r>245,"degenerate UV chart propagated invalid lighting");
        std::cout<<"PASS material pixels: object two-channel Z reconstruction, signed UV Jacobian, mirrored charts, degenerate UV safety\n";
    }
}
#include "water-pixel-tests.hpp"

int main(int argc,char** argv)
{
    std::cout << std::unitbuf;
    try
    {
        vsg::Names layers;
        if (std::getenv("OPENMW_V4_PIXEL_VALIDATION")) layers.push_back("VK_LAYER_KHRONOS_validation");
        auto instance=vsg::Instance::create(vsg::Names{},layers,VK_API_VERSION_1_1);
        auto physical=instance->getPhysicalDevices();require(!physical.empty(),"no Vulkan device");
        auto selected=physical.front(); auto features=vsg::DeviceFeatures::create();features->get().samplerAnisotropy=selected->getFeatures().samplerAnisotropy;
        vsg::Names extensions;if(selected->supportsDeviceExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        auto device=vsg::Device::create(selected,vsg::QueueSettings{{selected->getQueueFamily(VK_QUEUE_GRAPHICS_BIT),{1.f}}},vsg::Names{},extensions,features);
        std::cout<<"DEVICE "<<selected->getProperties().deviceName<<'\n';
        std::string mode=argc>1?argv[1]:"all";
        require(mode=="all" || mode=="sky" || mode=="preview" || mode=="water" || mode=="normal" || mode=="baseline" || mode=="lighting" || mode=="specular" || mode=="shadow-light-routing" || mode=="environment" || mode=="water-probe", "unknown pixel test mode");
        if(mode=="all") {checkPersistentDrawScene(device);checkNativeVisibilityRouting(device);checkEffectFrustum(device);checkNativePostProcess(device);}
        if(mode=="all"||mode=="water-probe")checkWaterProbe(device);
        if(mode=="all"||mode=="environment")checkEnvironmentMaterial(device);
        if(mode=="all"||mode=="baseline")checkWaterPixels(device);
        if(mode=="all"||mode=="sky")checkSky(device);
        if(mode=="all"||mode=="preview")checkPreview(device);
        if(mode=="all"||mode=="lighting")checkUnshadowedLighting(device);
        if(mode=="all"||mode=="specular")checkSunSpecular(device);
        if(mode=="all"||mode=="shadow-light-routing")checkShadowViewFeatures(device);
        if(mode=="all"||mode=="water")checkWaterOptics(device);
        if(mode=="all"||mode=="normal")checkNormalMapping(device);
        return 0;
    }
    catch(const vsg::Exception& error){std::cerr<<"VSG: "<<error.message<<" result="<<error.result<<'\n';}
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';}
    return 1;
}
