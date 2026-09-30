#include <apps/openmw/mwrender/temporalmotion.hpp>
#include <components/rendercore/temporalframe.hpp>
#include <osgViewer/Viewer>
#include <osg/ColorMask>
#include <osg/FrameBufferObject>
#include <osg/GraphicsContext>
#include <osg/GLExtensions>
#include <osg/Geometry>
#include <osg/Image>
#include <osg/Multisample>
#include <osg/RenderInfo>
#include <osg/Shader>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
std::string source(const char* path)
{
    std::ifstream file(path); require(file.good(),"production shader missing");
    return {(std::istreambuf_iterator<char>(file)),std::istreambuf_iterator<char>()};
}
int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY();traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=64;traits->height=64;traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(),"real GL context missing");
    auto* state=context->getState();auto* ext=state->get<osg::GLExtensions>();
    osg::ref_ptr<osg::FrameStamp> stamp=new osg::FrameStamp;
    state->setFrameStamp(stamp);
    osg::RenderInfo info(state,nullptr);
    osg::ref_ptr<osg::Program> program=new osg::Program;
    program->addShader(new osg::Shader(osg::Shader::VERTEX,source(P9_VERTEX)));
    program->addShader(new osg::Shader(osg::Shader::FRAGMENT,source(P9_FRAGMENT)));
    MWRender::TemporalMotion pass(program);
    osg::ref_ptr<osg::Geometry> quad=new osg::Geometry;
    quad->setUseDisplayList(false);quad->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::Vec3Array> positions=new osg::Vec3Array;
    positions->push_back(osg::Vec3(-1,-1,0));positions->push_back(osg::Vec3(3,-1,0));positions->push_back(osg::Vec3(-1,3,0));
    quad->setVertexArray(positions);quad->addPrimitiveSet(new osg::DrawArrays(GL_TRIANGLES,0,3));
    auto depthImage=[](int size){
        osg::ref_ptr<osg::Image> image=new osg::Image;
        image->allocateImage(size,size,1,GL_RED,GL_FLOAT);
        auto* values=reinterpret_cast<float*>(image->data());
        for(int i=0;i<size*size;++i)values[i]=.75f;
        return image;
    };
    osg::ref_ptr<osg::Image> image=depthImage(16);
    osg::ref_ptr<osg::Texture2D> depth=new osg::Texture2D(image);
    depth->setInternalFormat(GL_R32F);depth->setTextureSize(16,16);
    depth->setFilter(osg::Texture::MIN_FILTER,osg::Texture::NEAREST);
    depth->setFilter(osg::Texture::MAG_FILTER,osg::Texture::NEAREST);
    depth->setResizeNonPowerOfTwoHint(false);
    osg::ref_ptr<osg::StateSet> original=new osg::StateSet;
    original->setMode(GL_BLEND,osg::StateAttribute::ON);
    original->setMode(GL_DEPTH_TEST,osg::StateAttribute::ON);
    original->setMode(GL_CULL_FACE,osg::StateAttribute::ON);
    original->setMode(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB,osg::StateAttribute::ON);
    original->setAttribute(new osg::ColorMask(false,true,false,true));
    state->pushStateSet(original);state->apply();
    GLuint sentinel[2]{};ext->glGenFramebuffers(2,sentinel);
    ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT,sentinel[0]);ext->glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT,sentinel[1]);
    glViewport(3,2,31,27);
    auto verifyState=[&]{
        GLint draw=0,read=0;std::array<GLint,4> viewport{};GLboolean mask[4]{};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT,&read);
        glGetIntegerv(GL_VIEWPORT,viewport.data());glGetBooleanv(GL_COLOR_WRITEMASK,mask);
        require(draw==static_cast<GLint>(sentinel[0]) && read==static_cast<GLint>(sentinel[1]),"motion pass leaked framebuffer bindings");
        require(viewport==std::array<GLint,4>{3,2,31,27},"motion pass leaked render viewport");
        require(glIsEnabled(GL_BLEND)&&glIsEnabled(GL_DEPTH_TEST)&&glIsEnabled(GL_CULL_FACE),"motion pass leaked raster state");
        require(glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE_ARB),"motion pass leaked alpha-to-coverage state");
        require(!mask[0]&&mask[1]&&!mask[2]&&mask[3],"motion pass leaked color mask");
    };
    MWRender::TemporalCamera camera;
    osg::ref_ptr<osg::RefMatrix> projection=new osg::RefMatrix;
    camera.projection=projection;camera.renderWidth=16;camera.renderHeight=16;
    camera.outputWidth=32;camera.outputHeight=32;camera.frame=1;
    // Model a cull-owned matrix finalized after capture. Taking an early copy
    // would give half the expected motion on the following frame.
    (*projection)(0,0)=2;(*projection)(1,1)=2;
    auto draw=[&](unsigned frame,float x,float y,bool reset){
        camera.frame=frame;stamp->setFrameNumber(frame);
        osg::Texture2D* flow=pass.render(info,camera,depth,*quad);
        require(flow,"production motion adapter failed");
        const auto status=pass.status(state->getContextID());
        require(status.submitted && !status.denseDynamicMotion,"incomplete camera field advertised as DLSS-ready");
        require((status.resetReasons!=0)==reset,"incorrect frame-history reset");
        require(status.historyValid==!reset,"consumer history-valid flag disagrees with reset state");
        require(status.renderWidth==camera.renderWidth && status.renderHeight==camera.renderHeight
            && status.outputWidth==camera.outputWidth && status.outputHeight==camera.outputHeight,
            "consumer extents differ from rendered frame");
        const auto consumer=pass.consumerFrame(state->getContextID(),frame);
        require(consumer.has_value(),"submitted temporal frame did not publish a consumer contract");
        require(consumer->motion.get()==flow && consumer->motionInPixels,
            "consumer motion surface or convention changed");
        require(consumer->status.frame==status.frame && consumer->status.previousFrame==status.previousFrame
            && consumer->status.targetRevision==status.targetRevision
            && consumer->status.resetReasons==status.resetReasons,
            "consumer status snapshot differs from render status");
        bool matrixData=false;
        for(float value:consumer->currentViewProjection) matrixData |= std::abs(value)>.00001f;
        require(matrixData,"consumer current view-projection matrix was not published");
        verifyState();
        osg::ref_ptr<osg::FrameBufferObject> read=new osg::FrameBufferObject;
        read->setAttachment(osg::Camera::COLOR_BUFFER0,osg::FrameBufferAttachment(flow));read->apply(*state);
        glReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
        std::array<float,512> values{};
        glReadPixels(0,0,camera.renderWidth,camera.renderHeight,GL_RG,GL_FLOAT,values.data());
        for(unsigned i=0;i<camera.renderWidth*camera.renderHeight;++i)
        {
            if(std::abs(values[2*i]-x)>.003f || std::abs(values[2*i+1]-y)>.003f)
            {
                std::cerr<<"frame="<<frame<<" pixel="<<i<<" actual="<<values[2*i]<<','<<values[2*i+1]<<" expected="<<x<<','<<y<<'\n';
                throw std::runtime_error("production adapter motion differs from independent pixel expectation");
            }
        }
        ext->glBindFramebuffer(GL_DRAW_FRAMEBUFFER_EXT,sentinel[0]);ext->glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT,sentinel[1]);
        return status;
    };
    draw(1,0,0,true);
    const auto firstDepth = depth;
    // Production has two equal-generation opaque depth inputs. Switching
    // between them is normal ownership, not a resize/reset on every frame.
    depth = new osg::Texture2D(*firstDepth,osg::CopyOp::SHALLOW_COPY);
    camera.view=osg::Matrixd::translate(-.125,.25,0);draw(2,2,4,false);
    depth = firstDepth;
    camera.view=osg::Matrixd::translate(-.25,.5,0);draw(3,2,4,false);
    require(!pass.render(info,camera,depth,*quad),"duplicate draw incorrectly advances history");
    camera.view=osg::Matrixd::translate(-.375,.75,0);draw(5,0,0,true);
    ++camera.cameraEpoch;draw(6,0,0,true);
    ++camera.worldEpoch;draw(7,0,0,true);
    (*projection)(0,0)=3;draw(8,0,0,true);
    (*projection)(2,2)=.5;draw(9,0,0,false);
    stamp->setFrameNumber(10);camera.frame=10;camera.renderWidth=15;
    require(!pass.render(info,camera,depth,*quad),"wrong-sized depth accepted");
    require(!pass.consumerFrame(state->getContextID(),camera.frame),"invalid frame left a stale DLSS consumer contract");
    verifyState();
    camera.renderWidth=16;draw(11,0,0,true);
    image=depthImage(8);depth->setImage(image);depth->setTextureSize(8,8);
    camera.renderWidth=8;camera.renderHeight=8;
    const auto resized=draw(12,0,0,true);require(resized.targetRevision==2,"target resize generation missing");
    camera.zeroToOne=true;camera.clearDepth=0;draw(13,0,0,true);
    camera.view=osg::Matrixd::translate(-.5,1,0);draw(14,1.5,2,false);
    pass.releaseGLObjects(state);
    require(!pass.consumerFrame(state->getContextID(),camera.frame),"GL release left stale temporal consumer state");
    draw(15,0,0,true);
    require(glGetError()==GL_NO_ERROR,"GL errors from temporal production adapter");
    pass.releaseGLObjects(state);depth->releaseGLObjects(state);quad->releaseGLObjects(state);
    state->popStateSet();state->apply();ext->glBindFramebuffer(GL_FRAMEBUFFER_EXT,0);ext->glDeleteFramebuffers(2,sentinel);
    context->releaseContext();context->close(true);
    std::cout<<"PASS: real production motion adapter, consumer-ready matrix/motion contract, retained final projection, camera/world/lens/gap/resize resets, near-far continuity, duplicate rejection, frame/depth validation and raster/FBO/viewport restoration\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
