#pragma once
#include "gpu-transfers.hpp"
#include "../../../../components/rendercore/synchronousinputlease.hpp"
#include <utility>

namespace P9Dlss
{
    // Separate, private GL owners in the game producer's formats. The bridge
    // samples them on the same current draw context; it never registers or
    // transfers ownership of the source textures to NGX/D3D.
    // The portable no-wait lease is obeyed by this owner. This is not an
    // asynchronous game lease or dense motion proof.
    struct SourceTextures
    {
        static constexpr GLenum DepthAttachment=0x8D00, DepthComponent32f=0x8CAC, Rg16f=0x822F;
        static constexpr GLenum DepthStencilAttachment=0x821A, DepthStencil=0x84F9;
        enum class FormatProfile { Float, Rgba8Depth24Stencil8, Rgba8Depth32fStencil8 };
        static constexpr GLenum VertexShader=0x8B31, FragmentShader=0x8B30, CompileStatus=0x8B81, LinkStatus=0x8B82;
        GLuint (APIENTRY* createShader)(GLenum)=function<decltype(createShader)>("glCreateShader");
        void (APIENTRY* shaderSource)(GLuint,GLsizei,const char* const*,const GLint*)=function<decltype(shaderSource)>("glShaderSource");
        void (APIENTRY* compileShader)(GLuint)=function<decltype(compileShader)>("glCompileShader");
        void (APIENTRY* shaderStatus)(GLuint,GLenum,GLint*)=function<decltype(shaderStatus)>("glGetShaderiv");
        void (APIENTRY* deleteShader)(GLuint)=function<decltype(deleteShader)>("glDeleteShader");
        GLuint (APIENTRY* createProgram)()=function<decltype(createProgram)>("glCreateProgram");
        void (APIENTRY* attachShader)(GLuint,GLuint)=function<decltype(attachShader)>("glAttachShader");
        void (APIENTRY* linkProgram)(GLuint)=function<decltype(linkProgram)>("glLinkProgram");
        void (APIENTRY* programStatus)(GLuint,GLenum,GLint*)=function<decltype(programStatus)>("glGetProgramiv");
        void (APIENTRY* useProgram)(GLuint)=function<decltype(useProgram)>("glUseProgram");
        void (APIENTRY* deleteProgram)(GLuint)=function<decltype(deleteProgram)>("glDeleteProgram");
        GLint (APIENTRY* uniformLocation)(GLuint,const char*)=function<decltype(uniformLocation)>("glGetUniformLocation");
        void (APIENTRY* uniform1i)(GLint,GLint)=function<decltype(uniform1i)>("glUniform1i");
        void (APIENTRY* activeTexture)(GLenum)=function<decltype(activeTexture)>("glActiveTexture");
        void (APIENTRY* genSampler)(GLsizei,GLuint*)=function<decltype(genSampler)>("glGenSamplers");
        void (APIENTRY* deleteSampler)(GLsizei,const GLuint*)=function<decltype(deleteSampler)>("glDeleteSamplers");
        void (APIENTRY* samplerParameter)(GLuint,GLenum,GLint)=function<decltype(samplerParameter)>("glSamplerParameteri");
        void (APIENTRY* clearDepthStencil)(GLenum,GLint,GLfloat,GLint)
            =function<decltype(clearDepthStencil)>("glClearBufferfi");
        std::array<Texture,3> images;
        std::array<GLint,3> realizedFormats{};
        GLuint framebuffer=0,program=0,importSampler=0;
        HGLRC ownerContext=nullptr;
        std::uint64_t frame=0,generation=0;
        bool produced=false,reversedDepth=false;
        FormatProfile profile=FormatProfile::Float;
        RenderCore::Temporal::SynchronousInputLease leases;
        using Request = RenderCore::Temporal::SynchronousRequest;
        using Lease = RenderCore::Temporal::SynchronousInputLease::Token;

        void initialize(GpuTransfers& gpu)
        {
            BorrowedGlState restore;
            ownerContext=wglGetCurrentContext(); require(ownerContext,"Private GL source owner context");
            gpu.gen(1,&framebuffer); require(framebuffer!=0,"Private source framebuffer");
            genSampler(1,&importSampler);require(importSampler!=0,"Owned source import sampler");
            samplerParameter(importSampler,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            samplerParameter(importSampler,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            samplerParameter(importSampler,GL_TEXTURE_WRAP_S,0x812F);
            samplerParameter(importSampler,GL_TEXTURE_WRAP_T,0x812F);
            samplerParameter(importSampler,0x884C,GL_NONE); // Never mutate a borrowed depth texture's compare policy.
            const char* vertex="#version 130\nvoid main(){gl_Position=gl_Vertex;}\n";
            const char* fragment="#version 130\nuniform sampler2D sourceImage;\nvoid main(){gl_FragColor=texelFetch(sourceImage,ivec2(gl_FragCoord.xy),0);}\n";
            program=createProgram();require(program!=0,"Source import program");
            for(const auto item:{std::pair{VertexShader,vertex},std::pair{FragmentShader,fragment}})
            {
                GLuint shader=createShader(item.first);require(shader!=0,"Source import shader");
                shaderSource(shader,1,&item.second,nullptr);compileShader(shader);
                GLint ready=0;shaderStatus(shader,CompileStatus,&ready);require(ready!=0,"Compile exact texel source import");
                attachShader(program,shader);deleteShader(shader);
            }
            linkProgram(program);GLint ready=0;programStatus(program,LinkStatus,&ready);require(ready!=0,"Link exact texel source import");
            useProgram(program);uniform1i(uniformLocation(program,"sourceImage"),0);useProgram(0);
            glCheck("Create private GL source import backend");
        }
        void create(unsigned width,unsigned height,std::uint64_t epoch,
            FormatProfile selected=FormatProfile::Float,bool reverse=false)
        {
            BorrowedGlState restore;
            restore.active(BorrowedGlState::Texture0);
            require(wglGetCurrentContext()==ownerContext,"Source generation created on exact owner");
            require(!images[0].gl && width && height && epoch>generation,"New source generation after retirement");
            generation=epoch;produced=false;profile=selected;reversedDepth=reverse;
            const bool packed=profile!=FormatProfile::Float;
            const bool floatDepth=profile==FormatProfile::Rgba8Depth32fStencil8;
            const std::array<GLenum,3> formats{packed?GL_RGBA8:GpuTransfers::Rgba16f,
                packed?(floatDepth?0x8CADu:0x88F0u):DepthComponent32f,Rg16f};
            const std::array<GLenum,3> external{GL_RGBA,packed?DepthStencil:GL_DEPTH_COMPONENT,0x8227};
            const std::array<GLenum,3> types{GLenum(packed?GL_UNSIGNED_BYTE:GL_FLOAT),
                packed?(floatDepth?0x8DADu:0x84FAu):GLenum(GL_FLOAT),GLenum(GL_FLOAT)};
            for(unsigned i=0;i<3;++i)
            {
                auto& texture=images[i];texture.width=width;texture.height=height;
                glGenTextures(1,&texture.gl);require(texture.gl!=0,"Private source texture");
                glBindTexture(GL_TEXTURE_2D,texture.gl);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D,0x884C,GL_NONE); // GL_TEXTURE_COMPARE_MODE
                glTexImage2D(GL_TEXTURE_2D,0,formats[i],width,height,0,external[i],types[i],nullptr);
                glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&realizedFormats[i]);
                require(realizedFormats[i]==GLint(formats[i]),"Realized source format equals requested sized format");
            }
            glBindTexture(GL_TEXTURE_2D,0);glCheck("Allocate game-shaped private color/depth/motion");
        }
        static std::pair<int,int> shift(unsigned index)
        {return index==1?std::pair{2,1}:index==3?std::pair{-2,-1}:std::pair{0,0};}
        static std::array<float,2> motion(unsigned index)
        {const auto [x,y]=shift(index);return {-float(x),float(y)};}
        static unsigned quadrant(unsigned x,unsigned y,unsigned width,unsigned height,unsigned index)
        {const auto [dx,dy]=shift(index);return (int(x)>=int(width/2)+dx)+2*(int(y)>=int(height/2)+dy);}
        static float depth(unsigned quadrant){return std::array{.25f,.375f,.5f,.75f}[quadrant];}
        template<class ColorFunction>
        void produce(GpuTransfers& gpu,unsigned index,std::uint64_t ticket,ColorFunction&& color)
        {
            require(!leases.borrowed(),"Cannot overwrite a borrowed source frame");
            BorrowedGlState restore;
            require(wglGetCurrentContext()==ownerContext && ticket>frame && images[0].gl,"Current fresh source draw owner");
            produced=false;frame=ticket;
            const auto [dx,dy]=shift(index);
            const auto mv=motion(index);
            for(unsigned lane=0;lane<3;++lane)
            {
                gpu.bind(GpuTransfers::Framebuffer,framebuffer);
                gpu.attach(GpuTransfers::Framebuffer,GpuTransfers::ColorAttachment,GL_TEXTURE_2D,0,0);
                gpu.attach(GpuTransfers::Framebuffer,DepthAttachment,GL_TEXTURE_2D,0,0);
                gpu.attach(GpuTransfers::Framebuffer,DepthStencilAttachment,GL_TEXTURE_2D,0,0);
                gpu.attach(GpuTransfers::Framebuffer,lane==1?(profile==FormatProfile::Float?DepthAttachment:
                    DepthStencilAttachment):GpuTransfers::ColorAttachment,
                    GL_TEXTURE_2D,images[lane].gl,0);
                glDrawBuffer(lane==1?GL_NONE:GpuTransfers::ColorAttachment);
                glReadBuffer(lane==1?GL_NONE:GpuTransfers::ColorAttachment);
                require(gpu.status(GpuTransfers::Framebuffer)==GpuTransfers::Complete,"Exact private producer attachment");
                glViewport(0,0,images[lane].width,images[lane].height);glDepthMask(GL_TRUE);
                gpu.colorMaskIndexed(0,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glEnable(GL_SCISSOR_TEST);
                const int splitX=int(images[lane].width/2)+dx,splitY=int(images[lane].height/2)+dy;
                for(unsigned q=0;q<4;++q)
                {
                    glScissor(q&1?splitX:0,q&2?splitY:0,q&1?int(images[lane].width)-splitX:splitX,
                        q&2?int(images[lane].height)-splitY:splitY);
                    const float z=reversedDepth?1.f-depth(q):depth(q);
                    const auto value=lane==0?color(q):lane==1?std::array<float,4>{z,0,0,1}:
                        std::array<float,4>{mv[0],mv[1],0,1};
                    if(lane==1 && profile!=FormatProfile::Float)
                        clearDepthStencil(0x84F9,0,z,GLint(q)+17);
                    else gpu.clear(lane==1?0x1801:GpuTransfers::Color,0,value.data()); // GL_DEPTH or GL_COLOR
                }
            }
            glDisable(GL_SCISSOR_TEST);gpu.bind(GpuTransfers::Framebuffer,0);glCheck("GPU produces moving owned source images");
            produced=true;
            require(leases.publish({ticket,ticket,1,reinterpret_cast<std::uintptr_t>(ownerContext),generation,
                {images[0].gl,images[1].gl,images[2].gl},images[0].width,images[0].height,true,true,true,true,reversedDepth}),
                "Publish exact fully produced synthetic source contract");
        }
        Request request(std::uint64_t ticket,std::uint64_t epoch) const
        { return {ticket,ticket,1,reinterpret_cast<std::uintptr_t>(wglGetCurrentContext()),epoch,
            images[0].width,images[0].height}; }
        bool accepts(std::uint64_t ticket,std::uint64_t epoch,const std::array<Texture,5>& shared) const
        {
            if(!produced || ticket!=frame || epoch!=generation || wglGetCurrentContext()!=ownerContext)return false;
            for(unsigned i=0;i<3;++i)
                if(!images[i].gl || images[i].gl==shared[i].gl || images[i].width!=shared[i].width
                    || images[i].height!=shared[i].height || !shared[i].registration)return false;
            return true;
        }
        bool copyToShared(GpuTransfers& gpu,const Request& expected,const std::array<Texture,5>& shared)
        {
            // Refusal performs no graphics calls and preserves ordinary output.
            if(!accepts(expected.ticket,expected.generation,shared)) return false;
            const auto lease=leases.acquire(expected);
            if(!lease) return false;
            struct Release { RenderCore::Temporal::SynchronousInputLease& owner; Lease token;
                ~Release(){if(!owner.release(token))std::terminate();} } release{leases,*lease};
            BorrowedGlState restore;
            restore.prepareImport();
            restore.bindSampler(0,importSampler);
            // The caller holds every DX lock; we only borrow the same draw
            // context until the exact source copy commands have been submitted.
            // Exact texelFetch has no filtering, UV flip or CPU pixel transfer.
            activeTexture(0x84C0);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_ALPHA_TEST);
            glDisable(GL_STENCIL_TEST);glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);useProgram(program);
            for(unsigned i=0;i<3;++i)
            {
                gpu.producerTarget(shared[i]);glBindTexture(GL_TEXTURE_2D,images[i].gl);
                glBegin(GL_TRIANGLE_STRIP);glVertex2f(-1,-1);glVertex2f(1,-1);glVertex2f(-1,1);glVertex2f(1,1);glEnd();
            }
            glBindTexture(GL_TEXTURE_2D,0);useProgram(0);gpu.bind(GpuTransfers::Framebuffer,0);
            glCheck("Import exact private GL source pixels into locked shared DLSS inputs");
            return true;
        }
        void retire(GpuTransfers& gpu)
        {
            require(leases.retire(),"Source generation cannot retire while borrowed");
            BorrowedGlState restore;
            require(wglGetCurrentContext()==ownerContext,"Source retirement on exact owner");
            // Only after the fixture's exact source/output verification and
            // completed application work. No destructor retires in-flight GL.
            gpu.bind(GpuTransfers::Framebuffer,framebuffer);
            gpu.attach(GpuTransfers::Framebuffer,GpuTransfers::ColorAttachment,GL_TEXTURE_2D,0,0);
            gpu.attach(GpuTransfers::Framebuffer,DepthAttachment,GL_TEXTURE_2D,0,0);
            gpu.attach(GpuTransfers::Framebuffer,DepthStencilAttachment,GL_TEXTURE_2D,0,0);
            gpu.bind(GpuTransfers::Framebuffer,0);
            for(auto& texture:images){glDeleteTextures(1,&texture.gl);texture={};}produced=false;
        }
        void close(GpuTransfers& gpu)
        {
            require(wglGetCurrentContext()==ownerContext,"Source backend closed on exact owner");
            require(!images[0].gl,"No live private sources at backend close");
            deleteProgram(program);program=0;deleteSampler(1,&importSampler);importSampler=0;
            gpu.remove(1,&framebuffer);framebuffer=0;
            glCheck("Release private source import backend");
        }
    };
}
