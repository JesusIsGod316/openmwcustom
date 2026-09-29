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

    Hook<15,GetQueryObjectuiv,decltype(fn)>::install(fn);
    { Scope scope({15,43,State,9});fn(7,9,&result); }
    if(calls!=3)throw std::runtime_error("idempotent installation chained itself");

    std::cout<<"PASS: synthetic dispatch ABI, active crash breadcrumb, eight-argument texture upload metadata, "
                "returns, exactly one original call, idempotent hook and outside-scope passthrough\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
