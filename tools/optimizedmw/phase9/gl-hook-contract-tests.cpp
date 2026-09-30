#include <components/sceneutil/glcalltrace.hpp>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace SceneUtil::GLCallTrace;

unsigned calls=0;
bool expectBreadcrumb=false;
void GL_APIENTRY query(GLuint id,GLenum pname,GLuint* result)
{
    if(id!=7 || pname!=9)throw std::runtime_error("arguments changed");
    if(expectBreadcrumb)
    {
        const auto breadcrumb=Capture::instance().breadcrumb(15);
        if(!breadcrumb.active || breadcrumb.row.api!=GetQueryObjectuiv
            || breadcrumb.row.where.frame!=42 || breadcrumb.row.where.phase!=State
            || breadcrumb.row.where.drawable!=9 || breadcrumb.row.args[0]!=7 || breadcrumb.row.args[1]!=9)
            throw std::runtime_error("active crash breadcrumb missing or corrupt");
    }
    ++calls;*result=123;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
GLboolean GL_APIENTRY unmap(GLenum target){return target==5?GL_TRUE:GL_FALSE;}

bool expectCompressedBreadcrumb=false;
bool expectTextureIdentity=false;
std::uintptr_t expectedTexture=0, expectedObject=0, expectedImage=0;
unsigned expectedRevision=0;
struct NoApplyTexture : osg::Texture2D { void apply(osg::State&) const override {} };
void GL_APIENTRY compressed(GLenum target,GLint level,GLenum internalFormat,GLsizei width,GLsizei height,
    GLint border,GLsizei imageSize,const void* data)
{
    if(target!=1 || level!=2 || internalFormat!=3 || width!=2048 || height!=1024 || border!=0
        || imageSize!=8388608 || !data)
        throw std::runtime_error("compressed upload arguments changed");
    if(expectCompressedBreadcrumb)
    {
        const auto breadcrumb=Capture::instance().breadcrumb(15);
        if(!breadcrumb.active || breadcrumb.row.api!=CompressedTexImage2D
            || breadcrumb.row.args[0]!=1 || breadcrumb.row.args[1]!=2 || breadcrumb.row.args[2]!=3
            || breadcrumb.row.args[3]!=2048 || breadcrumb.row.args[4]!=1024
            || breadcrumb.row.args[5]!=0 || breadcrumb.row.args[6]!=8388608)
            throw std::runtime_error("extended texture-upload breadcrumb missing");
    }
    if(expectTextureIdentity)
    {
        const auto row=Capture::instance().breadcrumb(15).row;
        if(row.texture!=expectedTexture || row.textureObject!=expectedObject || row.glName!=73
            || row.image!=expectedImage || row.imageRevision!=expectedRevision || row.textureUnit!=0
            || row.allocated || !row.dirty || !row.firstUse || row.where.camera!=88)
            throw std::runtime_error("actual OSG texture/object/revision/unit attribution wrong");
    }
}

int main(int argc,char** argv) try
{
    if(argc!=2)throw std::runtime_error("output prefix required");
    Capture::instance().enable(argv[1]);
    auto fn=&query;Hook<15,GetQueryObjectuiv,decltype(fn)>::install(fn);
    auto unmapFn=&unmap;Hook<15,UnmapBuffer,decltype(unmapFn)>::install(unmapFn);
    auto compressedFn=&compressed;Hook<15,CompressedTexImage2D,decltype(compressedFn)>::install(compressedFn);

    GLuint result=0;
    expectBreadcrumb=true;
    { Scope scope({15,42,State,9});fn(7,9,&result); }
    expectBreadcrumb=false;
    const auto completed=Capture::instance().breadcrumb(15);
    if(completed.active || completed.row.api!=GetQueryObjectuiv)
        throw std::runtime_error("breadcrumb did not retire after driver return");

    fn(7,9,&result); // Outside scope must pass through, without measurement.
    const auto t=Capture::instance().total(15,GetQueryObjectuiv);
    if(result!=123 || calls!=2 || t.calls!=1 || t.ms<1.)throw std::runtime_error("dispatch/record contract changed");
    if(Capture::instance().total(15,UnmapBuffer).calls!=0)
        throw std::runtime_error("unmap unexpectedly counted before traced scope");

    { Scope scope({15,42,State,9});
      if(unmapFn(5)!=GL_TRUE || unmapFn(6)!=GL_FALSE)throw std::runtime_error("return changed"); }
    if(Capture::instance().total(15,UnmapBuffer).calls!=2)throw std::runtime_error("returning dispatch not counted");

    int payload=1;
    expectCompressedBreadcrumb=true;
    { Scope scope({15,44,State,11});compressedFn(1,2,3,2048,1024,0,8388608,&payload); }
    expectCompressedBreadcrumb=false;
    if(Capture::instance().total(15,CompressedTexImage2D).calls!=1)
        throw std::runtime_error("compressed upload dispatch not counted");
    osg::ref_ptr<osg::State> state=new osg::State;state->setContextID(15);
    osg::ref_ptr<NoApplyTexture> texture=new NoApplyTexture;
    osg::ref_ptr<osg::Image> image=new osg::Image;
    image->allocateImage(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE);image->dirty();texture->setImage(image);
    texture->resizeGLObjectBuffers(16);
    osg::ref_ptr<osg::Texture::TextureObject> object=new osg::Texture::TextureObject(texture,73,GL_TEXTURE_2D);
    texture->setTextureObject(15,object);
    state->applyTextureAttribute(0,texture);
    expectedTexture=reinterpret_cast<std::uintptr_t>(texture.get());expectedObject=reinterpret_cast<std::uintptr_t>(object.get());
    expectedImage=reinterpret_cast<std::uintptr_t>(image.get());expectedRevision=image->getModifiedCount();
    expectTextureIdentity=true;
    { Scope scope({15,45,State,11,state.get(),88});ScopedTextureApply apply(*state,*texture);
      compressedFn(1,2,3,2048,1024,0,8388608,&payload); }
    expectTextureIdentity=false;
    { Scope scope({15,46,State,11,state.get(),88});compressedFn(1,2,3,2048,1024,0,8388608,&payload); }
    if(Capture::instance().breadcrumb(15).row.texture!=0)
        throw std::runtime_error("unscoped direct apply inherited previous texture identity");

    Hook<15,GetQueryObjectuiv,decltype(fn)>::install(fn);
    { Scope scope({15,43,State,9});fn(7,9,&result); }
    if(calls!=3)throw std::runtime_error("idempotent installation chained itself");

    std::cout<<"PASS: synthetic dispatch ABI, active crash breadcrumb, eight-argument texture upload metadata, "
                "returns, exactly one original call, idempotent hook and outside-scope passthrough\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
