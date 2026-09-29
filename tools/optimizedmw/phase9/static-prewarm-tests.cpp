#include <components/sceneutil/staticgeometryprewarm.hpp>
#include <osgViewer/Viewer>
#include <osg/GraphicsContext>
#include <iostream>
#include <stdexcept>
using namespace SceneUtil::StaticGeometryPrewarm;
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> t=new osg::GraphicsContext::Traits;
    t->readDISPLAY();t->setUndefinedScreenDetailsToDefaultScreen();t->width=32;t->height=32;t->doubleBuffer=false;
    auto context=osg::GraphicsContext::createGraphicsContext(t);
    osg::ref_ptr<osg::GraphicsContext> owner=context;
    require(context && context->realize() && context->makeCurrent(),"native context missing");
    auto* state=context->getState();osg::RenderInfo info(state,nullptr);
    osg::ref_ptr<osg::Geometry> g=new osg::Geometry;
    g->setUseDisplayList(false);g->setUseVertexBufferObjects(true);
    osg::ref_ptr<osg::VertexBufferObject> pose=new osg::VertexBufferObject;
    pose->setUsage(GL_DYNAMIC_DRAW_ARB);
    osg::ref_ptr<osg::Vec3Array> p=new osg::Vec3Array(3);
    p->setVertexBufferObject(pose);g->setVertexArray(p);
    osg::ref_ptr<osg::VertexBufferObject> shared=new osg::VertexBufferObject;
    shared->setUsage(GL_STATIC_DRAW_ARB);
    osg::ref_ptr<osg::Vec2Array> uv=new osg::Vec2Array(3);
    (*uv)[0].set(.25f,.5f);uv->setVertexBufferObject(shared);g->setTexCoordArray(0,uv);
    osg::ref_ptr<osg::DrawElementsUShort> indices=new osg::DrawElementsUShort(GL_TRIANGLES);
    indices->push_back(0);indices->push_back(1);indices->push_back(2);
    osg::ref_ptr<osg::ElementBufferObject> ebo=new osg::ElementBufferObject;
    ebo->setUsage(GL_STATIC_DRAW_ARB);indices->setElementBufferObject(ebo);g->addPrimitiveSet(indices);
    const auto before=p->getModifiedCount();
    const Result first=compile(info,*g);
    require(first.compiled==2 && first.bytes>0,"shared attributes and indices not prepared");
    require(!pose->getGLBufferObject(state->getContextID()),"private animated pose was materialized");
    require(p->getModifiedCount()==before,"precompile mutated live CPU pose");
    const Result second=compile(info,*g);
    require(second.compiled==0 && second.clean==2,"already prepared backing was resubmitted");
    auto* ext=state->get<osg::GLExtensions>();
    ext->glBindBuffer(GL_ARRAY_BUFFER_ARB,shared->getGLBufferObject(state->getContextID())->getGLObjectID());
    float read[6]{};ext->glGetBufferSubData(GL_ARRAY_BUFFER_ARB,0,sizeof(read),read);
    require(read[0]==.25f && read[1]==.5f,"shared UV bytes differ");
    uv->resize(4u*1024u*1024u/sizeof(osg::Vec2)+1);uv->dirty();
    require(compile(info,*g).skipped==1,"oversized shared buffer ignored bound");
    uv->resize(3);uv->dirty();uv->setDataVariance(osg::Object::DYNAMIC);
    require(compile(info,*g).skipped==1,"explicit mutable backing with STATIC_DRAW hint was prepared");
    uv->setDataVariance(osg::Object::STATIC);
    shared->setUsage(GL_DYNAMIC_DRAW_ARB);
    require(compile(info,*g).skipped==1,"foreign mutable buffer was prepared");
    require(glGetError()==GL_NO_ERROR,"GL error during static-only precompile");
    g->releaseGLObjects(state);context->releaseContext();context->close(true);
    std::cout<<"PASS: static UV/index contents, no private pose creation/mutation, clean reuse and bounded mutable/oversized fallback\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
