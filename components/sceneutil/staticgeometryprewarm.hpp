#ifndef OPENMW_SCENEUTIL_STATICGEOMETRYPREWARM_H
#define OPENMW_SCENEUTIL_STATICGEOMETRYPREWARM_H

#include <osg/Geometry>
#include <osg/BufferObject>
#include <osg/RenderInfo>
#include <osg/State>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace SceneUtil::StaticGeometryPrewarm
{
    inline bool enabled()
    {
        static const bool value=[] { const char* v=std::getenv("OPENMW_P9_STATIC_PREWARM");
            return v && std::strcmp(v,"1")==0; }();
        return value;
    }
    struct Result { std::uint64_t compiled=0,clean=0,skipped=0,bytes=0; };
    class Capture
    {
    public:
        static Capture& instance() { static Capture value;return value; }
        void record(const Result& r,double ms)
        {
            mCalls.fetch_add(1,std::memory_order_relaxed);
            mCompiled.fetch_add(r.compiled,std::memory_order_relaxed);
            mClean.fetch_add(r.clean,std::memory_order_relaxed);
            mSkipped.fetch_add(r.skipped,std::memory_order_relaxed);
            mBytes.fetch_add(r.bytes,std::memory_order_relaxed);
            mNs.fetch_add(static_cast<std::uint64_t>(ms*1000000.),std::memory_order_relaxed);
        }
        ~Capture()
        {
            if(mPath.empty()) return;
            try { std::ofstream out(mPath);
                out<<"calls="<<mCalls<<"\ncompiled_buffers="<<mCompiled<<"\nclean_buffers="<<mClean
                    <<"\nskipped_buffers="<<mSkipped<<"\nadmitted_bytes="<<mBytes
                    <<"\ncpu_ms="<<static_cast<double>(mNs.load())/1000000.
                    <<"\nscope=shared_static_buffer_precompile_not_live_pose_or_complete_actor_material_readiness\n";
            } catch (...) {}
        }
    private:
        Capture() { if(const char* p=std::getenv("OPENMW_P9_STATIC_PREWARM_FILE"))mPath=p; }
        std::string mPath;
        std::atomic<std::uint64_t> mCalls{0},mCompiled{0},mClean{0},mSkipped{0},mBytes{0},mNs{0};
    };

    // Run only from the existing ICO's context-owning compile operation. The
    // internal Geometry and its TemplateRef retain all source arrays. Never
    // evaluate a skeleton/morph, compile a display list, touch private pose VBOs,
    // clear revisions, or call drawImplementation here.
    inline Result compile(osg::RenderInfo& info,const osg::Geometry& geometry)
    {
        Result result;
        auto* state=info.getState();
        if(!state || !geometry.getUseVertexBufferObjects())return result;
        auto* ext=state->get<osg::GLExtensions>();
        if(!ext || !ext->isBufferObjectSupported)return result;
        const auto* vertices=geometry.getVertexArray();
        const osg::BufferObject* privatePose=vertices?vertices->getBufferObject():nullptr;
        std::array<osg::BufferObject*,32> buffers{};
        std::size_t count=0;
        const auto add=[&](osg::BufferObject* b) {
            if(!b || b==privatePose)return;
            for(std::size_t i=0;i<count;++i)if(buffers[i]==b)return;
            if(count==buffers.size()){++result.skipped;return;}
            buffers[count++]=b;
        };
        const auto array=[&](const osg::Array* a) { if(a)add(const_cast<osg::BufferObject*>(a->getBufferObject())); };
        array(geometry.getNormalArray());array(geometry.getColorArray());array(geometry.getSecondaryColorArray());
        array(geometry.getFogCoordArray());
        for(const auto& a:geometry.getTexCoordArrayList())array(a);
        for(const auto& a:geometry.getVertexAttribArrayList())array(a);
        for(const auto& primitive:geometry.getPrimitiveSetList())
            if(const auto* elements=primitive->getDrawElements())
                add(const_cast<osg::ElementBufferObject*>(elements->getElementBufferObject()));
        for(std::size_t i=0;i<count;++i)
        {
            auto* b=buffers[i];const std::uint64_t bytes=b->computeRequiredBufferSize();
            if(b->getUsage()!=GL_STATIC_DRAW_ARB || (b->getTarget()!=GL_ARRAY_BUFFER_ARB
                && b->getTarget()!=GL_ELEMENT_ARRAY_BUFFER_ARB) || bytes==0 || bytes>4u*1024u*1024u
                || result.bytes+bytes>8u*1024u*1024u)
            { ++result.skipped;continue; }
            bool mutableData=false;
            for(unsigned j=0;j<b->getNumBufferData();++j)
                if(!b->getBufferData(j) || b->getBufferData(j)->getDataVariance()==osg::Object::DYNAMIC)
                    mutableData=true;
            if(mutableData) { ++result.skipped;continue; }
            auto* gl=b->getOrCreateGLBufferObject(state->getContextID());
            if(gl->isDirty()) { gl->compileBuffer();++result.compiled;result.bytes+=bytes; }
            else ++result.clean;
        }
        // Manual compileBuffer binds GL directly; leave OSG's binding caches
        // invalidated. Attribute pointers and GL identities are not rewritten.
        state->unbindVertexBufferObject();state->unbindElementBufferObject();
        return result;
    }
    inline void prepare(osg::RenderInfo& info,const osg::Geometry* geometry)
    {
        if(!enabled() || !geometry)return;
        const auto begin=std::chrono::steady_clock::now();
        const Result result=compile(info,*geometry);
        Capture::instance().record(result,std::chrono::duration<double,std::milli>(
            std::chrono::steady_clock::now()-begin).count());
    }
}
#endif
