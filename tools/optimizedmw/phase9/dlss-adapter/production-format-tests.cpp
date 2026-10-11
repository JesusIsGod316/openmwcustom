#include "source-textures.hpp"
#include <osg/GraphicsContext>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Program>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <osg/State>
#include <osg/StateSet>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/Version>
#include <cmath>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>

// Import-only native format/OSG proof. The sources are controlled synthetic
// images. No game frame, NGX evaluation, jitter or dense game motion claim.
namespace P9Dlss
{
    std::string jsonString(const std::string& value)
    {
        std::string result="\"";
        for(const unsigned char c:value)
        {
            require(c>=32,"Evidence string has no control bytes");
            if(c=='\\' || c=='\"')result+='\\';
            result+=char(c);
        }
        return result+'\"';
    }

    std::string loadedModulePath(const wchar_t* name)
    {
        const HMODULE module=GetModuleHandleW(name);require(module,"Expected actual OSG runtime loaded");
        std::array<wchar_t,32768> path{};
        const DWORD count=GetModuleFileNameW(module,path.data(),DWORD(path.size()));
        require(count && count<path.size(),"Observe exact loaded OSG runtime path");
        const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),int(count),nullptr,0,nullptr,nullptr);
        require(bytes>0,"Encode actual OSG runtime path");std::string result(bytes,'\0');
        require(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,path.data(),int(count),result.data(),bytes,nullptr,nullptr)==bytes,
            "Complete runtime path encoding");return result;
    }

    struct FormatEvidence
    {
        std::ofstream file;
        explicit FormatEvidence(const std::filesystem::path& path) : file(path)
        { require(file.good(),"Open fresh production-format evidence"); }
        void row(const std::string& check,const std::string& fields)
        {
            const auto line="{\"schema\":1,\"check\":\""+check+"\","+fields+"}";
            file<<line<<'\n';file.flush();require(file.good(),"Write bounded format evidence");
            std::cout<<line<<std::endl;
        }
    };

    template<class Value>
    std::vector<Value> readImage(const Texture& image,GLenum format,GLenum type,unsigned channels)
    {
        BorrowedGlState restore;restore.active(BorrowedGlState::Texture0);
        std::vector<Value> result(std::size_t(image.width)*image.height*channels);
        glBindTexture(GL_TEXTURE_2D,image.gl);
        glGetTexImage(GL_TEXTURE_2D,0,format,type,result.data());
        glCheck("Read actual realized source/import image");return result;
    }

    // Independent D3D observation, outside the GL lock. Blocking readback is
    // confined to this 64x48 verifier and never added to a production frame.
    template<class Value>
    std::vector<Value> readD3d(Device& device,const Texture& image,unsigned channels)
    {
        D3D11_TEXTURE2D_DESC desc{};image.dx->GetDesc(&desc);
        desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;
        desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
        ComPtr<ID3D11Texture2D> staging;
        dx(device.device->CreateTexture2D(&desc,nullptr,&staging),"Create bounded D3D format verifier");
        device.commands->CopyResource(staging.Get(),image.dx.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        dx(device.commands->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped),"Observe actual imported D3D image");
        std::vector<Value> result(std::size_t(image.width)*image.height*channels);
        for(unsigned y=0;y<image.height;++y)
            std::memcpy(result.data()+std::size_t(y)*image.width*channels,
                static_cast<const char*>(mapped.pData)+std::size_t(y)*mapped.RowPitch,
                std::size_t(image.width)*channels*sizeof(Value));
        device.commands->Unmap(staging.Get(),0);return result;
    }

    float halfValue(std::uint16_t bits)
    {
        const unsigned sign=(bits&0x8000u)<<16,exponent=(bits>>10)&31,mantissa=bits&1023;
        // The fixture deliberately uses only exactly representable zero and
        // +/-1 or +/-2 pixel motion; other encodings fail rather than guess.
        require((exponent==0 && mantissa==0) || ((exponent==15 || exponent==16) && mantissa==0),
            "Actual D3D half motion is a fixture-representable value");
        return std::bit_cast<float>(sign|(exponent?(exponent+112)<<23:0));
    }

    struct PackedFloatDepth { float depth; std::uint32_t stencil; };
    static_assert(sizeof(PackedFloatDepth)==8);

    template<class Function>
    std::string quadrantSamples(unsigned width,unsigned height,Function&& sample)
    {
        const std::array<std::size_t,4> positions{0,width-1,std::size_t(height-1)*width,
            std::size_t(height)*width-1};
        std::string result="[";
        for(unsigned i=0;i<positions.size();++i){if(i)result+=',';result+=std::to_string(sample(positions[i]));}
        return result+']';
    }

    struct OsgCache
    {
        const osg::Program::PerContextProgram* program;
        const osg::StateAttribute* texture;
        const osg::VertexArrayState* arrays;
        unsigned active,clientActive,stack;
        GLuint vao;
        bool blend,depth;
        explicit OsgCache(osg::State& state)
            : program(state.getLastAppliedProgramObject())
            , texture(state.getLastAppliedTextureAttribute(0,osg::StateAttribute::TEXTURE))
            , arrays(state.getCurrentVertexArrayState())
            , active(state.getActiveTextureUnit()),clientActive(state.getClientActiveTextureUnit())
            , stack(state.getStateSetStackSize()),vao(state.getCurrentVertexArrayObject())
            , blend(state.getLastAppliedModeValue(GL_BLEND)),depth(state.getLastAppliedModeValue(GL_DEPTH_TEST)) {}
        bool same(const OsgCache& other) const
        { return program==other.program && texture==other.texture && arrays==other.arrays && active==other.active
            && clientActive==other.clientActive && stack==other.stack && vao==other.vao
            && blend==other.blend && depth==other.depth; }
    };

    struct ProductionFormatFixture
    {
        static constexpr unsigned Width=64,Height=48;
        HiddenContext context;
        Device device;
        Bridge bridge;
        GpuTransfers gpu;
        SourceTextures source;
        std::array<Texture,5> shared;
        osg::ref_ptr<osg::State> state;
        osg::ref_ptr<osg::Geometry> ordinary;
        osg::ref_ptr<osg::Program> ordinaryProgram;
        osg::ref_ptr<osg::Texture2D> ordinaryTexture;
        osg::ref_ptr<osg::StateSet> ordinaryState;
        Texture ordinaryTarget;
        GLuint ordinaryFbo=0;
        unsigned contextId=0,copies=0,ordinaryDraws=0,generations=0;
        std::uint64_t frame=0;
        bool negativeCorrupt=false;

        ProductionFormatFixture()
        {
            require(std::string(osgGetVersion())=="3.6.5","Exact pinned OSG 3.6.5 native proof");
            bridge.device=bridge.openDevice(device.device.Get());require(bridge.device,"Open same-device import bridge");
            gpu.initialize();source.initialize(gpu);
            auto samplerValue=function<void (APIENTRY*)(GLuint,GLenum,GLint*)>("glGetSamplerParameteriv");
            GLint compare=1;samplerValue(source.importSampler,0x884C,&compare);
            require(compare==GL_NONE,"Actual owned noncomparison import sampler");
            state=new osg::State;contextId=osg::GraphicsContext::createNewContextID();
            state->setContextID(contextId);state->initializeExtensionProcs();
            ordinaryProgram=new osg::Program;
            ordinaryProgram->addShader(new osg::Shader(osg::Shader::VERTEX,
                "#version 130\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;}\n"));
            ordinaryProgram->addShader(new osg::Shader(osg::Shader::FRAGMENT,
                "#version 130\nuniform sampler2D image;void main(){gl_FragColor=texture2D(image,gl_TexCoord[0].xy);}\n"));
            osg::ref_ptr<osg::Image> image=new osg::Image;image->allocateImage(2,2,1,GL_RGBA,GL_UNSIGNED_BYTE);
            const std::array<unsigned char,16> pixels{255,0,0,255,0,255,0,255,0,0,255,255,255,255,0,255};
            std::copy(pixels.begin(),pixels.end(),image->data());
            ordinaryTexture=new osg::Texture2D(image);ordinaryTexture->setInternalFormat(GL_RGBA8);
            ordinaryTexture->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
            ordinaryTexture->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
            ordinaryTexture->setWrap(osg::Texture::WRAP_S,osg::Texture::CLAMP_TO_EDGE);
            ordinaryTexture->setWrap(osg::Texture::WRAP_T,osg::Texture::CLAMP_TO_EDGE);
            ordinaryTexture->setResizeNonPowerOfTwoHint(false);
            ordinaryState=new osg::StateSet;ordinaryState->setAttributeAndModes(ordinaryProgram);
            ordinaryState->setTextureAttributeAndModes(0,ordinaryTexture);
            ordinaryState->addUniform(new osg::Uniform("image",0));
            for(const auto capability:BorrowedGlState::capabilities)ordinaryState->setMode(capability,osg::StateAttribute::OFF);
            ordinary=new osg::Geometry;ordinary->setUseDisplayList(false);ordinary->setUseVertexBufferObjects(true);
            ordinary->setUseVertexArrayObject(false);
            osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
            vertices->push_back({-1,-1,0});vertices->push_back({3,-1,0});vertices->push_back({-1,3,0});
            osg::ref_ptr<osg::Vec2Array> coords=new osg::Vec2Array;
            coords->push_back({0,0});coords->push_back({2,0});coords->push_back({0,2});
            ordinary->setVertexArray(vertices);ordinary->setTexCoordArray(0,coords);
            ordinary->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
            ordinaryTarget.width=Width;ordinaryTarget.height=Height;
            glGenTextures(1,&ordinaryTarget.gl);glBindTexture(GL_TEXTURE_2D,ordinaryTarget.gl);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,Width,Height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            gpu.gen(1,&ordinaryFbo);gpu.bind(GpuTransfers::Framebuffer,ordinaryFbo);
            gpu.attach(GpuTransfers::Framebuffer,GpuTransfers::ColorAttachment,GL_TEXTURE_2D,ordinaryTarget.gl,0);
            glDrawBuffer(GpuTransfers::ColorAttachment);glReadBuffer(GpuTransfers::ColorAttachment);
            require(gpu.status(GpuTransfers::Framebuffer)==GpuTransfers::Complete,"Ordinary Geometry proof target");
            state->pushStateSet(ordinaryState);glCheck("Create owned bounded OSG draw fixture");
        }

        std::vector<unsigned char> drawOrdinary()
        {
            gpu.bind(GpuTransfers::Framebuffer,ordinaryFbo);glViewport(0,0,Width,Height);
            gpu.colorMaskIndexed(0,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            state->apply();osg::RenderInfo info(state,nullptr);
            ordinary->osg::Geometry::drawImplementation(info);++ordinaryDraws;
            glCheck("Actual ordinary textured Geometry after raw import");
            auto result=readImage<unsigned char>(ordinaryTarget,GL_RGBA,GL_UNSIGNED_BYTE,4);
            const std::array<std::array<unsigned char,4>,4> expected{{{255,0,0,255},{0,255,0,255},
                {0,0,255,255},{255,255,0,255}}};
            for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)
                for(unsigned c=0;c<4;++c)
                    require(result[(std::size_t(y)*Width+x)*4+c]==expected[(x>=Width/2)+2*(y>=Height/2)][c],
                        "Ordinary textured Geometry exact nonempty expected pixels");
            require(state->getLastAppliedProgramObject() && state->getLastAppliedTextureAttribute(0,
                osg::StateAttribute::TEXTURE)==ordinaryTexture.get(),"Actual OSG program/texture caches populated");
            return result;
        }

        std::vector<unsigned char> stencil()
        {
            BorrowedGlState restore;gpu.bind(GpuTransfers::Framebuffer,gpu.framebuffers[1]);
            gpu.attach(GpuTransfers::Framebuffer,GpuTransfers::ColorAttachment,GL_TEXTURE_2D,0,0);
            gpu.attach(GpuTransfers::Framebuffer,SourceTextures::DepthStencilAttachment,GL_TEXTURE_2D,source.images[1].gl,0);
            glReadBuffer(GL_NONE);glDrawBuffer(GL_NONE);
            require(gpu.status(GpuTransfers::Framebuffer)==GpuTransfers::Complete,"Packed stencil validation attachment");
            std::vector<unsigned char> values(Width*Height);
            glReadPixels(0,0,Width,Height,GL_STENCIL_INDEX,GL_UNSIGNED_BYTE,values.data());
            gpu.attach(GpuTransfers::Framebuffer,SourceTextures::DepthStencilAttachment,GL_TEXTURE_2D,0,0);
            glCheck("Observe unchanged actual packed stencil plane");return values;
        }

        void run(SourceTextures::FormatProfile profile,bool reverse,FormatEvidence& evidence)
        {
            ++generations;source.create(Width,Height,generations,profile,reverse);
            shared[0]=device.make(bridge,Width,Height,DXGI_FORMAT_R8G8B8A8_UNORM,false);
            shared[1]=device.make(bridge,Width,Height,DXGI_FORMAT_R32_FLOAT,false);
            shared[2]=device.make(bridge,Width,Height,DXGI_FORMAT_R16G16_FLOAT,false);
            std::vector<HANDLE> handles{shared[0].registration,shared[1].registration,shared[2].registration};
            for(unsigned index=0;index<4;++index)
            {
                source.produce(gpu,index,++frame,[](unsigned q){
                    std::array<float,4> color{};
                    for(unsigned c=0;c<4;++c)color[c]=float((q*37+c*53+19)%256)/255.f;
                    return color;
                });
                auto color=readImage<unsigned char>(source.images[0],GL_RGBA,GL_UNSIGNED_BYTE,4);
                auto depth=readImage<float>(source.images[1],GL_DEPTH_COMPONENT,GL_FLOAT,1);
                auto motion=readImage<float>(source.images[2],0x8227,GL_FLOAT,2);
                const auto beforeStencil=stencil();
                const bool depth24=profile==SourceTextures::FormatProfile::Rgba8Depth24Stencil8;
                std::vector<std::uint32_t> packed24;
                std::vector<PackedFloatDepth> packed32;
                std::vector<float> canonicalDepth(depth.size());
                if(depth24)packed24=readImage<std::uint32_t>(source.images[1],SourceTextures::DepthStencil,0x84FA,1);
                else packed32=readImage<PackedFloatDepth>(source.images[1],SourceTextures::DepthStencil,0x8DAD,1);
                unsigned maximumCpuDepthUlps=0;
                for(std::size_t i=0;i<depth.size();++i)
                {
                    // Independent format expectation, never based on the
                    // imported sample: exact packed integer / (2^24-1),
                    // rounded once to R32F. CPU GL_FLOAT readback is an
                    // additional conversion and can round differently.
                    canonicalDepth[i]=depth24?float(double(packed24[i]>>8)/16777215.0):packed32[i].depth;
                    const auto a=std::bit_cast<unsigned>(depth[i]),b=std::bit_cast<unsigned>(canonicalDepth[i]);
                    maximumCpuDepthUlps=std::max(maximumCpuDepthUlps,a>b?a-b:b-a);
                    require((depth24?packed24[i]&255:packed32[i].stencil&255)==beforeStencil[i],
                        "Actual packed representation independently contains the observed stencil");
                }
                for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x)
                {
                    const auto q=SourceTextures::quadrant(x,y,Width,Height,index);const auto pos=std::size_t(y)*Width+x;
                    for(unsigned c=0;c<4;++c)require(color[pos*4+c]==(q*37+c*53+19)%256,"Actual RGBA8 producer bytes");
                    const float expected=reverse?1.f-SourceTextures::depth(q):SourceTextures::depth(q);
                    require(std::abs(canonicalDepth[pos]-expected)<0.0000002f,"Packed depth producer quantization bound");
                    require(maximumCpuDepthUlps<=1,"Bounded D24 CPU readback conversion relative to packed representation");
                    require(beforeStencil[pos]==q+17,"Actual nonzero packed stencil producer bytes");
                    const auto mv=SourceTextures::motion(index);
                    require(motion[pos*2]==mv[0] && motion[pos*2+1]==mv[1],"Signed synthetic pixel motion producer");
                }
                {
                    BorrowedGlState restore;restore.active(BorrowedGlState::Texture0);
                    glBindTexture(GL_TEXTURE_2D,source.images[1].gl);
                    glTexParameteri(GL_TEXTURE_2D,0x884C,0x884E); // Borrowed source demands comparison; owned sampler must override.
                }
                const auto ordinaryBefore=drawOrdinary();const OsgCache cacheBefore(*state);
                bridge.lock(handles);
                gpu.fill(shared[0],{-1,-1,-1,-1});
                const auto untouched=readImage<unsigned char>(shared[0],GL_RGBA,GL_UNSIGNED_BYTE,4);
                auto bad=source.request(frame,generations);++bad.frame;
                require(!source.copyToShared(gpu,bad,shared),"Actual stale frame refuses import");
                require(readImage<unsigned char>(shared[0],GL_RGBA,GL_UNSIGNED_BYTE,4)==untouched,
                    "Refused lease leaves fallback image untouched");
                {
                    ForeignGlSentinel poison(source.program,unsigned(frame));BorrowedGlState before(false);
                    require(source.copyToShared(gpu,source.request(frame,generations),shared),"Exact-format synthetic lease imports");
                    BorrowedGlState after(false);require(before.same(after),"Adversarial raw state restored after format import");
                    require(cacheBefore.same(OsgCache(*state)),"OSG caches unchanged while raw import borrows context");
                    poison.close();
                }
                if(negativeCorrupt)gpu.fill(shared[1],{-13,0,0,0});
                const auto copiedColor=readImage<unsigned char>(shared[0],GL_RGBA,GL_UNSIGNED_BYTE,4);
                const auto copiedDepth=readImage<float>(shared[1],0x1903,GL_FLOAT,1);
                const auto copiedMotion=readImage<float>(shared[2],0x8227,GL_FLOAT,2);
                const bool exact=copiedColor==color && copiedDepth==canonicalDepth && copiedMotion==motion;
                if(!exact && !negativeCorrupt)
                {
                    std::size_t colorMismatch=0,depthMismatch=0,motionMismatch=0,firstDepth=0;
                    float maximumDepthError=0;
                    for(std::size_t i=0;i<color.size();++i)colorMismatch+=copiedColor[i]!=color[i];
                    for(std::size_t i=0;i<motion.size();++i)motionMismatch+=copiedMotion[i]!=motion[i];
                    for(std::size_t i=0;i<depth.size();++i)
                    {
                        if(copiedDepth[i]!=canonicalDepth[i]){if(!depthMismatch)firstDepth=i;++depthMismatch;}
                        maximumDepthError=std::max(maximumDepthError,std::abs(copiedDepth[i]-canonicalDepth[i]));
                    }
                    evidence.row("production_format_pixel_mismatch","\"status\":\"fail\",\"color_components\":"
                        +std::to_string(colorMismatch)+",\"depth_components\":"+std::to_string(depthMismatch)
                        +",\"motion_components\":"+std::to_string(motionMismatch)
                        +",\"maximum_depth_error\":"+std::to_string(maximumDepthError)
                        +",\"source_depth_bits\":"+std::to_string(std::bit_cast<unsigned>(depth[firstDepth]))
                        +",\"canonical_depth_bits\":"+std::to_string(std::bit_cast<unsigned>(canonicalDepth[firstDepth]))
                        +",\"source_packed_integer\":"+std::to_string(depth24?packed24[firstDepth]>>8:0)
                        +",\"copied_depth_bits\":"+std::to_string(std::bit_cast<unsigned>(copiedDepth[firstDepth])));
                }
                require(exact,"Exact imported RGBA8 packed-depth and motion pixels");
                bridge.unlock(handles);
                require(readD3d<unsigned char>(device,shared[0],4)==color
                    && readD3d<float>(device,shared[1],1)==canonicalDepth,
                    "Independent actual D3D color/depth pixels equal the GL source");
                const auto d3dMotion=readD3d<std::uint16_t>(device,shared[2],2);
                for(std::size_t i=0;i<motion.size();++i)
                    require(halfValue(d3dMotion[i])==motion[i],"Independent actual D3D signed motion pixels");
                ++copies;
                GLint compare=0;
                {
                    BorrowedGlState restore;restore.active(BorrowedGlState::Texture0);
                    glBindTexture(GL_TEXTURE_2D,source.images[1].gl);glGetTexParameteriv(GL_TEXTURE_2D,0x884C,&compare);
                }
                require(compare==0x884E,"Borrowed depth compare policy unchanged by noncomparison import");
                require(stencil()==beforeStencil && readImage<unsigned char>(source.images[0],GL_RGBA,GL_UNSIGNED_BYTE,4)==color
                    && readImage<float>(source.images[1],GL_DEPTH_COMPONENT,GL_FLOAT,1)==depth,
                    "Borrowed color and both packed depth/stencil planes unchanged");
                if(depth24)
                    require(readImage<std::uint32_t>(source.images[1],SourceTextures::DepthStencil,0x84FA,1)==packed24,
                        "Exact actual packed D24 depth/stencil representation unchanged");
                else
                {
                    const auto after=readImage<PackedFloatDepth>(source.images[1],SourceTextures::DepthStencil,0x8DAD,1);
                    for(std::size_t i=0;i<after.size();++i)
                        require(std::bit_cast<unsigned>(after[i].depth)==std::bit_cast<unsigned>(packed32[i].depth)
                            && (after[i].stencil&255)==(packed32[i].stencil&255),
                            "Exact actual packed D32F depth/stencil representation unchanged");
                }
                require(cacheBefore.same(OsgCache(*state)),"Complete OSG cache identities unchanged before following draw");
                require(drawOrdinary()==ordinaryBefore,"Ordinary cached textured draw remains exact after import");
                evidence.row("production_format_import","\"status\":\"pass\",\"sequence\":"+std::to_string(frame)
                    +",\"generation\":"+std::to_string(generations)+",\"color_format\":\"RGBA8\",\"depth_format\":\""
                    +(profile==SourceTextures::FormatProfile::Rgba8Depth24Stencil8?"DEPTH24_STENCIL8":"DEPTH32F_STENCIL8")
                    +"\",\"reversed_depth\":"+(reverse?"true":"false")
                    +",\"width\":64,\"height\":48,\"exact_pixels\":true,\"source_stencil_unchanged\":true,"
                    "\"borrowed_compare_unchanged\":true,\"owned_noncomparison_sampler\":true,\"raw_state_restored\":true,"
                    "\"osg_cache_restored\":true,\"ordinary_geometry_exact\":true,\"fallback_untouched_on_refusal\":true,"
                    "\"independent_d3d_pixels_exact\":true,"
                    "\"packed_source_representation_unchanged\":true,\"depth_expectation_independent_of_import\":true,"
                    "\"depth_mapping\":\"D24_uint_div_16777215_round_once_to_R32F_or_D32F_identity\","
                    "\"import_depth_allowed_ulps\":0,\"maximum_cpu_depth_readback_ulps\":"+std::to_string(maximumCpuDepthUlps)+","
                    "\"depth_packed_integers_by_quadrant\":"+(depth24?
                        quadrantSamples(Width,Height,[&](auto i){return packed24[i]>>8;}):"[]")+","
                    "\"depth_packed_float_bits_by_quadrant\":"+(!depth24?
                        quadrantSamples(Width,Height,[&](auto i){return std::bit_cast<unsigned>(packed32[i].depth);}):"[]")+","
                    "\"depth_cpu_float_bits_by_quadrant\":"+
                        quadrantSamples(Width,Height,[&](auto i){return std::bit_cast<unsigned>(depth[i]);})+","
                    "\"depth_canonical_float_bits_by_quadrant\":"+
                        quadrantSamples(Width,Height,[&](auto i){return std::bit_cast<unsigned>(canonicalDepth[i]);})+","
                    "\"depth_import_float_bits_by_quadrant\":"+
                        quadrantSamples(Width,Height,[&](auto i){return std::bit_cast<unsigned>(copiedDepth[i]);})+","
                    "\"actual_dlss_evaluations\":0,\"dense_game_motion_proved\":false");
            }
            glFinish(); // Native verification/retirement only; no production frame wait added.
            {
                BorrowedGlState restore;
                // Same retirement order as the evaluated adapter: owned FBOs
                // release every completed borrowed registration before NV/DX
                // unregister. No hidden attachment extends an image lifetime.
                for(const auto fbo:gpu.framebuffers)
                {
                    gpu.bind(GpuTransfers::Framebuffer,fbo);
                    gpu.attach(GpuTransfers::Framebuffer,GpuTransfers::ColorAttachment,GL_TEXTURE_2D,0,0);
                    gpu.attach(GpuTransfers::Framebuffer,SourceTextures::DepthAttachment,GL_TEXTURE_2D,0,0);
                    gpu.attach(GpuTransfers::Framebuffer,SourceTextures::DepthStencilAttachment,GL_TEXTURE_2D,0,0);
                }
                glCheck("Detach every completed borrowed image before interop retirement");
            }
            source.retire(gpu);for(unsigned i=0;i<3;++i)retire(bridge,shared[i]);
        }

        void close()
        {
            state->popStateSet();ordinary->releaseGLObjects(state);ordinaryProgram->releaseGLObjects(state);
            ordinaryTexture->releaseGLObjects(state);state->releaseGLObjects();
            gpu.remove(1,&ordinaryFbo);glDeleteTextures(1,&ordinaryTarget.gl);
            source.close(gpu);gpu.close();require(bridge.closeDevice(bridge.device),"Close verified format bridge");
            osg::GraphicsContext::decrementContextIDUsageCount(contextId);
            state=nullptr;device.commands.Reset();device.device.Reset();device.adapter.Reset();context.close();
        }
    };
}

int main(int argc,char** argv)
{
    bool negative=false;
    try
    {
        P9Dlss::require((argc==3 || argc==4) && std::string(argv[1])=="--output",
            "Usage: p9-dlss-production-format --output fresh-evidence.jsonl [--negative-corrupt-copy]");
        if(argc==4){P9Dlss::require(std::string(argv[3])=="--negative-corrupt-copy","Unknown format fixture option");negative=true;}
        const auto path=std::filesystem::absolute(argv[2]);
        P9Dlss::require(!std::filesystem::exists(path),"Preserve prior production-format evidence");
        P9Dlss::FormatEvidence evidence(path);
        auto* fixture=new P9Dlss::ProductionFormatFixture;fixture->negativeCorrupt=negative;
        evidence.row("production_format_device","\"status\":\"pass\",\"same_physical_device\":true,"
            "\"nvidia_vendor\":true,\"window_hidden\":true,\"osg_version\":\"3.6.5\","
            "\"device_node_mask\":"+std::to_string(fixture->device.nodeMask)
            +",\"gl_renderer\":"+P9Dlss::jsonString(reinterpret_cast<const char*>(glGetString(GL_RENDERER)))
            +",\"gl_version\":"+P9Dlss::jsonString(reinterpret_cast<const char*>(glGetString(GL_VERSION)))
            +",\"osg_runtime_path\":"+P9Dlss::jsonString(P9Dlss::loadedModulePath(L"osg.dll"))
            +",\"openthreads_runtime_path\":"+P9Dlss::jsonString(P9Dlss::loadedModulePath(L"OpenThreads.dll"))
            +",\"actual_dlss_evaluations\":0,\"game_dlss_enabled\":false,\"dense_game_motion_proved\":false");
        for(const auto profile:{P9Dlss::SourceTextures::FormatProfile::Rgba8Depth24Stencil8,
            P9Dlss::SourceTextures::FormatProfile::Rgba8Depth32fStencil8})
            for(const bool reverse:{false,true})fixture->run(profile,reverse,evidence);
        fixture->close();
        evidence.row("production_format_complete","\"status\":\"pass\",\"copied_frames\":"+std::to_string(fixture->copies)
            +",\"ordinary_geometry_draws\":"+std::to_string(fixture->ordinaryDraws)
            +",\"retired_generations\":4,\"actual_dlss_evaluations\":0,\"game_dlss_enabled\":false,"
            "\"dense_game_motion_proved\":false,\"performance_promoted\":false");
        delete fixture;return 0;
    }
    catch(const std::exception& error)
    {
        if(negative && std::string(error.what())=="Exact imported RGBA8 packed-depth and motion pixels")
        {
            std::cout<<"{\"schema\":1,\"check\":\"production_format_negative\",\"status\":\"pass\","
                "\"corrupted_import_rejected\":true,\"actual_dlss_evaluations\":0}"<<std::endl;return 0;
        }
        std::cerr<<"Production-format fixture failed: "<<error.what()<<std::endl;return 1;
    }
}
