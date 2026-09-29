// Preload the real OSG dependencies, then remove the optional core alias.
// Windows' GL headers do not supply it; Linux headers must not hide that gap.
#include <osg/Array>
#include <osg/BufferObject>
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/GLExtensions>
#include <osg/RenderInfo>
#include <osg/State>
#include <osgViewer/Viewer>
#include <osg/GraphicsContext>
#ifdef GL_ARRAY_BUFFER
static_assert(GL_ARRAY_BUFFER == GL_ARRAY_BUFFER_ARB);
#undef GL_ARRAY_BUFFER
#endif
#include <components/sceneutil/dynamicstream.hpp>
#ifdef GL_ARRAY_BUFFER
#error The regression must compile the production header without the core alias
#endif
#include <iostream>
#include <stdexcept>
using namespace SceneUtil::DynamicStream;
void require(bool v, const char* message) { if (!v) throw std::runtime_error(message); }
int main() try
{
    Budget budget;
    require(budget.admit(1, 4*1024*1024), "valid stream budget rejected");
    for (int i=1;i<8;++i) require(budget.admit(1, 4*1024*1024), "bounded admission rejected");
    require(!budget.admit(1, 1), "frame bound exceeded");
    require(!budget.admit(2, 4*1024*1024+1), "per-buffer bound exceeded");
    require(budget.admit(2, 1024), "next frame never recovers");

    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width=32; traits->height=32; traits->doubleBuffer=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "real GL context unavailable");
    auto* state=context->getState(); auto* ext=state->get<osg::GLExtensions>();
    osg::ref_ptr<osg::FrameStamp> stamp=new osg::FrameStamp;
    stamp->setFrameNumber(1); state->setFrameStamp(stamp);
    osg::ref_ptr<osg::Geometry> geometry=new osg::Geometry;
    osg::ref_ptr<osg::VertexBufferObject> vbo=new osg::VertexBufferObject;
    vbo->setUsage(GL_DYNAMIC_DRAW_ARB);
    osg::ref_ptr<osg::Vec3Array> vertices=new osg::Vec3Array;
    vertices->push_back(osg::Vec3(1,2,3)); vertices->push_back(osg::Vec3(4,5,6));
    vertices->setBufferObject(vbo); geometry->setVertexArray(vertices);
    osg::ref_ptr<osg::Vec3Array> normals=new osg::Vec3Array;
    normals->push_back(osg::Vec3(0,0,1)); normals->push_back(osg::Vec3(0,1,0));
    normals->setBufferObject(vbo); geometry->setNormalArray(normals);
    PrivateBuffer owned(vbo);
    Budget drawBudget;
    require(owned.refresh(*state,*geometry,drawBudget).result==Result::ColdOrClean, "cold VBO should use original path");
    auto* glbo=vbo->getOrCreateGLBufferObject(state->getContextID()); glbo->compileBuffer();
    const auto name=glbo->getGLObjectID();
    require(owned.refresh(*state,*geometry,drawBudget).result==Result::ColdOrClean, "clean VBO was unnecessarily orphaned");
    auto verify=[&]{
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB,name);
        std::array<float,12> actual{};
        ext->glGetBufferSubData(GL_ARRAY_BUFFER_ARB,0,sizeof(actual),actual.data());
        const float* wanted=reinterpret_cast<const float*>(vertices->getDataPointer());
        for (int i=0;i<6;++i) require(actual[i]==wanted[i],"position storage was not fully restored");
        wanted=reinterpret_cast<const float*>(normals->getDataPointer());
        for (int i=0;i<6;++i) require(actual[i+6]==wanted[i],"unchanged normals lost during full refill");
        ext->glBindBuffer(GL_ARRAY_BUFFER_ARB,0);
    };
    verify();
    for (unsigned frame=2;frame<202;++frame)
    {
        stamp->setFrameNumber(frame);
        (*vertices)[0].x()=static_cast<float>(frame); vertices->dirty();
        const auto outcome=owned.refresh(*state,*geometry,drawBudget);
        require(outcome.result==Result::Refreshed && outcome.bytes==48,"owned dirty VBO not refreshed");
        require(glbo->getGLObjectID()==name,"GL identity changed and could invalidate VAO bindings");
        require(!glbo->isDirty(),"refill did not complete before drawing");
        verify();
        require(owned.refresh(*state,*geometry,drawBudget).result==Result::ColdOrClean,"second view redundantly refreshed");
    }
    vertices->setModifiedCount(0xffffffu); glbo->dirty();
    require(owned.refresh(*state,*geometry,drawBudget).result==Result::Ineligible,"OSG sentinel revision must fall back");
    vertices->setModifiedCount(204);
    osg::ref_ptr<osg::Vec3Array> foreign=new osg::Vec3Array(*vertices);
    foreign->setBufferObject(new osg::VertexBufferObject); geometry->setVertexArray(foreign);
    require(owned.refresh(*state,*geometry,drawBudget).result==Result::Ineligible,"clone/rebind touched old owner's buffer");
    geometry->setVertexArray(vertices);
    vbo->setUsage(GL_STATIC_DRAW_ARB);
    require(owned.refresh(*state,*geometry,drawBudget).result==Result::Ineligible,"static source buffer must remain untouched");
    require(glGetError()==GL_NO_ERROR,"GL error from streaming experiment");
    vbo->releaseGLObjects(state); state->unbindVertexBufferObject();
    context->releaseContext(); context->close(true);
    std::cout << "PASS: ARB-only header compatibility, private VBO 200 revisions, all-stream refill, stable name, multiview reuse, sentinel/rebind/static fallback and bounded admission\n";
}
catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
