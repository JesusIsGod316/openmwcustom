// Headless pixel tests of the production pipelines. No game window, save, mod
// configuration or benchmark is involved. A Vulkan implementation is required.
#include <components/render/backend/vsg/nativesky.hpp>
#include <components/render/backend/vsg/isolatedscene.hpp>
#include <components/render/backend/vsg/offscreenrendertarget.hpp>
#include <components/render/backend/vsg/openmwviewdependentstate.hpp>
#include <components/render/backend/vsg/statictexturedecode.hpp>
#include <components/render/backend/vsg/legacymaterialshader.hpp>
#include <components/render/backend/vsg/uipipeline.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <components/render/backend/vsg/watersurface.hpp>
#include <components/rendercore/frameproducer.hpp>
#include <vsg/all.h>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>

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
        vsg::ref_ptr<vsg::SharedObjects> shared=vsg::SharedObjects::create();
        std::uint64_t tick=0;
        explicit Fixture(vsg::ref_ptr<vsg::Device> input) : device(input)
        {
            frame.extent={128,128}; frame.current.projection.nearPlane=.1; frame.current.projection.farPlane=10000.;
            frame.current.projection.matrix=glm::perspectiveRH_ZO(glm::radians(60.f),1.f,10000.f,.1f);
            frame.current.projection.matrix[1][1]*=-1.f;
            camera=RenderVsg::FrameCameraObjects::create(frame);
            target=RenderVsg::createOffscreenRenderTarget(device,{128,128},RenderTargetFormat::Rgba8Srgb,RenderTargetFormat::Depth32Float);
            require(bool(target),"headless target");
            require(target.renderGraph->clearValues.size()==2 && target.renderGraph->clearValues[1].depthStencil.depth==0.f,
                "default reverse-depth clear missing or typed as colour");
            view=vsg::View::create(camera.camera,root);
            state=RenderVsg::OpenMwViewDependentState::create(view.get());
            state->shaderSet=RenderVsg::createLegacyCompatibilityShaderSet();
            view->viewDependentState=state;
            view->bins=RenderVsg::createStaticConformanceBins();
            view->bins.push_back(vsg::Bin::create(RenderVsg::UiOverlayBinNumber,vsg::Bin::NO_SORT));
            target.renderGraph->addChild(view);
            commands=vsg::CommandGraph::create(device,device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT));
            commands->addChild(target.renderGraph);
            viewer->assignRecordAndSubmitTaskAndPresentation({commands});
            auto result=viewer->compile();
            require(bool(result),"initial target compilation "+result.message);
        }
        bool compile(vsg::ref_ptr<vsg::Node> node)
        {
            auto result=RenderVsg::compileForViewerView(*viewer,*view,node);
            if (!result) std::cerr<<"compile: "<<result.message<<" code="<<result.result<<'\n';
            return bool(result);
        }
        Pixels render()
        {
            camera.update(frame);
            state->setEnvironment(environment,frame.current.projection);
            require(viewer->advanceToNextFrame(double(++tick)),"advance frame");
            viewer->update();
            for (auto& task:viewer->recordAndSubmitTasks)
            {
                for (auto& graph:task->commandGraphs) graph->reset();
                require(task->submit(vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp()))==VK_SUCCESS,"pixel submit");
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
        require(mode=="all" || mode=="sky" || mode=="preview" || mode=="water" || mode=="normal" || mode=="baseline", "unknown pixel test mode");
        if(mode=="all"||mode=="baseline")checkWaterPixels(device);
        if(mode=="all"||mode=="sky")checkSky(device);
        if(mode=="all"||mode=="preview")checkPreview(device);
        if(mode=="all"||mode=="water")checkWaterOptics(device);
        if(mode=="all"||mode=="normal")checkNormalMapping(device);
        return 0;
    }
    catch(const vsg::Exception& error){std::cerr<<"VSG: "<<error.message<<" result="<<error.result<<'\n';}
    catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';}
    return 1;
}
