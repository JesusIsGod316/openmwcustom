#include <components/debug/v36gpuprofiler.hpp>
#include <osg/FrameStamp>
#include <osg/GraphicsContext>
#include <osgViewer/Viewer>
#include <iostream>
#include <stdexcept>

int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=16; traits->height=16; traits->doubleBuffer=false; traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    if(!context || !context->realize() || !context->makeCurrent()) throw std::runtime_error("no GL context");
    auto* state=context->getState();
    osg::ref_ptr<osg::FrameStamp> stamp=new osg::FrameStamp;
    state->setFrameStamp(stamp);
    osg::ref_ptr<osg::Camera> camera=new osg::Camera;
    osg::RenderInfo info(state,nullptr); info.pushCamera(camera);
    osg::ref_ptr<Debug::V36GpuProfiler::PassTracker> tracker=new Debug::V36GpuProfiler::PassTracker("owner-fixture");
    Debug::V3HitchTelemetry::sCurrentFrame=999999;
    for(unsigned frame=100; frame<140; ++frame)
    {
        stamp->setFrameNumber(frame);
        tracker->begin(info); glClear(GL_COLOR_BUFFER_BIT); tracker->end(info);
        // Fixture readback completes the tiny pass; production tracking never
        // adds this wait. Collection on the next begin remains availability-only.
        std::array<unsigned char,16*16*4> pixels{};
        glReadPixels(0,0,16,16,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    }
    if(glGetError()!=GL_NO_ERROR) throw std::runtime_error("GPU owner fixture GL error");
    context->releaseContext(); context->close(true);
    std::cout << "PASS actual draw-owner GPU query submission with deliberately different update frame\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
