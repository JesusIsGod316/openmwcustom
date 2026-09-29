#include <components/sceneutil/glcalltrace.hpp>
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace SceneUtil::GLCallTrace;
unsigned calls=0;
void GL_APIENTRY query(GLuint id,GLenum pname,GLuint* result)
{
    if(id!=7 || pname!=9)throw std::runtime_error("arguments changed");
    ++calls;*result=123;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
}
GLboolean GL_APIENTRY unmap(GLenum target){return target==5?GL_TRUE:GL_FALSE;}
int main(int argc,char** argv) try
{
    if(argc!=2)throw std::runtime_error("output prefix required");
    Capture::instance().enable(argv[1]);
    auto fn=&query;Hook<15,GetQueryObjectuiv,decltype(fn)>::install(fn);
    auto unmapFn=&unmap;Hook<15,UnmapBuffer,decltype(unmapFn)>::install(unmapFn);
    GLuint result=0;
    { Scope scope({15,42,State,9});fn(7,9,&result);
      if(unmapFn(5)!=GL_TRUE || unmapFn(6)!=GL_FALSE)throw std::runtime_error("return changed"); }
    fn(7,9,&result); // Outside scope must pass through, without measurement.
    const auto t=Capture::instance().total(15,GetQueryObjectuiv);
    if(result!=123 || calls!=2 || t.calls!=1 || t.ms<1.)throw std::runtime_error("dispatch/record contract changed");
    if(Capture::instance().total(15,UnmapBuffer).calls!=2)throw std::runtime_error("returning dispatch not counted");
    Hook<15,GetQueryObjectuiv,decltype(fn)>::install(fn);
    { Scope scope({15,43,State,9});fn(7,9,&result); }
    if(calls!=3)throw std::runtime_error("idempotent installation chained itself");
    std::cout<<"PASS: synthetic dispatch ABI arguments/returns, exactly one original call, idempotent hook, outside-scope passthrough and timed scope\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
