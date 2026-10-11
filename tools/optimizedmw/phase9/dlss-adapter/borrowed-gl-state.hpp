#pragma once
#include "shared-d3d11-textures.hpp"
#include <exception>

namespace P9Dlss
{
    // Preserve the complete set of state touched by this synchronous bridge.
    // Does not pop an OSG state stack or pretend to restore unmodified state.
    // Object-local changes are restricted to our private transfer FBOs.
    struct BorrowedGlState
    {
        static constexpr GLenum ActiveTexture = 0x84E0, Texture0 = 0x84C0, CurrentProgram = 0x8B8D;
        static constexpr GLenum ReadFramebuffer = 0x8CA8, DrawFramebuffer = 0x8CA9;
        static constexpr GLenum ReadBinding = 0x8CAA, DrawBinding = 0x8CA6;
        static constexpr GLenum SamplerBinding = 0x8919;
        static constexpr std::array<GLenum, 13> capabilities{GL_BLEND, GL_DEPTH_TEST, GL_ALPHA_TEST,
            GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_DITHER, 0x8DB9, GL_COLOR_LOGIC_OP,
            0x8C89, 0x809D, 0x809E, 0x80A0};
        void (APIENTRY* active)(GLenum) = function<decltype(active)>("glActiveTexture");
        void (APIENTRY* useProgram)(GLuint) = function<decltype(useProgram)>("glUseProgram");
        void (APIENTRY* bindFramebuffer)(GLenum, GLuint) = function<decltype(bindFramebuffer)>("glBindFramebuffer");
        void (APIENTRY* bindSampler)(GLuint, GLuint) = function<decltype(bindSampler)>("glBindSampler");
        void (APIENTRY* indexedInteger)(GLenum, GLuint, GLint*) = function<decltype(indexedInteger)>("glGetIntegeri_v");
        void (APIENTRY* indexedBoolean)(GLenum, GLuint, GLboolean*) = function<decltype(indexedBoolean)>("glGetBooleani_v");
        void (APIENTRY* colorMaskIndexed)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean)
            = function<decltype(colorMaskIndexed)>("glColorMaski");
        void (APIENTRY* clipControl)(GLenum,GLenum) = function<decltype(clipControl)>("glClipControl");
        HGLRC context = wglGetCurrentContext();
        GLint program = 0, textureUnit = 0, activeTextureBinding = 0, texture0 = 0, sampler0 = 0;
        GLint readFramebuffer = 0, drawFramebuffer = 0;
        std::array<GLint, 4> viewport{}, scissor{};
        std::array<GLint, 2> polygonMode{};
        GLint clipOrigin=0,clipDepth=0,clipCount=0,drawBufferCount=0;
        std::array<std::array<GLboolean,4>,8> colorMasks{};
        std::array<GLboolean,8> clips{};
        GLboolean depthMask = GL_FALSE;
        std::array<GLboolean, capabilities.size()> enabled{};
        bool live = true;
        explicit BorrowedGlState(bool autoRestore = true) : live(autoRestore)
        {
            require(context != nullptr, "Borrow actual current GL state");
            glGetIntegerv(CurrentProgram, &program); glGetIntegerv(ActiveTexture, &textureUnit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &activeTextureBinding);
            active(Texture0); glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);
            indexedInteger(SamplerBinding, 0, &sampler0); active(textureUnit);
            glGetIntegerv(ReadBinding, &readFramebuffer); glGetIntegerv(DrawBinding, &drawFramebuffer);
            glGetIntegerv(GL_VIEWPORT, viewport.data()); glGetIntegerv(GL_SCISSOR_BOX, scissor.data());
            glGetIntegerv(GL_POLYGON_MODE,polygonMode.data());glGetIntegerv(0x935C,&clipOrigin);glGetIntegerv(0x935D,&clipDepth);
            glGetIntegerv(GL_MAX_CLIP_PLANES,&clipCount);glGetIntegerv(0x8824,&drawBufferCount);
            require(clipCount>=0 && clipCount<=int(clips.size()) && drawBufferCount>0 && drawBufferCount<=int(colorMasks.size()),
                "Borrowed GL state fits audited bounded clip/mask capacity");
            for(GLint i=0;i<clipCount;++i)clips[i]=glIsEnabled(GL_CLIP_PLANE0+i);
            for(GLint i=0;i<drawBufferCount;++i)indexedBoolean(GL_COLOR_WRITEMASK,i,colorMasks[i].data());
            glGetBooleanv(GL_DEPTH_WRITEMASK, &depthMask);
            for (unsigned i = 0; i < capabilities.size(); ++i) enabled[i] = glIsEnabled(capabilities[i]);
            glCheck("Capture borrowed GL bridge state");
        }
        BorrowedGlState(const BorrowedGlState&) = delete;
        bool same(const BorrowedGlState& other) const
        {
            return context == other.context && program == other.program && textureUnit == other.textureUnit
                && activeTextureBinding == other.activeTextureBinding
                && texture0 == other.texture0 && sampler0 == other.sampler0 && readFramebuffer == other.readFramebuffer
                && drawFramebuffer == other.drawFramebuffer && viewport == other.viewport && scissor == other.scissor
                && colorMasks == other.colorMasks && depthMask == other.depthMask && enabled == other.enabled
                && polygonMode==other.polygonMode && clipOrigin==other.clipOrigin && clipDepth==other.clipDepth
                && clipCount==other.clipCount && drawBufferCount==other.drawBufferCount && clips==other.clips;
        }
        void restore() noexcept
        {
            if (!live) return;
            // Changing contexts inside this scope is a contract violation. Do
            // not restore these object names into an unrelated owner's context.
            if (wglGetCurrentContext() != context) std::terminate();
            bindFramebuffer(ReadFramebuffer, readFramebuffer); bindFramebuffer(DrawFramebuffer, drawFramebuffer);
            useProgram(program); active(Texture0); glBindTexture(GL_TEXTURE_2D, texture0);
            bindSampler(0, sampler0); active(textureUnit);
            glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            glScissor(scissor[0], scissor[1], scissor[2], scissor[3]);
            colorMaskIndexed(0,colorMasks[0][0],colorMasks[0][1],colorMasks[0][2],colorMasks[0][3]);glDepthMask(depthMask);
            glPolygonMode(GL_FRONT,polygonMode[0]);glPolygonMode(GL_BACK,polygonMode[1]);clipControl(clipOrigin,clipDepth);
            for(GLint i=0;i<clipCount;++i)if(clips[i])glEnable(GL_CLIP_PLANE0+i);else glDisable(GL_CLIP_PLANE0+i);
            for (unsigned i = 0; i < capabilities.size(); ++i)
                if (enabled[i]) glEnable(capabilities[i]); else glDisable(capabilities[i]);
            live = false;
        }
        ~BorrowedGlState() { restore(); }
        void prepareImport() const
        {
            for (const auto capability : capabilities) glDisable(capability);
            for(GLint i=0;i<clipCount;++i)glDisable(GL_CLIP_PLANE0+i);
            glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);clipControl(0x8CA1,clipDepth); // GL_LOWER_LEFT; preserve depth convention.
            active(Texture0); bindSampler(0, 0);
        }
    };

    // Real foreign object/state sentinels. Verification observes state without
    // restoring it, so an incorrect importer cannot be hidden by the test.
    struct ForeignGlSentinel
    {
        BorrowedGlState baseline;
        void (APIENTRY* genFbo)(GLsizei, GLuint*) = function<decltype(genFbo)>("glGenFramebuffers");
        void (APIENTRY* deleteFbo)(GLsizei, const GLuint*) = function<decltype(deleteFbo)>("glDeleteFramebuffers");
        void (APIENTRY* attach)(GLenum,GLenum,GLenum,GLuint,GLint) = function<decltype(attach)>("glFramebufferTexture2D");
        void (APIENTRY* genSampler)(GLsizei, GLuint*) = function<decltype(genSampler)>("glGenSamplers");
        void (APIENTRY* deleteSampler)(GLsizei, const GLuint*) = function<decltype(deleteSampler)>("glDeleteSamplers");
        void (APIENTRY* samplerParameter)(GLuint,GLenum,GLint) = function<decltype(samplerParameter)>("glSamplerParameteri");
        std::array<GLuint,2> fbos{},textures{};
        GLuint sampler = 0;
        explicit ForeignGlSentinel(GLuint program,unsigned index)
        {
            genFbo(2,fbos.data());glGenTextures(2,textures.data());genSampler(1,&sampler);
            require(fbos[0] && fbos[1] && textures[0] && textures[1] && sampler,"Foreign GL sentinel objects");
            baseline.active(BorrowedGlState::Texture0);
            for(unsigned i=0;i<2;++i)
            {
                glBindTexture(GL_TEXTURE_2D,textures[i]);
                glTexImage2D(GL_TEXTURE_2D,0,0x881A,8,8,0,GL_RGBA,GL_FLOAT,nullptr);
                baseline.bindFramebuffer(0x8D40,fbos[i]);attach(0x8D40,0x8CE0,GL_TEXTURE_2D,textures[i],0);
                glDrawBuffer(0x8CE0);glReadBuffer(0x8CE0);
            }
            // The importer must unbind this depth comparison sampler while it
            // reads an ordinary sampler2D, then restore the exact foreign name.
            samplerParameter(sampler,0x884C,0x884E);
            baseline.bindSampler(0,sampler);glBindTexture(GL_TEXTURE_2D,textures[0]);
            baseline.active(BorrowedGlState::Texture0+3);glBindTexture(GL_TEXTURE_2D,textures[1]);
            baseline.bindFramebuffer(BorrowedGlState::ReadFramebuffer,fbos[0]);
            baseline.bindFramebuffer(BorrowedGlState::DrawFramebuffer,fbos[1]);baseline.useProgram(program);
            glViewport(3,5,17,19);glScissor(1,2,3,4);
            baseline.colorMaskIndexed(0,GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);
            baseline.colorMaskIndexed(1,GL_TRUE,GL_FALSE,GL_TRUE,GL_FALSE);
            glDepthMask(GL_FALSE);
            glPolygonMode(GL_FRONT_AND_BACK,index%2?GL_LINE:GL_POINT);
            baseline.clipControl(0x8CA2,index%2?0x935F:0x935E); // upper-left; vary zero/negative-one depth range.
            for(GLint i=0;i<baseline.clipCount;++i)if((i+index)%2)glEnable(GL_CLIP_PLANE0+i);else glDisable(GL_CLIP_PLANE0+i);
            for(unsigned i=0;i<BorrowedGlState::capabilities.size();++i)
                if((i+index)%2)glEnable(BorrowedGlState::capabilities[i]);else glDisable(BorrowedGlState::capabilities[i]);
            glCheck("Install adversarial borrowed GL state");
        }
        void close()
        {
            baseline.restore();
            // This sentinel deliberately changes index 1; the importer does
            // not. Retire the fixture's own change after proving preservation.
            baseline.colorMaskIndexed(1,baseline.colorMasks[1][0],baseline.colorMasks[1][1],baseline.colorMasks[1][2],baseline.colorMasks[1][3]);
            deleteSampler(1,&sampler);glDeleteTextures(2,textures.data());deleteFbo(2,fbos.data());
            sampler=0; textures={};fbos={};glCheck("Retire foreign sentinel objects after state restoration");
        }
    };
}
