// Headless tests of the production OMWFX executor. No user files or game state.
#include <components/render/backend/vsg/omwfx.hpp>
#include <components/render/backend/vsg/fximagestate.hpp>
#include <components/render/backend/vsg/nativepostprocess.hpp>
#include <components/render/backend/vsg/offscreenrendertarget.hpp>
#include <components/render/backend/vsg/framecamera.hpp>
#include <components/render/backend/vsg/vsgsubmission.hpp>
#include <components/fx/stateupdater.hpp>
#include <components/resource/imagemanager.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/vfs/filesystemarchive.hpp>
#include <components/vfs/manager.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL_opengl_glext.h>
#include <osg/Texture1D>
#include <osg/Texture3D>
#include <cmath>
#include <cstring>
#include <iostream>

namespace
{
    void require(bool value, const std::string& message)
    { if (!value) throw std::runtime_error(message); }
    constexpr unsigned Size = 32;
    constexpr const char* Vertex = R"(#version 450
layout(location=0) out vec2 uv;
void main(){ uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);gl_Position=vec4(uv*2-1,0,1); }
)";
    Fx::NativePass pass(std::string body, std::vector<std::string> samplers, std::string target = {})
    {
        Fx::NativePass result;
        result.name = "pixel-test";
        result.target = std::move(target);
        result.shaders.vertex = Vertex;
        result.shaders.fragment = "#version 450\nlayout(location=0) in vec2 uv;layout(location=0) out vec4 color;\n" + body;
        result.shaders.samplers = std::move(samplers);
        return result;
    }
    int encoded(float value)
    { return int(std::round(255.f * (value <= .0031308f ? value * 12.92f : 1.055f * std::pow(value, 1.f/2.4f)-.055f))); }
    void close(int actual, float expected, const std::string& name)
    { require(std::abs(actual-encoded(expected)) <= 3, name+": "+std::to_string(actual)+" expected "+std::to_string(encoded(expected))); }

    struct Fixture
    {
        vsg::ref_ptr<vsg::Device> device;
        vsg::ref_ptr<vsg::Viewer> viewer = vsg::Viewer::create();
        RenderVsg::OffscreenRenderTarget scene, finalTarget;
        RenderCore::FrameView frameView;
        RenderVsg::FrameCameraObjects camera;
        vsg::ref_ptr<vsg::View> outputView;
        vsg::ref_ptr<vsg::Group> outputRoot = vsg::Group::create();
        vsg::ref_ptr<vsg::CommandGraph> commands;
        std::unique_ptr<RenderVsg::OmwFxRuntime> effects;
        unsigned tick = 0;
        Fixture(vsg::ref_ptr<vsg::Device> input, const Fx::NativeFrame& frame) : device(input)
        {
            std::cout << "TRACE create targets\n";
            scene = RenderVsg::createOffscreenRenderTarget(device, {Size,Size}, RenderCore::RenderTargetFormat::Rgba16Float,
                RenderCore::RenderTargetFormat::Depth32Float, true);
            finalTarget = RenderVsg::createOffscreenRenderTarget(device, {Size,Size}, RenderCore::RenderTargetFormat::Rgba8Srgb,
                RenderCore::RenderTargetFormat::Depth32Float);
            scene.setClearValues({{.25f,.5f,.75f,1}}, {.25f,0});
            frameView.extent = {Size,Size};
            camera = RenderVsg::FrameCameraObjects::create(frameView);
            outputView = vsg::View::create(camera.camera, outputRoot, static_cast<vsg::ViewFeatures>(0));
            finalTarget.renderGraph->addChild(outputView);
            commands = vsg::CommandGraph::create(device, device->getPhysicalDevice()->getQueueFamily(VK_QUEUE_GRAPHICS_BIT));
            commands->addChild(scene.renderGraph);
            commands->addChild(finalTarget.renderGraph);
            viewer->assignRecordAndSubmitTaskAndPresentation({commands});
            auto hints = vsg::ResourceHints::create();
            std::cout << "TRACE compile fixture\n";
            require(bool(viewer->compile(hints)), "fixture compile");
            viewer->compileManager = RenderVsg::ViewCompileManager::create(*viewer, hints);
            std::cout << "TRACE create native executor\n";
            auto lights = vsg::ubyteArray::create(1936, std::uint8_t(0));
            effects = std::make_unique<RenderVsg::OmwFxRuntime>(*viewer, device, scene.color, scene.depth,
                RenderCore::Extent2D{Size,Size}, frame, lights);
            auto present = RenderVsg::createNativePostProcess(effects->output, scene.depth, RenderVsg::NativePostProcessMode::Copy);
            std::cout << "TRACE compile presentation\n";
            require(bool(RenderVsg::compileForViewerView(*viewer, *outputView, present)), "output compile");
            outputRoot->addChild(present);
            commands->children.insert(commands->children.begin()+1, effects->commands);
        }
        ~Fixture() { viewer->deviceWaitIdle(); effects.reset(); }
        std::vector<unsigned char> render(const Fx::NativeFrame& frame)
        {
            std::cout << "TRACE render " << tick << '\n';
            effects->update(frame, frameView, tick, .016);
            require(viewer->advanceToNextFrame(++tick*.016), "advance");
            viewer->update();
            for (auto& task : viewer->recordAndSubmitTasks)
            {
                for (auto& graph : task->commandGraphs) graph->reset();
                require(RenderVsg::submitTaskChecked(*task, vsg::ref_ptr<vsg::FrameStamp>(viewer->getFrameStamp())) == VK_SUCCESS, "submit");
            }
            viewer->deviceWaitIdle(); // Test readback, never the normal rendering path.
            auto buffer = vsg::createBufferAndMemory(device, Size*Size*4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                VK_SHARING_MODE_EXCLUSIVE, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            auto pool = vsg::CommandPool::create(device, commands->queueFamily);
            auto fence = vsg::Fence::create(device);
            auto copy = vsg::CopyImageToBuffer::create();
            copy->srcImage = finalTarget.color->image;
            copy->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            copy->dstBuffer = buffer;
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; region.imageExtent = {Size,Size,1};
            copy->regions.push_back(region);
            require(vsg::submitCommandsToQueue(pool, fence, 10000000000ull, device->getQueue(commands->queueFamily),
                [&](vsg::CommandBuffer& command)
                {
                    RenderVsg::fxImageBarrier(command,*copy->srcImage,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                    copy->record(command);
                    RenderVsg::fxImageBarrier(command,*copy->srcImage,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,RenderVsg::FxSampleStages);
                }) == VK_SUCCESS, "readback");
            auto mapped = vsg::MappedData<vsg::ubyteArray>::create(buffer->getDeviceMemory(device->deviceID),
                buffer->getMemoryOffset(device->deviceID),0,vsg::Data::Properties{},Size*Size*4);
            std::vector<unsigned char> bytes(Size*Size*4);
            std::memcpy(bytes.data(),mapped->dataPointer(),bytes.size());
            return bytes;
        }
    };

    void checkImportedTextures(vsg::ref_ptr<vsg::Device> device)
    {
        auto chain = std::make_shared<Fx::NativeChain>();
        Fx::NativeTechnique technique;
        technique.name = "compressed-texture-regression";
        const auto compressed = [&](const char* name, unsigned format, std::initializer_list<unsigned char> block)
        {
            auto image = new osg::Image;
            auto bytes = new unsigned char[block.size()];
            std::copy(block.begin(), block.end(), bytes);
            image->setImage(4, 4, 1, format, format, GL_UNSIGNED_BYTE, bytes, osg::Image::USE_NEW_DELETE, 1);
            auto texture = new osg::Texture2D(image);
            texture->setName(name);
            texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::NEAREST);
            texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::NEAREST);
            technique.textures.push_back(texture);
        };
        compressed("dxt1", GL_COMPRESSED_RGB_S3TC_DXT1_EXT, {0,248,0,0,0,0,0,0});
        compressed("dxt3", GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
            {136,136,136,136,136,136,136,136,224,7,0,0,0,0,0,0});
        compressed("dxt5", GL_COMPRESSED_RGBA_S3TC_DXT5_EXT,
            {128,0,0,0,0,0,0,0,31,0,0,0,0,0,0,0});
        technique.passes.push_back(pass(R"(
layout(set=0,binding=3) uniform sampler2D dxt1;
layout(set=0,binding=4) uniform sampler2D dxt3;
layout(set=0,binding=5) uniform sampler2D dxt5;
void main(){ color=vec4(texture(dxt1,uv).r,texture(dxt3,uv).a,texture(dxt5,uv).a,1); }
)", {"dxt1", "dxt3", "dxt5"}));
        chain->techniques.push_back(technique);
        Fx::NativeFrame frame;
        frame.chain = chain; frame.enabled = true; frame.state.resize(1024,0);
        frame.parameters.assign(1,std::vector<char>(16,0));
        {
            Fixture fixture(device, frame);
            auto pixels = fixture.render(frame);
            for (unsigned index : {0u,(16*Size+16)*4,(Size*Size-1)*4})
            {
                close(pixels[index],1,"DXT1 color upload");
                close(pixels[index+1],8.f/15.f,"DXT3 explicit alpha upload");
                close(pixels[index+2],128.f/255.f,"DXT5 interpolated alpha upload");
            }
        }
        auto image = new osg::Image;
        image->allocateImage(2,2,2,GL_RGBA,GL_FLOAT);
        for (unsigned i=0; i<8; ++i)
        {
            const float color[] = {.2f,.4f,.6f,1};
            std::memcpy(image->data()+i*sizeof(color),color,sizeof(color));
        }
        auto volume = new osg::Texture3D(image);
        volume->setName("volume");
        volume->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        auto lineImage = new osg::Image;
        lineImage->allocateImage(2,1,1,GL_RGBA,GL_UNSIGNED_BYTE);
        std::fill(lineImage->data(),lineImage->data()+8,static_cast<unsigned char>(51));
        auto line = new osg::Texture1D(lineImage);
        line->setName("line");
        line->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR);
        auto& plan = chain->techniques.front();
        plan.textures = {volume,line};
        plan.passes = {pass(R"(
layout(set=0,binding=3) uniform sampler3D volume;
layout(set=0,binding=4) uniform sampler1D line;
void main(){ color=vec4(texture(line,.5).r,texture(volume,vec3(uv,.5)).gb,1); }
)", {"volume","line"})};
        Fixture fixture(device,frame);
        auto pixels = fixture.render(frame);
        for (unsigned index : {0u,(16*Size+16)*4,(Size*Size-1)*4})
        {
            close(pixels[index],.2f,"1D image upload");
            close(pixels[index+1],.4f,"3D float green upload");
            close(pixels[index+2],.6f,"3D float blue upload");
        }
    }

    void checkCameraApi(vsg::ref_ptr<vsg::Device> device, const char* dataPath)
    {
        VFS::Manager vfs;
        vfs.addArchive(std::make_unique<VFS::FileSystemArchive>(dataPath));
        vfs.buildIndex();
        Resource::ImageManager images(&vfs,0.0);
        Fx::Technique source(vfs,images,Fx::Technique::makeFileName("native-camera-probe"),
            "native-camera-probe",Size,Size,true,false);
        require(source.compile(),"camera probe parse: "+source.getLastError());
        auto chain = std::make_shared<Fx::NativeChain>();
        chain->techniques.push_back(Fx::makeNativeTechnique(source));
        Fx::NativeFrame frame;
        frame.chain=chain; frame.enabled=true;
        osg::ref_ptr<Fx::StateUpdater> state = new Fx::StateUpdater(false);
        frame.state=state->snapshot();
        frame.parameters.push_back(Fx::packNativeParameters(chain->techniques.front(),source));
        Fixture fixture(device,frame);
        fixture.frameView.current.projection.nearPlane=1;
        fixture.frameView.current.projection.farPlane=100;
        fixture.frameView.current.projection.matrix=glm::perspectiveRH_ZO(glm::radians(60.f),1.f,100.f,1.f);
        fixture.frameView.current.projection.matrix[1][1]*=-1;
        for (float angle : {0.f,.4f,-.6f})
        {
            const glm::vec3 eye(3200,-2100,600);
            const glm::vec3 forward(std::sin(angle),std::cos(angle),0);
            fixture.frameView.current.view=glm::lookAtRH(eye,eye+forward,glm::vec3(0,0,1));
            fixture.frameView.previous=fixture.frameView.current;
            auto pixels=fixture.render(frame);
            for (unsigned index : {0u,(16*Size+16)*4,(Size*Size-1)*4})
            {
                close(pixels[index],.25f,"API eye position follows native view");
                close(pixels[index+1],.5f,"API eye direction follows native view");
                close(pixels[index+2],.75f,"API world/depth reconstruction agrees");
            }
        }
    }
}

int main(int argc, char** argv)
{
    try
    {
        std::cout << std::unitbuf;
        require(argc==3,"Usage: openmw-vulkan-fx-render-tests defaults.bin test-data-directory");
        Settings::SettingsFileParser parser;
        parser.loadSettingsFile(argv[1],Settings::Manager::mDefaultSettings,true,false);
        Settings::StaticValues::initDefaults();
        Settings::StaticValues::init();
        auto instance = vsg::Instance::create(vsg::Names{}, vsg::Names{}, VK_API_VERSION_1_1);
        auto devices = instance->getPhysicalDevices(); require(!devices.empty(), "Vulkan device unavailable");
        auto physical = devices.front();
        auto device = vsg::Device::create(physical, vsg::QueueSettings{{physical->getQueueFamily(VK_QUEUE_GRAPHICS_BIT),{1.f}}},
            vsg::Names{},vsg::Names{},vsg::DeviceFeatures::create());
        std::cout << "DEVICE " << physical->getProperties().deviceName << '\n';
        auto chain = std::make_shared<Fx::NativeChain>();
        Fx::NativeTechnique first; first.name = "multiply";
        first.passes.push_back(pass(R"(
layout(set=0,binding=2,std140) uniform Params { float factor; };
layout(set=0,binding=3) uniform sampler2D scene;
void main(){ color=texture(scene,uv)*factor; }
)", {"omw_SamplerLastShader"}));
        chain->techniques.push_back(first);
        Fx::NativeTechnique second; second.name = "history";
        Fx::Types::RenderTarget history;
        history.mTarget->setInternalFormat(GL_RGBA32F);
        history.mTarget->setFilter(osg::Texture::MIN_FILTER,osg::Texture::LINEAR_MIPMAP_LINEAR);
        history.mMipMap = true;
        second.targets.emplace("history",history);
        second.passes.push_back(pass(R"(
layout(set=0,binding=3) uniform sampler2D history;
void main(){ color=vec4(textureLod(history,uv,2).r*.5+.1,0,0,1); }
)", {"history"}, "history"));
        second.passes.push_back(pass(R"(
layout(set=0,binding=3) uniform sampler2D previous;
layout(set=0,binding=4) uniform sampler2D history;
layout(set=0,binding=5) uniform sampler2D depth;
void main(){ color=vec4(texture(previous,uv).r,textureLod(history,uv,2).r,texture(depth,uv).r,1); }
)", {"omw_SamplerLastShader","history","omw_SamplerDepth"}));
        chain->techniques.push_back(second);
        Fx::NativeFrame frame; frame.chain=chain; frame.enabled=true; frame.state.resize(1024,0);
        frame.parameters.assign(2,std::vector<char>(16,0));
        float factor=.5f; std::memcpy(frame.parameters[0].data(),&factor,sizeof(factor));
        Fixture fixture(device,frame);
        for (unsigned iteration=0; iteration<5; ++iteration)
        {
            if (iteration==2) { factor=1; std::memcpy(frame.parameters[0].data(),&factor,sizeof(factor)); }
            auto pixels=fixture.render(frame);
            for (unsigned index : {0u,(16*Size+16)*4,(Size*Size-1)*4})
            {
                close(pixels[index],.25f*factor,"last-shader/live parameter");
                close(pixels[index+1],.2f*(1-std::pow(.5f,float(iteration+1))),"persistent history and mipmaps");
                close(pixels[index+2],.25f,"sampled depth");
            }
        }
        std::cout << "PASS native OMWFX: chained passes, full viewport, live parameters, depth, temporal targets, mipmaps (45 pixel checks)\n";
        checkImportedTextures(device);
        std::cout << "PASS native OMWFX imported images: DXT1, DXT3, DXT5, 1D, 3D (18 pixel checks)\n";
        checkCameraApi(device,argv[2]);
        std::cout << "PASS parsed OMWFX camera API: translated/rotated view, world/depth reconstruction (27 pixel checks)\n";
        return 0;
    }
    catch (const vsg::Exception& error) { std::cerr << "Vulkan: " << error.message << '\n'; return 1; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
