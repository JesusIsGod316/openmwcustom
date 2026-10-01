#ifndef OPENMW_SCENEUTIL_DRAWPHASETRACE_H
#define OPENMW_SCENEUTIL_DRAWPHASETRACE_H

#include "glcalltrace.hpp"
#include <components/debug/hitchtrace.hpp>
#include <osgUtil/Statistics>
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
#include <algorithm>
#include <charconv>
#include <new>
#include <span>
#include <string_view>
#include <initializer_list>

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
        static constexpr std::size_t ResourceDefaultCapacity = 131072;
        static constexpr std::size_t ResourceMinimumCapacity = 32768;
        static constexpr std::size_t ResourceMaximumCapacity = 262144;
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
                row.bytes,row.context,row.unit,row.imageRevision,row.scope};
            const auto existing=mResourceIndex.find(key);
            if(existing!=mResourceIndex.end())
            {
                auto& value=mResources[existing->second];
                value.firstFrame=std::min(value.firstFrame,row.firstFrame);
                value.lastFrame=std::max(value.lastFrame,row.lastFrame);
                return;
            }
            if(mResourceIndex.size()>=mResourceCapacity)
            {
                mResourceDropped.fetch_add(1,std::memory_order_relaxed);
                return;
            }
            const std::size_t index=mResourceIndex.size();
            try
            {
                mResourceIndex.emplace(key,index);
                mResources[index]=row;
            }
            catch(const std::bad_alloc&)
            {
                mResourceAllocationFailures.fetch_add(1,std::memory_order_relaxed);
                mResourceDropped.fetch_add(1,std::memory_order_relaxed);
            }
        }
        void noteResourceUncovered() { mResourceDropped.fetch_add(1,std::memory_order_relaxed); }
        void noteCatalogCost(std::uint64_t nanoseconds, std::size_t textures, std::size_t uniforms)
        {
            mCatalogCalls.fetch_add(1, std::memory_order_relaxed);
            mCatalogWallNs.fetch_add(nanoseconds, std::memory_order_relaxed);
            mCatalogTextures.fetch_add(textures, std::memory_order_relaxed);
            mCatalogUniforms.fetch_add(uniforms, std::memory_order_relaxed);
        }
        std::uint64_t catalogCalls() const { return mCatalogCalls.load(std::memory_order_relaxed); }
        std::uint64_t catalogTextureObservations() const { return mCatalogTextures.load(std::memory_order_relaxed); }
        std::uint64_t catalogUniformObservations() const { return mCatalogUniforms.load(std::memory_order_relaxed); }
        bool catalogOptimizedRequested() const { return mCatalogOptimizedRequested; }
        void noteCatalogCandidateScratchAllocationFailure()
        { mCatalogCandidateScratchAllocationFailures.fetch_add(1, std::memory_order_relaxed); }
        std::uint64_t catalogCandidateScratchAllocationFailures() const
        { return mCatalogCandidateScratchAllocationFailures.load(std::memory_order_relaxed); }
        std::uint64_t resourceDropped() const { return mResourceDropped.load(std::memory_order_relaxed); }
        std::size_t resourceCapacity() const { return mResourceCapacity; }
        std::size_t resourceCount() const
        {
            std::lock_guard<std::mutex> lock(mResourceMutex);
            return mResourceIndex.size();
        }
        ResourceRow resource(std::size_t index) const
        {
            std::lock_guard<std::mutex> lock(mResourceMutex);
            return mResources && index<mResourceIndex.size() ? mResources[index] : ResourceRow{};
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
            if (!enabled())
            {
                if(mResourceAllocationFailures.load())
                {
                    std::ofstream status(mPath + ".status.txt");
                    status << "valid_leaf_capture=0\nresource_catalog_allocation_failures="
                        << mResourceAllocationFailures.load() << '\n';
                }
                return;
            }
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
                    << "\nresource_catalog_capacity=" << mResourceCapacity
                    << "\nresource_catalog_maximum_capacity=" << ResourceMaximumCapacity
                    << "\nresource_catalog_descriptor_bytes=" << mResourceCapacity*sizeof(ResourceRow)
                    << "\nresource_catalog_index_entry_limit=" << mResourceCapacity
                    << "\nresource_catalog_allocation_failures=" << mResourceAllocationFailures.load()
                    << "\nresource_catalog_calls=" << mCatalogCalls.load()
                    << "\nresource_catalog_wall_ns=" << mCatalogWallNs.load()
                    << "\nresource_catalog_effective_texture_observations=" << mCatalogTextures.load()
                    << "\nresource_catalog_effective_uniform_observations=" << mCatalogUniforms.load()
                    << "\nresource_catalog_algorithm_requested=" << (mCatalogOptimizedRequested ? "ordered_merge" : "legacy")
                    << "\nresource_catalog_candidate_scratch_allocation_failures=" << mCatalogCandidateScratchAllocationFailures.load()
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
                    const auto& row=mResources[i];
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
        struct ResourceKey
        {
            std::uint64_t texture,image,stateSet,submitCamera,labelHash,bytes;
            unsigned context,unit,imageRevision,scope;
            friend bool operator==(const ResourceKey&,const ResourceKey&)=default;
        };
        struct ResourceKeyHash
        {
            std::size_t operator()(const ResourceKey& key) const
            {
                std::uint64_t hash=1469598103934665603ull;
                for(const auto value:{key.texture,key.image,key.stateSet,key.submitCamera,key.labelHash,key.bytes,
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
                if(const char* capacity=std::getenv("OPENMW_P9_RESOURCE_CAPACITY");capacity && *capacity)
                {
                    std::size_t requested=0;
                    const auto end=capacity+std::strlen(capacity);
                    const auto parsed=std::from_chars(capacity,end,requested);
                    if(parsed.ec==std::errc{} && parsed.ptr==end && requested>=ResourceMinimumCapacity
                        && requested<=ResourceMaximumCapacity)
                        mResourceCapacity=requested;
                }
                try
                {
                    mRows = std::make_unique<std::array<Row, Capacity>>();
                    mFrames=std::make_unique<std::array<FrameRow,FrameCapacity>>();
                    mOuterRows=std::make_unique<std::array<OuterRow,FrameCapacity>>();
                    mResources=std::make_unique<ResourceRow[]>(mResourceCapacity);
                    // Preserve the full observed texture/image/StateSet/camera
                    // tuple. Collapsing transient material or camera pointers
                    // would fabricate joins. Pre-reserve only when trace is on.
                    mResourceIndex.reserve(mResourceCapacity);
                    GLCallTrace::Capture::instance().enable(mPath);
                    // The candidate is diagnostic-only and independently opt-in.
                    // Do not inspect its switch on the normal family-OFF path.
                    const char* candidate = std::getenv("OPENMW_P9_CATALOG_OPTIMIZED");
                    mCatalogOptimizedRequested = candidate && std::strcmp(candidate, "1") == 0;
                }
                catch(const std::bad_alloc&)
                {
                    mResourceAllocationFailures.fetch_add(1,std::memory_order_relaxed);
                    mRows.reset();mFrames.reset();mOuterRows.reset();mResources.reset();
                }
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
        std::unique_ptr<ResourceRow[]> mResources;
        std::size_t mResourceCapacity=ResourceDefaultCapacity;
        mutable std::mutex mResourceMutex;
        std::unordered_map<ResourceKey,std::size_t,ResourceKeyHash> mResourceIndex;
        std::atomic<std::uint64_t> mResourceDropped{0};
        std::atomic<std::uint64_t> mResourceAllocationFailures{0};
        std::atomic<std::uint64_t> mCatalogCalls{0}, mCatalogWallNs{0}, mCatalogTextures{0}, mCatalogUniforms{0};
        bool mCatalogOptimizedRequested = false;
        std::atomic<std::uint64_t> mCatalogCandidateScratchAllocationFailures{0};
    };

    // Reused storage only, never a revision/pointer cache. Counts are reset for
    // every submission and all effective values are reread from live StateSets.
    // Raw references are used only during this serialized call; no ref_ptr copy,
    // name allocation or retained ownership crosses into another cull.
    struct CatalogScratch
    {
        static constexpr std::size_t MaxBindings = 512;
        struct TextureBinding
        {
            unsigned unit;
            const osg::Texture* texture;
            const osg::StateSet* stateSet;
            unsigned flags;
        };
        struct UniformBinding
        {
            const std::string* name;
            const osg::Uniform* uniform;
            unsigned flags;
        };
        // Only [0,count) is initialized or read. Insertion initializes every
        // field before increasing the next lookup range; unused slots need no
        // per-call zero-fill, including the stack-local convenience path.
        std::array<TextureBinding, MaxBindings> textures;
        std::array<UniformBinding, MaxBindings> uniforms;
        struct OptimizedStorage
        {
            struct Role
            {
                decltype(Capture::ResourceRow::semanticRole) text;
                std::size_t size;
            };
            std::array<TextureBinding, MaxBindings> mergedTextures;
            std::array<UniformBinding, MaxBindings> mergedUniforms;
            std::array<Role, MaxBindings> roles;
        };
        // Only the candidate allocates this bounded storage. It is an owner-local
        // work area, never a mutable-state memo; active counts reset every call.
        std::unique_ptr<OptimizedStorage> optimized;
        bool optimizedUnavailable = false;
        std::size_t textureCount = 0, uniformCount = 0;
        void reset() { textureCount = uniformCount = 0; }
    };

    // Wall-only detail scopes remain thresholded. The complete aggregate also
    // counts short calls, including material-path collection in the cull path.
    class CatalogDiagnostic
    {
    public:
        explicit CatalogDiagnostic(CatalogScratch& scratch)
            : mScratch(scratch), mSpan("diagnostic.resource_catalog", Debug::HitchTrace::CpuTiming::None)
            , mStart(Debug::HitchTrace::Clock::now()) { scratch.reset(); }
        ~CatalogDiagnostic()
        {
            Capture::instance().noteCatalogCost(std::chrono::duration_cast<std::chrono::nanoseconds>(
                Debug::HitchTrace::Clock::now() - mStart).count(), mScratch.textureCount, mScratch.uniformCount);
            // Inactive slots may contain old addresses, but cannot be consulted
            // by the next submission: reset runs before any effective lookup.
            mScratch.reset();
        }
    private:
        CatalogScratch& mScratch;
        Debug::HitchTrace::Scope mSpan;
        Debug::HitchTrace::Clock::time_point mStart;
    };

    template <class ResourceCapture>
    inline void catalogStateSetsLegacyImpl(std::span<const osg::StateSet* const> stateSets,
        unsigned frame, unsigned context, std::uint64_t camera, unsigned scope,
        Capture::Row* descriptor, CatalogScratch& scratch, ResourceCapture& capture)
    {
        for (const auto* stateSet : stateSets)
        {
            if (!stateSet) continue;
            const auto& attributes = stateSet->getTextureAttributeList();
            for (unsigned unit = 0; unit < attributes.size(); ++unit)
            {
                const auto* pair = stateSet->getTextureAttributePair(unit, osg::StateAttribute::TEXTURE);
                const auto* texture = pair && pair->first ? pair->first->asTexture() : nullptr;
                if (!texture) continue;
                auto end = scratch.textures.begin() + scratch.textureCount;
                auto at = std::lower_bound(scratch.textures.begin(), end, unit,
                    [](const auto& binding, unsigned value) { return binding.unit < value; });
                const bool found = at != end && at->unit == unit;
                if (found && (at->flags & osg::StateAttribute::OVERRIDE)
                    && !(pair->second & osg::StateAttribute::PROTECTED)) continue;
                if (!found)
                {
                    if (scratch.textureCount == CatalogScratch::MaxBindings)
                    { capture.noteResourceUncovered(); continue; }
                    std::move_backward(at, end, end + 1);
                }
                *at = {unit, texture, stateSet, pair->second};
                if (!found) ++scratch.textureCount;
            }
            for (const auto& [name, pair] : stateSet->getUniformList())
            {
                auto end = scratch.uniforms.begin() + scratch.uniformCount;
                auto at = std::lower_bound(scratch.uniforms.begin(), end, std::string_view(name),
                    [](const auto& binding, std::string_view value) { return std::string_view(*binding.name) < value; });
                const bool found = at != end && *at->name == name;
                if (found && (at->flags & osg::StateAttribute::OVERRIDE)
                    && !(pair.second & osg::StateAttribute::PROTECTED)) continue;
                if (!found)
                {
                    if (scratch.uniformCount == CatalogScratch::MaxBindings)
                    { capture.noteResourceUncovered(); continue; }
                    std::move_backward(at, end, end + 1);
                }
                *at = {&name, pair.first.get(), pair.second};
                if (!found) ++scratch.uniformCount;
            }
        }
        for (std::size_t index = 0; index < scratch.textureCount; ++index)
        {
            const auto& binding = scratch.textures[index];
            const unsigned unit = binding.unit;
            const auto* image = binding.texture->getNumImages() > 0 ? binding.texture->getImage(0) : nullptr;
            Capture::ResourceRow row;
            row.firstFrame = row.lastFrame = frame; row.context = context; row.unit = unit; row.scope = scope;
            row.texture = reinterpret_cast<std::uintptr_t>(binding.texture);
            row.image = reinterpret_cast<std::uintptr_t>(image);
            row.imageRevision = image ? image->getModifiedCount() : 0;
            row.bytes = image ? image->getTotalSizeInBytesIncludingMipmaps() : 0;
            row.stateSet = reinterpret_cast<std::uintptr_t>(binding.stateSet); row.submitCamera = camera;
            const std::string& filename = image && !image->getFileName().empty()
                ? image->getFileName() : binding.texture->getName();
            std::size_t roleSize = 0;
            for (std::size_t i = 0; i < scratch.uniformCount; ++i)
            {
                const auto& uniform = scratch.uniforms[i];
                const std::string_view name(*uniform.name);
                if (!uniform.uniform || uniform.uniform->getType() != osg::Uniform::SAMPLER_2D) continue;
                const auto elements = uniform.uniform->getNumElements();
                if (elements > 512) capture.noteResourceUncovered();
                for (unsigned element = 0; element < std::min(512u, elements); ++element)
                {
                    int boundUnit = -1;
                    if (!uniform.uniform->getElement(element, boundUnit) || boundUnit < 0
                        || static_cast<unsigned>(boundUnit) != unit) continue;
                    std::array<char, 12> suffix{};
                    std::size_t suffixSize = 0;
                    if (elements > 1)
                    {
                        suffix[0] = '[';
                        const auto number = std::to_chars(suffix.data() + 1, suffix.data() + suffix.size() - 1, element);
                        *number.ptr = ']'; suffixSize = number.ptr - suffix.data() + 1;
                    }
                    const auto nameSize = name.size() + suffixSize;
                    // Preserve the previous truncation/drop rule exactly.
                    if (roleSize + nameSize + 1 >= row.semanticRole.size())
                    { capture.noteResourceUncovered(); continue; }
                    if (roleSize) row.semanticRole[roleSize++] = '|';
                    std::memcpy(row.semanticRole.data() + roleSize, name.data(), name.size());
                    roleSize += name.size();
                    if (suffixSize) std::memcpy(row.semanticRole.data() + roleSize, suffix.data(), suffixSize);
                    roleSize += suffixSize;
                }
            }
            auto copyText = [](auto& output, std::string_view value) {
                std::memcpy(output.data(), value.data(), std::min(value.size(), output.size() - 1));
            };
            if (!roleSize)
            {
                const std::string_view fallback = scope == 2 && unit <= 1
                    ? (unit == 0 ? "terrain_composite_diffuse" : "terrain_composite_blendmap") : "unknown";
                copyText(row.semanticRole, fallback); roleSize = fallback.size();
            }
            const std::string_view role(row.semanticRole.data(), roleSize);
            const std::string_view textureClass(binding.texture->className());
            copyText(row.textureClass, textureClass); copyText(row.filename, filename);
            if (filename.size() >= row.filename.size()) capture.noteResourceUncovered();
            if (textureClass.size() >= row.textureClass.size()) capture.noteResourceUncovered();
            if (binding.texture->getNumImages() > 1) capture.noteResourceUncovered();
            row.labelHash = 1469598103934665603ull;
            for (const auto value : {std::string_view(filename), role})
            {
                row.labelHash ^= value.size(); row.labelHash *= 1099511628211ull;
                for (unsigned char character : value) { row.labelHash ^= character; row.labelHash *= 1099511628211ull; }
            }
            for (unsigned char character : textureClass) { row.labelHash ^= character; row.labelHash *= 1099511628211ull; }
            capture.catalogResource(row);
            if (descriptor)
            {
                if (row.bytes >= descriptor->texture0Bytes)
                {
                    descriptor->texture1Bytes = descriptor->texture0Bytes; descriptor->texture1 = descriptor->texture0;
                    descriptor->texture0Bytes = row.bytes; descriptor->texture0 = {};
                    copyText(descriptor->texture0, filename);
                }
                else if (row.bytes >= descriptor->texture1Bytes)
                {
                    descriptor->texture1Bytes = row.bytes; descriptor->texture1 = {};
                    copyText(descriptor->texture1, filename);
                }
            }
        }
    }


    // Each StateSet supplies ordered units/names. Merge it into the bounded
    // effective list in one pass. Capacity is reserved for all old bindings:
    // accept new keys in input order exactly as the legacy insertion path does.
    template <class ResourceCapture>
    inline void catalogStateSetsOptimizedImpl(std::span<const osg::StateSet* const> stateSets,
        unsigned frame, unsigned context, std::uint64_t camera, unsigned scope,
        Capture::Row* descriptor, CatalogScratch& scratch,
        CatalogScratch::OptimizedStorage& storage, ResourceCapture& capture)
    {
        for (const auto* stateSet : stateSets)
        {
            if (!stateSet) continue;
            const auto& attributes = stateSet->getTextureAttributeList();
            if (!attributes.empty())
            {
                const auto oldCount = scratch.textureCount;
                std::size_t old = 0, out = 0, added = 0;
                for (unsigned unit = 0; unit < attributes.size(); ++unit)
                {
                    const auto* pair = stateSet->getTextureAttributePair(unit, osg::StateAttribute::TEXTURE);
                    const auto* texture = pair && pair->first ? pair->first->asTexture() : nullptr;
                    if (!texture) continue;
                    while (old < oldCount && scratch.textures[old].unit < unit)
                        storage.mergedTextures[out++] = scratch.textures[old++];
                    if (old < oldCount && scratch.textures[old].unit == unit)
                    {
                        const auto& previous = scratch.textures[old++];
                        storage.mergedTextures[out++] = (previous.flags & osg::StateAttribute::OVERRIDE)
                            && !(pair->second & osg::StateAttribute::PROTECTED)
                            ? previous : CatalogScratch::TextureBinding{unit, texture, stateSet, pair->second};
                    }
                    else if (added < CatalogScratch::MaxBindings - oldCount)
                    {
                        storage.mergedTextures[out++] = {unit, texture, stateSet, pair->second};
                        ++added;
                    }
                    else capture.noteResourceUncovered();
                }
                while (old < oldCount) storage.mergedTextures[out++] = scratch.textures[old++];
                std::copy_n(storage.mergedTextures.begin(), out, scratch.textures.begin());
                scratch.textureCount = out;
            }
            const auto& uniforms = stateSet->getUniformList();
            if (!uniforms.empty())
            {
                const auto oldCount = scratch.uniformCount;
                std::size_t old = 0, out = 0, added = 0;
                for (const auto& [name, pair] : uniforms)
                {
                    const std::string_view key(name);
                    while (old < oldCount && std::string_view(*scratch.uniforms[old].name) < key)
                        storage.mergedUniforms[out++] = scratch.uniforms[old++];
                    if (old < oldCount && *scratch.uniforms[old].name == name)
                    {
                        const auto& previous = scratch.uniforms[old++];
                        storage.mergedUniforms[out++] = (previous.flags & osg::StateAttribute::OVERRIDE)
                            && !(pair.second & osg::StateAttribute::PROTECTED)
                            ? previous : CatalogScratch::UniformBinding{&name, pair.first.get(), pair.second};
                    }
                    else if (added < CatalogScratch::MaxBindings - oldCount)
                    {
                        storage.mergedUniforms[out++] = {&name, pair.first.get(), pair.second};
                        ++added;
                    }
                    else capture.noteResourceUncovered();
                }
                while (old < oldCount) storage.mergedUniforms[out++] = scratch.uniforms[old++];
                std::copy_n(storage.mergedUniforms.begin(), out, scratch.uniforms.begin());
                scratch.uniformCount = out;
            }
        }

        // Resolve every live effective sampler element once, after non-sampler
        // shadowing/OVERRIDE/PROTECTED have been applied. A role is owned scalar
        // output for this call, not a retained Uniform value or cross-call cache.
        for (std::size_t i = 0; i < scratch.textureCount; ++i) storage.roles[i].size = 0;
        if (scratch.textureCount)
        {
            const auto end = scratch.textures.begin() + scratch.textureCount;
            for (std::size_t i = 0; i < scratch.uniformCount; ++i)
            {
                const auto& uniform = scratch.uniforms[i];
                if (!uniform.uniform || uniform.uniform->getType() != osg::Uniform::SAMPLER_2D) continue;
                const std::string_view name(*uniform.name);
                const auto elements = uniform.uniform->getNumElements();
                // The legacy unit is one notification per effective texture,
                // even when no element refers to that texture. Preserve it.
                if (elements > 512)
                    for (std::size_t texture = 0; texture < scratch.textureCount; ++texture)
                        capture.noteResourceUncovered();
                for (unsigned element = 0; element < std::min(512u, elements); ++element)
                {
                    int boundUnit = -1;
                    if (!uniform.uniform->getElement(element, boundUnit) || boundUnit < 0) continue;
                    const auto at = std::lower_bound(scratch.textures.begin(), end, static_cast<unsigned>(boundUnit),
                        [](const auto& binding, unsigned value) { return binding.unit < value; });
                    if (at == end || at->unit != static_cast<unsigned>(boundUnit)) continue;
                    auto& role = storage.roles[at - scratch.textures.begin()];
                    std::array<char, 12> suffix{};
                    std::size_t suffixSize = 0;
                    if (elements > 1)
                    {
                        suffix[0] = '[';
                        const auto number = std::to_chars(suffix.data() + 1, suffix.data() + suffix.size() - 1, element);
                        *number.ptr = ']'; suffixSize = number.ptr - suffix.data() + 1;
                    }
                    const auto nameSize = name.size() + suffixSize;
                    if (role.size + nameSize + 1 >= role.text.size())
                    { capture.noteResourceUncovered(); continue; }
                    if (role.size) role.text[role.size++] = '|';
                    std::memcpy(role.text.data() + role.size, name.data(), name.size());
                    role.size += name.size();
                    if (suffixSize) std::memcpy(role.text.data() + role.size, suffix.data(), suffixSize);
                    role.size += suffixSize;
                }
            }
        }
        for (std::size_t index = 0; index < scratch.textureCount; ++index)
        {
            const auto& binding = scratch.textures[index];
            const unsigned unit = binding.unit;
            const auto* image = binding.texture->getNumImages() > 0 ? binding.texture->getImage(0) : nullptr;
            Capture::ResourceRow row;
            row.firstFrame = row.lastFrame = frame; row.context = context; row.unit = unit; row.scope = scope;
            row.texture = reinterpret_cast<std::uintptr_t>(binding.texture);
            row.image = reinterpret_cast<std::uintptr_t>(image);
            row.imageRevision = image ? image->getModifiedCount() : 0;
            row.bytes = image ? image->getTotalSizeInBytesIncludingMipmaps() : 0;
            row.stateSet = reinterpret_cast<std::uintptr_t>(binding.stateSet); row.submitCamera = camera;
            const std::string& filename = image && !image->getFileName().empty()
                ? image->getFileName() : binding.texture->getName();
            std::size_t roleSize = storage.roles[index].size;
            if (roleSize) std::memcpy(row.semanticRole.data(), storage.roles[index].text.data(), roleSize);
            auto copyText = [](auto& output, std::string_view value) {
                std::memcpy(output.data(), value.data(), std::min(value.size(), output.size() - 1));
            };
            if (!roleSize)
            {
                const std::string_view fallback = scope == 2 && unit <= 1
                    ? (unit == 0 ? "terrain_composite_diffuse" : "terrain_composite_blendmap") : "unknown";
                copyText(row.semanticRole, fallback); roleSize = fallback.size();
            }
            const std::string_view role(row.semanticRole.data(), roleSize);
            const std::string_view textureClass(binding.texture->className());
            copyText(row.textureClass, textureClass); copyText(row.filename, filename);
            if (filename.size() >= row.filename.size()) capture.noteResourceUncovered();
            if (textureClass.size() >= row.textureClass.size()) capture.noteResourceUncovered();
            if (binding.texture->getNumImages() > 1) capture.noteResourceUncovered();
            row.labelHash = 1469598103934665603ull;
            for (const auto value : {std::string_view(filename), role})
            {
                row.labelHash ^= value.size(); row.labelHash *= 1099511628211ull;
                for (unsigned char character : value) { row.labelHash ^= character; row.labelHash *= 1099511628211ull; }
            }
            for (unsigned char character : textureClass) { row.labelHash ^= character; row.labelHash *= 1099511628211ull; }
            capture.catalogResource(row);
            if (descriptor)
            {
                if (row.bytes >= descriptor->texture0Bytes)
                {
                    descriptor->texture1Bytes = descriptor->texture0Bytes; descriptor->texture1 = descriptor->texture0;
                    descriptor->texture0Bytes = row.bytes; descriptor->texture0 = {};
                    copyText(descriptor->texture0, filename);
                }
                else if (row.bytes >= descriptor->texture1Bytes)
                {
                    descriptor->texture1Bytes = row.bytes; descriptor->texture1 = {};
                    copyText(descriptor->texture1, filename);
                }
            }
        }
    }

    inline void catalogStateSetsImpl(std::span<const osg::StateSet* const> stateSets,
        unsigned frame, unsigned context, std::uint64_t camera, unsigned scope,
        Capture::Row* descriptor, CatalogScratch& scratch)
    {
        auto& capture = Capture::instance();
        if (capture.catalogOptimizedRequested())
        {
            // Allocate before any observations or drop notifications. Failure
            // falls back once per owner, with complete legacy output and counts.
            if (!scratch.optimized && !scratch.optimizedUnavailable)
            {
                try { scratch.optimized = std::make_unique<CatalogScratch::OptimizedStorage>(); }
                catch (const std::bad_alloc&)
                {
                    scratch.optimizedUnavailable = true;
                    capture.noteCatalogCandidateScratchAllocationFailure();
                }
            }
            if (scratch.optimized)
            {
                catalogStateSetsOptimizedImpl(stateSets, frame, context, camera, scope, descriptor,
                    scratch, *scratch.optimized, capture);
                return;
            }
        }
        catalogStateSetsLegacyImpl(stateSets, frame, context, camera, scope, descriptor, scratch, capture);
    }

    // Caller-owned scratch makes concurrent cull visitors and nested calls
    // independent. The convenience path is stack-local; it cannot reuse stale
    // references across calls or share TLS storage with a recursive producer.
    inline void catalogStateSets(std::span<const osg::StateSet* const> stateSets,
        unsigned frame, unsigned context, std::uint64_t camera, unsigned scope, Capture::Row* descriptor = nullptr)
    {
        if (!Capture::instance().enabled()) return;
        CatalogScratch scratch;
        CatalogDiagnostic diagnostic(scratch);
        catalogStateSetsImpl(stateSets, frame, context, camera, scope, descriptor, scratch);
    }
    inline void catalogStateSets(std::initializer_list<const osg::StateSet*> stateSets,
        unsigned frame, unsigned context, std::uint64_t camera, unsigned scope, Capture::Row* descriptor = nullptr)
    {
        catalogStateSets(std::span<const osg::StateSet* const>(stateSets.begin(), stateSets.size()),
            frame, context, camera, scope, descriptor);
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

                {
                    CatalogDiagnostic catalogDiagnostic(mCatalogScratch);
                    mMaterialPath.clear();
                    for(auto* graph=leaf->_parent;graph;graph=graph->_parent)
                        if(graph->getStateSet()) mMaterialPath.push_back(graph->getStateSet());
                    std::reverse(mMaterialPath.begin(),mMaterialPath.end());
                    const auto* camera=getCurrentCamera();
                    const auto* context=camera ? camera->getGraphicsContext() : nullptr;
                    const auto* stamp=getFrameStamp();
                    catalogStateSetsImpl(mMaterialPath,stamp ? stamp->getFrameNumber() : 0,
                        context && context->getState() ? context->getState()->getContextID() : ~0u,
                        leaf->submitCamera,1,&leaf->descriptor,mCatalogScratch);
                }

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
        CatalogScratch mCatalogScratch;
        std::vector<const osg::StateSet*> mMaterialPath;
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
        #include "rendererculltrace.inl"
        void draw() override { run(GLCallTrace::RendererDraw,[this]{osgViewer::Renderer::draw();}); }
        void cull_draw() override { run(GLCallTrace::CullDraw,[this]{osgViewer::Renderer::cull_draw();}); }
        void compile() override { run(GLCallTrace::Compile,[this]{osgViewer::Renderer::compile();}); }
    private:
        template<typename F> void run(unsigned phase,F fn)
        {
            Debug::HitchTrace::Scope envelope(phase == GLCallTrace::RendererDraw ? "renderer.draw"
                : phase == GLCallTrace::CullDraw ? "renderer.cull_draw" : "renderer.compile", Debug::HitchTrace::CpuTiming::Scoped);
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
