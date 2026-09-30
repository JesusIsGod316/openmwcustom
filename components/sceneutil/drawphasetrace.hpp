#ifndef OPENMW_SCENEUTIL_DRAWPHASETRACE_H
#define OPENMW_SCENEUTIL_DRAWPHASETRACE_H

#include "glcalltrace.hpp"
#include <osg/FrameStamp>
#include <osg/GraphicsContext>
#include <stdexcept>
#include <osg/Geometry>
#include <osg/State>
#include <osg/Texture2D>
#include <osg/Version>
#include <osgUtil/CullVisitor>
#include <osgUtil/RenderLeaf>
#include <osgUtil/StateGraph>
#include <osgViewer/Renderer>
#include <osgViewer/Viewer>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <typeinfo>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <map>
#include <algorithm>

namespace SceneUtil::DrawPhaseTrace
{
    enum CameraBucket : unsigned
    {
        CameraUnknown = 0,
        CameraScene,
        CameraRefraction,
        CameraReflection,
        CameraShadow,
        CameraTerrainComposite,
        CameraBucketCount
    };

    inline const char* cameraBucketName(unsigned bucket)
    {
        static constexpr const char* names[] = {
            "unknown", "scene", "refraction", "reflection", "shadow", "terrain_composite"
        };
        return bucket < CameraBucketCount ? names[bucket] : "invalid";
    }

    inline unsigned classifyCamera(const std::string& name)
    {
        if (name == "SceneCam") return CameraScene;
        if (name == "RefractionCamera") return CameraRefraction;
        if (name == "ReflectionCamera") return CameraReflection;
        if (name == "ShadowCamera") return CameraShadow;
        if (name == "TerrainCompositeMapCamera") return CameraTerrainComposite;
        return CameraUnknown;
    }

    // Diagnostic-only. Timers are CPU envelopes and may include driver waits;
    // none is a GPU timer. No output, formatting or allocation during a draw.
    class Capture
    {
    public:
        struct ResourceRow
        {
            unsigned firstFrame=0, lastFrame=0, context=~0u, unit=0, imageRevision=0, scope=0;
            std::uint64_t texture=0, image=0, stateSet=0, submitCamera=0, bytes=0, labelHash=0;
            std::array<char,32> textureClass{};
            std::array<char,256> filename{};
            std::array<char,128> semanticRole{};
        };
        void catalogResource(const ResourceRow& row)
        {
            if (!mResources) return;
            std::lock_guard<std::mutex> lock(mResourceMutex);
            const ResourceKey key{row.texture,row.image,row.stateSet,row.submitCamera,row.labelHash,
                row.context,row.unit,row.imageRevision,row.scope};
            const auto existing=mResourceIndex.find(key);
            if(existing!=mResourceIndex.end())
            {
                auto& value=(*mResources)[existing->second];
                value.firstFrame=std::min(value.firstFrame,row.firstFrame);
                value.lastFrame=std::max(value.lastFrame,row.lastFrame);
                return;
            }
            if(mResourceIndex.size()>=ResourceCapacity)
            {
                mResourceDropped.fetch_add(1,std::memory_order_relaxed);
                return;
            }
            const std::size_t index=mResourceIndex.size();
            (*mResources)[index]=row;
            mResourceIndex.emplace(key,index);
        }
        void noteResourceUncovered() { mResourceDropped.fetch_add(1,std::memory_order_relaxed); }
        std::uint64_t resourceDropped() const { return mResourceDropped.load(std::memory_order_relaxed); }
        std::size_t resourceCount() const
        {
            std::lock_guard<std::mutex> lock(mResourceMutex);
            return mResourceIndex.size();
        }
        ResourceRow resource(std::size_t index) const
        {
            std::lock_guard<std::mutex> lock(mResourceMutex);
            return mResources && index<mResourceIndex.size() ? (*mResources)[index] : ResourceRow{};
        }
        struct Row
        {
            unsigned frame = 0, context = 0;
            std::uint64_t drawable = 0, camera = 0, stateSet = 0, nodePathHash = 0;
            std::uint64_t submitCamera = 0;
            std::uint64_t texture0Bytes = 0, texture1Bytes = 0;
            std::int64_t startNs = 0;
            double matricesMs = 0, stateMs = 0, drawMs = 0, retireMs = 0;
            std::uint32_t vertices = 0, primitiveSets = 0, cameraBucket = CameraUnknown;
            bool dynamic = false;
            std::array<char, 96> name{};
            std::array<char, 64> ownerName{};
            std::array<char, 32> ownerClass{};
            std::array<char, 48> cameraName{};
            std::array<char, 48> submitCameraName{};
            std::array<char, 32> drawableClass{};
            std::array<char, 128> texture0{};
            std::array<char, 128> texture1{};
        };
        struct Totals
        {
            std::uint64_t calls = 0;
            double matricesMs = 0, stateMs = 0, drawMs = 0, retireMs = 0;
        };
        struct FrameRow { unsigned frame=0,context=0; Totals totals; };
        void outer(unsigned context, unsigned kind, double ms, std::int64_t startNs)
        {
            if(context<16) { ++mOuterCalls[context]; mOuterMs[context]+=ms;
                mOuterKind[context]=kind;
                const auto i=mOuterCount.fetch_add(1,std::memory_order_relaxed);
                if(i<FrameCapacity) (*mOuterRows)[i]={context,kind,mCurrentFrame[context].frame,startNs,ms};
            }
        }
        const std::string& path() const { return mPath; }
        static Capture& instance() { static Capture value; return value; }
        bool enabled() const { return static_cast<bool>(mRows); }
        bool reserveVisitor()
        {
            const unsigned n = mVisitors.fetch_add(1);
            return enabled() && n < 8;
        }
        void noteUncovered(unsigned leaves) { mUncovered.fetch_add(leaves, std::memory_order_relaxed); }
        void append(const Row& row)
        {
            if (!enabled()) return;
            if (row.context < mTotals.size())
            {
                // Each OSG context has a single draw owner. Different contexts
                // use different elements, and totals are read after shutdown.
                auto& f=mCurrentFrame[row.context];
                if(f.totals.calls && f.frame!=row.frame) saveFrame(f);
                if(!f.totals.calls) { f.frame=row.frame;f.context=row.context; }
                ++f.totals.calls; f.totals.matricesMs+=row.matricesMs; f.totals.stateMs+=row.stateMs;
                f.totals.drawMs+=row.drawMs; f.totals.retireMs+=row.retireMs;
                auto& t = mTotals[row.context];
                ++t.calls; t.matricesMs += row.matricesMs; t.stateMs += row.stateMs;
                t.drawMs += row.drawMs; t.retireMs += row.retireMs;
                if (row.cameraBucket < CameraBucketCount)
                {
                    auto& camera = mCameraTotals[row.context][row.cameraBucket];
                    ++camera.calls; camera.matricesMs += row.matricesMs; camera.stateMs += row.stateMs;
                    camera.drawMs += row.drawMs; camera.retireMs += row.retireMs;
                }
            }
            if (row.matricesMs + row.stateMs + row.drawMs + row.retireMs < .25) return;
            const auto index = mCount.fetch_add(1, std::memory_order_relaxed);
            if (index >= Capacity) return;
            // The row owns all metadata. In particular, do not dereference a
            // drawable after the dynamic-completion decrement releases update.
            (*mRows)[index] = row;
        }
        Totals totals(unsigned context) const { return context < mTotals.size() ? mTotals[context] : Totals{}; }
        std::size_t count() const { return (std::min)(mCount.load(), Capacity); }
        Row row(std::size_t index) const { return enabled() && index < count() ? (*mRows)[index] : Row{}; }
        ~Capture()
        {
            if (!enabled()) return;
            try
            {
                std::ofstream out(mPath);
                out << "frame,context,drawable,matrices_ms,state_ms,draw_ms,retire_ms,dynamic,name,camera,stateset,start_ns,"
                    << "camera_name,drawable_class,camera_bucket,node_path_hash,owner_name,owner_class,vertices,primitive_sets,"
                    << "texture0,texture0_bytes,texture1,texture1_bytes,submit_camera,submit_camera_name\n" << std::setprecision(8);
                for (std::size_t i = 0; i < count(); ++i)
                {
                    const auto& r = (*mRows)[i];
                    out << r.frame << ',' << r.context << ',' << r.drawable << ',' << r.matricesMs
                        << ',' << r.stateMs << ',' << r.drawMs << ',' << r.retireMs << ',' << r.dynamic << ",\"";
                    for (char c : r.name)
                    {
                        if (!c) break;
                        if (c == '"') out << '"';
                        out << c;
                    }
                    out << "\"," << r.camera << ',' << r.stateSet << ',' << r.startNs << ",\"";
                    for(char c:r.cameraName) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\",\"";
                    for(char c:r.drawableClass) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\","<<r.cameraBucket<<','<<r.nodePathHash<<",\"";
                    for(char c:r.ownerName) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\",\"";
                    for(char c:r.ownerClass) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\","<<r.vertices<<','<<r.primitiveSets<<",\"";
                    for(char c:r.texture0) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\","<<r.texture0Bytes<<",\"";
                    for(char c:r.texture1) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\","<<r.texture1Bytes<<','<<r.submitCamera<<",\"";
                    for(char c:r.submitCameraName) { if(!c)break;if(c=='"')out<<'"';out<<c; }
                    out<<"\"\n";
                }
                for(auto& frame:mCurrentFrame) if(frame.totals.calls) saveFrame(frame);
                std::ofstream frames(mPath+".frames.csv");
                frames<<"frame,context,leaves,matrices_ms,state_ms,draw_ms,retire_ms\n"<<std::setprecision(8);
                for(std::size_t i=0;i<(std::min)(mFrameCount.load(),FrameCapacity);++i)
                {
                    const auto& f=(*mFrames)[i];const auto& t=f.totals;
                    frames<<f.frame<<','<<f.context<<','<<t.calls<<','<<t.matricesMs<<','<<t.stateMs
                        <<','<<t.drawMs<<','<<t.retireMs<<'\n';
                }
                std::ofstream outerFile(mPath+".renderer.csv");
                outerFile<<"context,kind,last_leaf_frame,start_ns,wall_ms\n";
                for(std::size_t i=0;i<(std::min)(mOuterCount.load(),FrameCapacity);++i)
                { const auto& x=(*mOuterRows)[i];outerFile<<x.context<<','<<x.kind<<','<<x.frame<<','<<x.startNs<<','<<x.ms<<'\n'; }
                std::uint64_t calls=0;for(const auto& t:mTotals)calls+=t.calls;
                std::ofstream status(mPath + ".status.txt");
                const auto n = mCount.load();
                status << "rows_dropped=" << (n > Capacity ? n - Capacity : 0)
                    << "\nuninstrumented_pool_overflow=" << mUncovered.load()
                    << "\nvisitor_instances=" << mVisitors.load()
                    << "\nframe_rows_dropped=" << (mFrameCount>FrameCapacity?mFrameCount-FrameCapacity:0)
                    << "\nvalid_leaf_capture=" << (calls && !mUncovered.load() && mCount<=Capacity && mFrameCount<=FrameCapacity ? 1:0)
                    << "\nrenderer_rows_dropped=" << (mOuterCount>FrameCapacity?mOuterCount-FrameCapacity:0)
                    << "\nresource_catalog_rows=" << mResourceIndex.size()
                    << "\nresource_catalog_dropped_attempts=" << mResourceDropped.load()
                    << "\nresource_catalog_scope=effective_cull_materials_and_terrain_composite_producer_layers"
                    << "\nresource_source_image_scope=first_source_image_per_effective_texture_attribute"
                    << "\nresource_unknown_context=4294967295_resource_scope_1_cull_2_composite_producer"
                    << "\nscope=instrumented_render_leaves_not_renderstage_setup_swap_or_gpu_time\n"
                    << "renderer_kind=1_draw_including_queue_wait_6_cull_draw_7_compile_nested_do_not_sum\n"
                    << "last_leaf_frame=last_observed_not_a_claim_all_outer_work_belongs_to_that_frame\n";
                for(unsigned i=0;i<16;++i) if(mOuterCalls[i])
                    status<<"outer_context="<<i<<" calls="<<mOuterCalls[i]<<" kind="<<mOuterKind[i]
                        <<" cpu_ms="<<mOuterMs[i]<<" includes_renderer_queue_wait_not_same_as_OSG_draw_timer\n";
                for (unsigned i = 0; i < mTotals.size(); ++i)
                {
                    const auto& t = mTotals[i];
                    if (t.calls) status << "context=" << i << " calls=" << t.calls << " matrices_ms=" << t.matricesMs
                        << " state_ms=" << t.stateMs << " draw_ms=" << t.drawMs << " retire_ms=" << t.retireMs << '\n';
                }
                for (unsigned i = 0; i < CameraBucketCount; ++i)
                {
                    Totals t;
                    for(const auto& context : mCameraTotals) {
                        const auto& camera=context[i];t.calls+=camera.calls;t.matricesMs+=camera.matricesMs;
                        t.stateMs+=camera.stateMs;t.drawMs+=camera.drawMs;t.retireMs+=camera.retireMs;
                    }
                    if (t.calls) status << "camera_bucket=" << cameraBucketName(i) << " calls=" << t.calls
                        << " matrices_ms=" << t.matricesMs << " state_ms=" << t.stateMs
                        << " draw_ms=" << t.drawMs << " retire_ms=" << t.retireMs << '\n';
                }
                std::ofstream resources(mPath+".resources.csv");
                resources<<"first_frame,last_frame,context,texture_unit,texture,image,image_revision,stateset,submit_camera,"
                    "bytes,scope,texture_class,filename,semantic_role\n";
                auto quoted=[&resources](const auto& value) {
                    resources<<'"';
                    for(char character:value) { if(!character)break;if(character=='"')resources<<'"';resources<<character; }
                    resources<<'"';
                };
                for(std::size_t i=0;i<mResourceIndex.size();++i)
                {
                    const auto& row=(*mResources)[i];
                    resources<<row.firstFrame<<','<<row.lastFrame<<','<<row.context<<','<<row.unit<<','<<row.texture<<','
                        <<row.image<<','<<row.imageRevision<<','<<row.stateSet<<','<<row.submitCamera<<','<<row.bytes<<','
                        <<row.scope<<',';
                    quoted(row.textureClass);resources<<',';quoted(row.filename);resources<<',';
                    quoted(row.semanticRole);resources<<'\n';
                }
            }
            catch (...) {}
        }
    private:
        static constexpr std::size_t Capacity = 32768, FrameCapacity = 65536;
        static constexpr std::size_t ResourceCapacity=32768;
        struct ResourceKey
        {
            std::uint64_t texture,image,stateSet,submitCamera,labelHash;
            unsigned context,unit,imageRevision,scope;
            friend bool operator==(const ResourceKey&,const ResourceKey&)=default;
        };
        struct ResourceKeyHash
        {
            std::size_t operator()(const ResourceKey& key) const
            {
                std::uint64_t hash=1469598103934665603ull;
                for(const auto value:{key.texture,key.image,key.stateSet,key.submitCamera,key.labelHash,
                    std::uint64_t(key.context),std::uint64_t(key.unit),std::uint64_t(key.imageRevision),std::uint64_t(key.scope)})
                { hash^=value;hash*=1099511628211ull; }
                return static_cast<std::size_t>(hash);
            }
        };
        void saveFrame(FrameRow& f)
        {
            const auto i=mFrameCount.fetch_add(1,std::memory_order_relaxed);
            if(i<FrameCapacity) (*mFrames)[i]=f;
            f.totals={};
        }
        Capture()
        {
            if (const char* value = std::getenv("OPENMW_P9_LEAF_TRACE_FILE"); value && *value)
            {
                mPath = value;
                mRows = std::make_unique<std::array<Row, Capacity>>();
                mFrames=std::make_unique<std::array<FrameRow,FrameCapacity>>();
                mOuterRows=std::make_unique<std::array<OuterRow,FrameCapacity>>();
                mResources=std::make_unique<std::array<ResourceRow,ResourceCapacity>>();
                GLCallTrace::Capture::instance().enable(mPath);
            }
        }
        struct OuterRow { unsigned context=0,kind=0,frame=0;std::int64_t startNs=0;double ms=0; };
        std::unique_ptr<std::array<OuterRow,FrameCapacity>> mOuterRows;
        std::atomic<std::size_t> mOuterCount{0};
        std::string mPath;
        std::unique_ptr<std::array<Row, Capacity>> mRows;
        std::atomic<std::size_t> mCount{0};
        std::atomic<std::uint64_t> mUncovered{0};
        std::atomic<unsigned> mVisitors{0};
        std::array<Totals, 16> mTotals{};
        std::array<std::array<Totals, CameraBucketCount>,16> mCameraTotals{};
        std::unique_ptr<std::array<FrameRow,FrameCapacity>> mFrames;
        std::array<FrameRow,16> mCurrentFrame{};
        std::atomic<std::size_t> mFrameCount{0};
        std::array<std::uint64_t,16> mOuterCalls{};
        std::array<double,16> mOuterMs{};
        std::array<unsigned,16> mOuterKind{};
        std::unique_ptr<std::array<ResourceRow,ResourceCapacity>> mResources;
        mutable std::mutex mResourceMutex;
        std::unordered_map<ResourceKey,std::size_t,ResourceKeyHash> mResourceIndex;
        std::atomic<std::uint64_t> mResourceDropped{0};
    };

    // Called during cull or terrain CPU production, never from render/GL
    // hooks. Build the effective root-to-leaf attributes and sampler bindings,
    // including OSG OVERRIDE/PROTECTED semantics, instead of cataloguing every
    // ancestor texture as though it were the texture used by this submission.
    inline void catalogStateSets(const std::vector<const osg::StateSet*>& stateSets,
        unsigned frame,unsigned context,std::uint64_t camera,unsigned scope,Capture::Row* descriptor=nullptr)
    {
        auto& capture=Capture::instance();
        if(!capture.enabled()) return;
        struct Binding { const osg::Texture* texture=nullptr;const osg::StateSet* stateSet=nullptr;unsigned flags=0; };
        std::map<unsigned,Binding> textures;
        std::map<std::string,osg::StateSet::RefUniformPair> uniforms;
        constexpr std::size_t MaxBindings=512;
        for(const auto* stateSet:stateSets)
        {
            if(!stateSet) continue;
            const auto& attributes=stateSet->getTextureAttributeList();
            for(unsigned unit=0;unit<attributes.size();++unit)
            {
                const auto* pair=stateSet->getTextureAttributePair(unit,osg::StateAttribute::TEXTURE);
                const auto* texture=pair && pair->first ? pair->first->asTexture() : nullptr;
                if(!texture) continue;
                auto existing=textures.find(unit);
                if(existing!=textures.end() && (existing->second.flags & osg::StateAttribute::OVERRIDE)
                    && !(pair->second & osg::StateAttribute::PROTECTED)) continue;
                if(existing==textures.end() && textures.size()>=MaxBindings) { capture.noteResourceUncovered();continue; }
                textures[unit]={texture,stateSet,pair->second};
            }
            for(const auto& [name,pair]:stateSet->getUniformList())
            {
                auto existing=uniforms.find(name);
                if(existing!=uniforms.end() && (existing->second.second & osg::StateAttribute::OVERRIDE)
                    && !(pair.second & osg::StateAttribute::PROTECTED)) continue;
                if(existing==uniforms.end() && uniforms.size()>=MaxBindings) { capture.noteResourceUncovered();continue; }
                uniforms[name]=pair;
            }
        }
        for(const auto& [unit,binding]:textures)
        {
            const auto* image=binding.texture->getNumImages()>0 ? binding.texture->getImage(0) : nullptr;
            Capture::ResourceRow row;
            row.firstFrame=row.lastFrame=frame;row.context=context;row.unit=unit;row.scope=scope;
            row.texture=reinterpret_cast<std::uintptr_t>(binding.texture);
            row.image=reinterpret_cast<std::uintptr_t>(image);
            row.imageRevision=image ? image->getModifiedCount() : 0;
            row.bytes=image ? image->getTotalSizeInBytesIncludingMipmaps() : 0;
            row.stateSet=reinterpret_cast<std::uintptr_t>(binding.stateSet);row.submitCamera=camera;
            const std::string& filename=image && !image->getFileName().empty() ? image->getFileName() : binding.texture->getName();
            std::string role;
            for(const auto& [name,pair]:uniforms)
            {
                if(!pair.first || pair.first->getType()!=osg::Uniform::SAMPLER_2D) continue;
                if(pair.first->getNumElements()>512) capture.noteResourceUncovered();
                for(unsigned element=0;element<std::min(512u,pair.first->getNumElements());++element)
                {
                    int boundUnit=-1;
                    if(pair.first->getElement(element,boundUnit) && boundUnit>=0 && static_cast<unsigned>(boundUnit)==unit)
                    {
                        std::string bindingName=name;
                        if(pair.first->getNumElements()>1) bindingName+='['+std::to_string(element)+']';
                        if(role.size()+bindingName.size()+1>=row.semanticRole.size())
                        { capture.noteResourceUncovered();continue; }
                        if(!role.empty()) role+='|';
                        role+=bindingName;
                    }
                }
            }
            if(role.empty() && scope==2 && unit<=1)
                role=unit==0 ? "terrain_composite_diffuse" : "terrain_composite_blendmap";
            if(role.empty()) role="unknown";
            auto copyText=[](auto& output,const char* value) {
                std::memcpy(output.data(),value,std::min(std::strlen(value),output.size()-1));
            };
            copyText(row.textureClass,binding.texture->className());
            copyText(row.filename,filename.c_str());copyText(row.semanticRole,role.c_str());
            if(filename.size()>=row.filename.size()) capture.noteResourceUncovered();
            if(std::strlen(binding.texture->className())>=row.textureClass.size()) capture.noteResourceUncovered();
            if(binding.texture->getNumImages()>1) capture.noteResourceUncovered();
            row.labelHash=1469598103934665603ull;
            for(const auto* value:std::array<const std::string*,2>{&filename,&role})
            {
                row.labelHash^=value->size();row.labelHash*=1099511628211ull;
                for(unsigned char character:*value) { row.labelHash^=character;row.labelHash*=1099511628211ull; }
            }
            for(const char* character=binding.texture->className();*character;++character)
            { row.labelHash^=static_cast<unsigned char>(*character);row.labelHash*=1099511628211ull; }
            capture.catalogResource(row);
            if(descriptor)
            {
                if(row.bytes>=descriptor->texture0Bytes)
                {
                    descriptor->texture1Bytes=descriptor->texture0Bytes;descriptor->texture1=descriptor->texture0;
                    descriptor->texture0Bytes=row.bytes;descriptor->texture0={};
                    copyText(descriptor->texture0,filename.c_str());
                }
                else if(row.bytes>=descriptor->texture1Bytes)
                {
                    descriptor->texture1Bytes=row.bytes;descriptor->texture1={};
                    copyText(descriptor->texture1,filename.c_str());
                }
            }
        }
    }

    class Leaf final : public osgUtil::RenderLeaf
    {
    public:
        Leaf(osg::Drawable* drawable, osg::RefMatrix* projection, osg::RefMatrix* modelview)
            : osgUtil::RenderLeaf(drawable, projection, modelview) {}
        Capture::Row descriptor;
        std::uint64_t nodePathHash = 0;
        std::uint32_t vertices = 0, primitiveSets = 0, cameraBucket = CameraUnknown;
        std::array<char, 64> ownerName{};
        std::array<char, 32> ownerClass{};
        std::uint64_t submitCamera = 0;
        std::array<char, 48> submitCameraName{};
        void render(osg::RenderInfo& info, osgUtil::RenderLeaf* previous) override
        {
            auto& capture = Capture::instance();
            if (!capture.enabled()) { osgUtil::RenderLeaf::render(info, previous); return; }
            osg::State& state = *info.getState();
            if (state.getAbortRendering()) return;
            using Clock = std::chrono::steady_clock;
            const auto start = Clock::now();
            const auto* stamp = state.getFrameStamp();
            const unsigned frame=stamp?stamp->getFrameNumber():0;
            GLCallTrace::Scope scope({state.getContextID(),frame,GLCallTrace::Matrices,
                reinterpret_cast<std::uintptr_t>(_drawable.get()), &state,
                reinterpret_cast<std::uintptr_t>(info.getCurrentCamera())});
            state.applyProjectionMatrix(_projection.get());
            state.applyModelViewMatrix(_modelview.get());
            const auto matrices = Clock::now();
            GLCallTrace::location.phase=GLCallTrace::State;

            // Preserve pinned OSG 3.6.5 RenderLeaf's state-graph application
            // order, generated matrix uniforms, draw dispatch and dynamic-count
            // decrement. In particular, same-StateGraph draws do not reapply it.
            // Reference: OpenSceneGraph-3.6.5/src/osgUtil/RenderLeaf.cpp.
            if (!previous)
            {
                osgUtil::StateGraph::moveStateGraph(state, nullptr, _parent->_parent);
                state.apply(_parent->getStateSet());
            }
            else if (previous->_parent->_parent != _parent->_parent)
            {
                osgUtil::StateGraph::moveStateGraph(state, previous->_parent->_parent, _parent->_parent);
                state.apply(_parent->getStateSet());
            }
            else if (previous->_parent != _parent)
                state.apply(_parent->getStateSet());
            if (state.getUseModelViewAndProjectionUniforms()) state.applyModelViewAndProjectionUniformsIfRequired();
            const auto applied = Clock::now();
            GLCallTrace::location.phase=GLCallTrace::Drawable;
            _drawable->draw(info);
            const auto drawn = Clock::now();
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            Capture::Row row = descriptor;
            row.frame = frame;
            row.startNs=std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count();
            row.camera=reinterpret_cast<std::uintptr_t>(info.getCurrentCamera());
            row.stateSet=reinterpret_cast<std::uintptr_t>(_parent->getStateSet());
            row.context = state.getContextID();
            row.matricesMs = ms(start, matrices); row.stateMs = ms(matrices, applied);
            row.drawMs = ms(applied, drawn); row.dynamic = _dynamic;
            row.drawable = reinterpret_cast<std::uintptr_t>(_drawable.get());
            row.nodePathHash = nodePathHash;
            row.vertices = vertices;
            row.primitiveSets = primitiveSets;
            row.cameraBucket = cameraBucket;
            row.submitCamera = submitCamera;
            row.submitCameraName = submitCameraName;
            row.ownerName = ownerName;
            row.ownerClass = ownerClass;
            GLCallTrace::location.phase=GLCallTrace::Retire;
            const auto retireStart = Clock::now();
            if (_dynamic) state.decrementDynamicObjectCount();
            const auto retired = Clock::now();
            row.retireMs = ms(retireStart, retired);
            capture.append(row);
        }
    };

    class CullVisitor final : public osgUtil::CullVisitor
    {
    public:
        explicit CullVisitor(const osgUtil::CullVisitor& other) : osgUtil::CullVisitor(other)
        {
            if (!Capture::instance().reserveVisitor()) return;
            osg::ref_ptr<osg::Geometry> dummy = new osg::Geometry;
            _reuseRenderLeafList.clear();
            _reuseRenderLeafList.reserve(PoolSize);
            for (unsigned i = 0; i < PoolSize; ++i)
            {
                osg::ref_ptr<Leaf> leaf = new Leaf(dummy, nullptr, nullptr);
                leaf->reset();
                _reuseRenderLeafList.push_back(leaf);
            }
            mInstrumented = true;
        }
        CullVisitor(const CullVisitor& other) : CullVisitor(static_cast<const osgUtil::CullVisitor&>(other)) {}
        using osgUtil::CullVisitor::clone;
        osgUtil::CullVisitor* clone() const override { return new CullVisitor(*this); }
        using osgUtil::CullVisitor::apply;
        void apply(osg::Drawable& drawable) override
        {
            const unsigned first = _currentReuseRenderLeafIndex;
            osgUtil::CullVisitor::apply(drawable);
            stampSubmissions(first, drawable);
        }
        unsigned submissionIndex() const { return _currentReuseRenderLeafIndex; }
        // TerrainDrawable submits directly and bypasses apply(Drawable). Both
        // paths call this while their real node path and camera are still live.
        void stampSubmissions(unsigned first, osg::Drawable& drawable)
        {
            for (unsigned i = first; i < _currentReuseRenderLeafIndex && i < (mInstrumented ? PoolSize : 0); ++i)
            {
                auto* leaf = static_cast<Leaf*>(_reuseRenderLeafList[i].get());
                leaf->nodePathHash = 1469598103934665603ull;
                leaf->vertices = 0;
                leaf->primitiveSets = 0;
                leaf->cameraBucket = CameraUnknown;
                leaf->ownerName = {};
                leaf->ownerClass = {};
                leaf->descriptor = {};
                auto copyText = [](auto& destination, const auto& text) {
                    const auto length = (std::min)(std::strlen(text), destination.size()-1);
                    std::memcpy(destination.data(), text, length);
                };
                copyText(leaf->descriptor.name, drawable.getName().c_str());
                copyText(leaf->descriptor.drawableClass, drawable.className());
                leaf->submitCamera = 0;
                leaf->submitCameraName = {};

                if (const osg::Camera* camera = getCurrentCamera())
                {
                    leaf->cameraBucket = classifyCamera(camera->getName());
                    leaf->submitCamera = reinterpret_cast<std::uintptr_t>(camera);
                    const auto& name = camera->getName();
                    std::memcpy(leaf->submitCameraName.data(), name.data(),
                        (std::min)(name.size(), leaf->submitCameraName.size()-1));
                }

                std::vector<const osg::StateSet*> materialPath;
                for(auto* graph=leaf->_parent;graph;graph=graph->_parent)
                    if(graph->getStateSet()) materialPath.push_back(graph->getStateSet());
                std::reverse(materialPath.begin(),materialPath.end());
                const auto* camera=getCurrentCamera();
                const auto* context=camera ? camera->getGraphicsContext() : nullptr;
                const auto* stamp=getFrameStamp();
                catalogStateSets(materialPath,stamp ? stamp->getFrameNumber() : 0,
                    context && context->getState() ? context->getState()->getContextID() : ~0u,
                    leaf->submitCamera,1,&leaf->descriptor);

                const auto& path = getNodePath();
                for (osg::Node* node : path)
                {
                    leaf->nodePathHash ^= static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(node));
                    leaf->nodePathHash *= 1099511628211ull;
                }
                for (auto it = path.rbegin(); it != path.rend(); ++it)
                {
                    osg::Node* node = *it;
                    if (!node || node->getName().empty())
                        continue;
                    const auto& name = node->getName();
                    std::memcpy(leaf->ownerName.data(), name.data(),
                        (std::min)(name.size(), leaf->ownerName.size() - 1));
                    const char* cls = node->className();
                    if (cls)
                        std::memcpy(leaf->ownerClass.data(), cls,
                            (std::min)(std::strlen(cls), leaf->ownerClass.size() - 1));
                    break;
                }

                if (osg::Geometry* geometry = drawable.asGeometry())
                {
                    if (const osg::Array* array = geometry->getVertexArray())
                        leaf->vertices = array->getNumElements();
                    leaf->primitiveSets = geometry->getNumPrimitiveSets();
                }
            }
        }
        void reset() override
        {
            if (_currentReuseRenderLeafIndex > (mInstrumented ? PoolSize : 0))
                Capture::instance().noteUncovered(_currentReuseRenderLeafIndex - (mInstrumented ? PoolSize : 0));
            osgUtil::CullVisitor::reset();
        }
    private:
        static constexpr unsigned PoolSize = 8192;
        bool mInstrumented = false;
    };

    class SubmissionScope
    {
    public:
        SubmissionScope(osgUtil::CullVisitor& visitor, osg::Drawable& drawable)
            : mVisitor(Capture::instance().enabled() ? dynamic_cast<CullVisitor*>(&visitor) : nullptr)
            , mDrawable(drawable), mFirst(mVisitor ? mVisitor->submissionIndex() : 0) {}
        ~SubmissionScope() { if (mVisitor) mVisitor->stampSubmissions(mFirst, mDrawable); }
    private:
        CullVisitor* mVisitor;
        osg::Drawable& mDrawable;
        unsigned mFirst;
    };

    inline unsigned install(osgViewer::Viewer& viewer)
    {
        if (!Capture::instance().enabled() || viewer.areThreadsRunning()
            || std::strcmp(osgGetVersion(), "3.6.5") != 0) return 0;
        auto* renderer = dynamic_cast<osgViewer::Renderer*>(viewer.getCamera()->getRenderer());
        if (!renderer) return 0;
        unsigned installed = 0;
        for (unsigned i = 0; i < 2; ++i)
        {
            auto* view = renderer->getSceneView(i);
            auto* cv = view ? view->getCullVisitor() : nullptr;
            if (!cv || typeid(*cv) != typeid(osgUtil::CullVisitor)) continue;
            view->setCullVisitor(new CullVisitor(*cv));
            ++installed;
        }
        return installed;
    }
    class Renderer final : public osgViewer::Renderer
    {
    public:
        explicit Renderer(osg::Camera* camera) : osgViewer::Renderer(camera) {}
        void draw() override { run(GLCallTrace::RendererDraw,[this]{osgViewer::Renderer::draw();}); }
        void cull_draw() override { run(GLCallTrace::CullDraw,[this]{osgViewer::Renderer::cull_draw();}); }
        void compile() override { run(GLCallTrace::Compile,[this]{osgViewer::Renderer::compile();}); }
    private:
        template<typename F> void run(unsigned phase,F fn)
        {
            osg::Camera* camera=_camera.get();
            auto* gc=camera?camera->getGraphicsContext():nullptr;
            auto* state=gc?gc->getState():nullptr;
            if(!state) { fn();return; }
            const unsigned context=state->getContextID();
            auto* extensions=state->get<osg::GLExtensions>();
            if(extensions && !GLCallTrace::install(context,*extensions))
                throw std::runtime_error("Phase 9 trace: unsupported context ID; capture cannot be considered valid");
            GLCallTrace::Scope scope({context,0,phase,0});
            const auto begin=GLCallTrace::Clock::now();
            fn();
            Capture::instance().outer(context,phase,
                std::chrono::duration<double,std::milli>(GLCallTrace::Clock::now()-begin).count(),
                std::chrono::duration_cast<std::chrono::nanoseconds>(begin.time_since_epoch()).count());
        }
    };

    inline unsigned installBeforeRealize(osgViewer::Viewer& viewer)
    {
        if(!Capture::instance().enabled()) return 0;
        if(viewer.isRealized() || viewer.areThreadsRunning() || std::strcmp(osgGetVersion(),"3.6.5")!=0)
            throw std::runtime_error("Phase 9 trace must be installed on OSG 3.6.5 before Viewer::realize");
        auto* camera=viewer.getCamera();
        auto* old=camera?camera->getRenderer():nullptr;
        if(!camera || (old && typeid(*old)!=typeid(osgViewer::Renderer)))
            throw std::runtime_error("Phase 9 trace: custom renderer is unsupported; refusing an empty capture");
        // A fresh default renderer has not realized/compiled/rendered any scene.
        // Do not replace initialized renderers or mutate live queues/SceneViews.
        camera->setRenderer(new Renderer(camera));
        const unsigned installed=install(viewer);
        if(installed!=2) throw std::runtime_error("Phase 9 trace: both live SceneViews must be instrumented");
        return installed;
    }

}
#endif
