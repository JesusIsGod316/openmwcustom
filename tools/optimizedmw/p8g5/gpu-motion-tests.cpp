#include <components/rendercore/temporalframe.hpp>
#include <osgViewer/Viewer>
#include <osg/GraphicsContext>
#include <osg/GLExtensions>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
using namespace RenderCore::Temporal;
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> t=new osg::GraphicsContext::Traits;
    t->readDISPLAY();t->setUndefinedScreenDetailsToDefaultScreen();t->width=32;t->height=32;
    t->doubleBuffer=false;t->windowDecoration=false;
    auto context=osg::ref_ptr<osg::GraphicsContext>(osg::GraphicsContext::createGraphicsContext(t));
    require(context && context->realize() && context->makeCurrent(),"no real OpenGL context");
    auto* ext=context->getState()->get<osg::GLExtensions>();
    auto shader=[&](GLenum type,const std::string& source){
        GLuint s=ext->glCreateShader(type);const char* p=source.c_str();ext->glShaderSource(s,1,&p,nullptr);ext->glCompileShader(s);
        GLint ok=0;ext->glGetShaderiv(s,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[4096]{};ext->glGetShaderInfoLog(s,sizeof(log),nullptr,log);throw std::runtime_error(log);}return s;
    };
    std::ifstream file(P8G5_SHADER);require(file.good(),"candidate shader missing");
    std::string fragment((std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>());
    GLuint vs=shader(GL_VERTEX_SHADER,"#version 120\nvoid main(){gl_Position=gl_Vertex;}\n");
    GLuint fs=shader(GL_FRAGMENT_SHADER,fragment);GLuint program=ext->glCreateProgram();
    ext->glAttachShader(program,vs);ext->glAttachShader(program,fs);ext->glLinkProgram(program);
    GLint linked=0;ext->glGetProgramiv(program,GL_LINK_STATUS,&linked);require(linked,"motion program failed to link");
    GLuint textures[2]{};glGenTextures(2,textures);
    std::array<float,256> depths;depths.fill(.75f);
    glBindTexture(GL_TEXTURE_2D,textures[0]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,16,16,0,GL_RED,GL_FLOAT,depths.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D,textures[1]);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RG16F,16,16,0,GL_RG,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    GLuint fbo=0;ext->glGenFramebuffers(1,&fbo);ext->glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    ext->glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,textures[1],0);
    require(ext->glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE,"RG16F target incomplete");
    glDrawBuffer(GL_COLOR_ATTACHMENT0);glViewport(0,0,16,16);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    ext->glUseProgram(program);ext->glUniform1i(ext->glGetUniformLocation(program,"temporalDepth"),0);
    ext->glUniform2f(ext->glGetUniformLocation(program,"renderSize"),16,16);
    auto render=[&](const Frame& frame){
        glm::mat4 matrix(frame.clipToPreviousClip);
        ext->glUniformMatrix4fv(ext->glGetUniformLocation(program,"clipToPreviousClip"),1,GL_FALSE,glm::value_ptr(matrix));
        ext->glUniform2f(ext->glGetUniformLocation(program,"jitterPixels"),frame.jitterPixels.x,frame.jitterPixels.y);
        ext->glUniform1i(ext->glGetUniformLocation(program,"depthZeroToOne"),frame.input.depthRange==DepthRange::ZeroToOne);
        ext->glUniform1i(ext->glGetUniformLocation(program,"resetHistory"),!frame.hasHistory());
        glBindTexture(GL_TEXTURE_2D,textures[0]);glBegin(GL_TRIANGLES);
        glVertex2f(-1,-1);glVertex2f(3,-1);glVertex2f(-1,3);glEnd();
        std::array<float,512> flow{};glReadPixels(0,0,16,16,GL_RG,GL_FLOAT,flow.data());
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x){
            const auto expected=staticMotionFromDepth(frame,{x+.5,16-(y+.5)},.75);
            require(expected.has_value(),"invalid CPU fixture");
            const auto pos=(y*16+x)*2;
            require(std::abs(flow[pos]-expected->x)<.003 && std::abs(flow[pos+1]-expected->y)<.003,"RG16F camera/depth motion differs from CPU contract");
        }
    };
    History history;FrameInput input;input.identity={1,1,1,1};input.render={16,16};input.output={32,32};
    auto f=history.prepare(input);render(*f);history.commit(f->ticket);
    ++input.frame;f=history.prepare(input);render(*f);history.commit(f->ticket);
    ++input.frame;input.view=glm::translate(glm::dmat4(1),glm::dvec3(-.125,.25,0));
    f=history.prepare(input);render(*f);history.commit(f->ticket);
    ++input.frame;input.depthRange=DepthRange::ZeroToOne;f=history.prepare(input);render(*f);history.commit(f->ticket);
    ++input.frame;input.view=glm::translate(glm::dmat4(1),glm::dvec3(-.25,.5,0));f=history.prepare(input);render(*f);
    require(glGetError()==GL_NO_ERROR,"OpenGL motion test error");
    ext->glUseProgram(0);ext->glBindFramebuffer(GL_FRAMEBUFFER,0);ext->glDeleteFramebuffers(1,&fbo);
    ext->glDeleteProgram(program);ext->glDeleteShader(vs);ext->glDeleteShader(fs);glDeleteTextures(2,textures);
    context->releaseContext();context->close(true);
    std::cout<<"PASS: real RG16F shader pixels match CPU contract across reset, jitter-only, camera movement and both clip-depth ranges (5 x 256 pixels)\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
