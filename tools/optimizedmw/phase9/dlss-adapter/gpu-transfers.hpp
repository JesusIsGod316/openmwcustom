#pragma once
#include "shared-d3d11-textures.hpp"
#include "borrowed-gl-state.hpp"

namespace P9Dlss
{
    // Owned private-context GPU producer and graphics consumer. Every borrowed
    // interop texture must be locked by the caller for these operations.
    struct GpuTransfers
    {
        static constexpr GLenum Framebuffer = 0x8D40, ReadFramebuffer = 0x8CA8, DrawFramebuffer = 0x8CA9;
        static constexpr GLenum ColorAttachment = 0x8CE0, Complete = 0x8CD5, Color = 0x1800, Rgba16f = 0x881A;
        void (APIENTRY* gen)(GLsizei, GLuint*) = function<decltype(gen)>("glGenFramebuffers");
        void (APIENTRY* remove)(GLsizei, const GLuint*) = function<decltype(remove)>("glDeleteFramebuffers");
        void (APIENTRY* bind)(GLenum, GLuint) = function<decltype(bind)>("glBindFramebuffer");
        void (APIENTRY* attach)(GLenum, GLenum, GLenum, GLuint, GLint)
            = function<decltype(attach)>("glFramebufferTexture2D");
        GLenum (APIENTRY* status)(GLenum) = function<decltype(status)>("glCheckFramebufferStatus");
        void (APIENTRY* clear)(GLenum, GLint, const GLfloat*) = function<decltype(clear)>("glClearBufferfv");
        void (APIENTRY* blit)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)
            = function<decltype(blit)>("glBlitFramebuffer");
        std::array<GLuint, 3> framebuffers{};
        void (APIENTRY* colorMaskIndexed)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean)
            = function<decltype(colorMaskIndexed)>("glColorMaski");
        Texture consumerImage; // Private GL owner; never registered with DX.

        void initialize()
        {
            gen(3, framebuffers.data());
            for (const auto name : framebuffers) require(name != 0, "Create owned GPU transfer framebuffer");
            glCheck("Create GPU transfer framebuffers");
        }
        void makeConsumer(unsigned width, unsigned height)
        {
            BorrowedGlState restore;
            restore.active(BorrowedGlState::Texture0);
            require(!consumerImage.gl, "No consumer target reuse before retirement");
            consumerImage.width = width; consumerImage.height = height;
            glGenTextures(1, &consumerImage.gl);
            glBindTexture(GL_TEXTURE_2D, consumerImage.gl);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, Rgba16f, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
            glBindTexture(GL_TEXTURE_2D, 0);
            glCheck("Allocate private graphics consumer target");
        }
        void producerTarget(const Texture& texture)
        {
            bind(Framebuffer, framebuffers[0]);
            attach(Framebuffer, ColorAttachment, GL_TEXTURE_2D, texture.gl, 0);
            glDrawBuffer(ColorAttachment);
            require(status(Framebuffer) == Complete, "Actual shared GL producer target is renderable");
            colorMaskIndexed(0,GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_DITHER);
            glDisable(0x8DB9); // GL_FRAMEBUFFER_SRGB: floating linear producer contract.
            glViewport(0, 0, texture.width, texture.height);
        }
        void fill(const Texture& texture, const std::array<float, 4>& value)
        {
            BorrowedGlState restore;
            producerTarget(texture);
            glDisable(GL_SCISSOR_TEST);
            // The float-buffer API preserves the negative output sentinel;
            // legacy clamped clear-color state is not an equivalent fixture.
            clear(Color, 0, value.data());
            bind(Framebuffer, 0);
            glCheck("GPU produces exact shared typed texture");
        }
        template<class ColorFunction>
        void quadrants(const Texture& texture, ColorFunction&& color)
        {
            BorrowedGlState restore;
            producerTarget(texture);
            glEnable(GL_SCISSOR_TEST);
            const unsigned halfWidth = texture.width / 2, halfHeight = texture.height / 2;
            for (unsigned q = 0; q < 4; ++q)
            {
                glScissor(q & 1 ? halfWidth : 0, q & 2 ? halfHeight : 0,
                    q & 1 ? texture.width - halfWidth : halfWidth,
                    q & 2 ? texture.height - halfHeight : halfHeight);
                const auto value = color(q);
                clear(Color, 0, value.data());
            }
            glDisable(GL_SCISSOR_TEST);
            bind(Framebuffer, 0);
            glCheck("GPU produces changed shared color quadrants");
        }
        void consume(const Texture& source)
        {
            BorrowedGlState restore;
            // A copied image must be independent of the draw owner's raster
            // policy as well as restore that policy afterward. State equality
            // alone cannot prove a correct blit under adversarial enables.
            restore.prepareImport();
            colorMaskIndexed(0,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
            require(consumerImage.gl && source.width == consumerImage.width && source.height == consumerImage.height,
                "Exact generation and extent at actual graphics consumer");
            bind(ReadFramebuffer, framebuffers[1]);
            attach(ReadFramebuffer, ColorAttachment, GL_TEXTURE_2D, source.gl, 0);
            glReadBuffer(ColorAttachment);
            require(status(ReadFramebuffer) == Complete, "Actual DLSS output is readable by graphics consumer");
            bind(DrawFramebuffer, framebuffers[2]);
            attach(DrawFramebuffer, ColorAttachment, GL_TEXTURE_2D, consumerImage.gl, 0);
            glDrawBuffer(ColorAttachment);
            require(status(DrawFramebuffer) == Complete, "Actual private graphics consumer is renderable");
            glDisable(GL_SCISSOR_TEST);
            blit(0, 0, source.width, source.height, 0, 0, source.width, source.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            bind(Framebuffer, 0);
            glCheck("OpenGL graphics consumes actual shared DLSS output");
        }
        void retireConsumer()
        {
            // The caller proved the actual copy completed by reading back this
            // exact destination. Detach borrowed interop owners before unregister.
            for (const auto name : framebuffers)
            {
                bind(Framebuffer, name);
                attach(Framebuffer, ColorAttachment, GL_TEXTURE_2D, 0, 0);
            }
            bind(Framebuffer, 0);
            glDeleteTextures(1, &consumerImage.gl); consumerImage.gl = 0;
            glCheck("Retire completed graphics consumer and borrowed attachments");
        }
        void close()
        {
            require(!consumerImage.gl, "Retired GPU consumer at context closure");
            remove(3, framebuffers.data()); framebuffers = {};
            glCheck("Release GPU transfer framebuffers");
        }
    };
}
