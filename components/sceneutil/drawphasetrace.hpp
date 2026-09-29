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
        struct Row
        {
            unsigned frame = 0, context = 0;
            std::uint64_t drawable = 0, camera = 0, stateSet = 0, nodePathHash = 0;
            std::uint64_t texture0Bytes = 0, texture1Bytes = 0;
            std::int64_t startNs = 0;
            double matricesMs = 0, stateMs = 0, drawMs = 0, retireMs = 0;
            std::uint32_t vertices = 0, primitiveSets = 0, cameraBucket = CameraUnknown;
            bool dynamic = false;
            std::array<char, 96> name{};
            std::array<char, 64> ownerName{};
            std::array<char, 32> ownerClass{};
            std::array<char, 48> cameraName{};
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
                if (row.cameraBucket < mCameraTotals.size())
                {
                    auto& camera = mCameraTotals[row.cameraBucket];
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
                    << "texture0,texture0_bytes,texture1,texture1_bytes\n" << std::setprecision(8);
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
                    out<<"\","<<r.texture1Bytes<<'\n';
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
                for (unsigned i = 0; i < mCameraTotals.size(); ++i)
                {
                    const auto& t = mCameraTotals[i];
                    if (t.calls) status << "camera_bucket=" << cameraBucketName(i) << " calls=" << t.calls
                        << " matrices_ms=" << t.matricesMs << " state_ms=" << t.stateMs
                        << " draw_ms=" << t.drawMs << " retire_ms=" << t.retireMs << '\n';
                }
            }
            catch (...) {}
        }
    private:
        static constexpr std::size_t Capacity = 32768, FrameCapacity = 65536;
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
        std::array<Totals, CameraBucketCount> mCameraTotals{};
        std::unique_ptr<std::array<FrameRow,FrameCapacity>> mFrames;
        std::array<FrameRow,16> mCurrentFrame{};
        std::atomic<std::size_t> mFrameCount{0};
        std::array<std::uint64_t,16> mOuterCalls{};
        std::array<double,16> mOuterMs{};
        std::array<unsigned,16> mOuterKind{};
    };

    class Leaf final : public osgUtil::RenderLeaf
    {
    public:
        Leaf(osg::Drawable* drawable, osg::RefMatrix* projection, osg::RefMatrix* modelview)
            : osgUtil::RenderLeaf(drawable, projection, modelview) {}
        std::uint64_t nodePathHash = 0;
        std::uint32_t vertices = 0, primitiveSets = 0, cameraBucket = CameraUnknown;
        std::array<char, 64> ownerName{};
        std::array<char, 32> ownerClass{};
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
                reinterpret_cast<std::uintptr_t>(_drawable.get())});
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
            Capture::Row row;
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
            row.ownerName = ownerName;
            row.ownerClass = ownerClass;
            // Copy scene-owned metadata BEFORE signalling dynamic completion.
            // The released update thread may then rename/rebind scene objects.
            if (row.matricesMs + row.stateMs + row.drawMs >= .25)
            {
                const auto& name = _drawable->getName();
                const auto size = (std::min)(name.size(), row.name.size() - 1);
                std::memcpy(row.name.data(), name.data(), size);
                if(const auto* camera=info.getCurrentCamera())
                { const auto& n=camera->getName();std::memcpy(row.cameraName.data(),n.data(),(std::min)(n.size(),row.cameraName.size()-1)); }
                const char* cls=_drawable->className();
                if(cls) std::memcpy(row.drawableClass.data(),cls,(std::min)(std::strlen(cls),row.drawableClass.size()-1));

                // State application can lazily realize terrain/material textures.
                // Snapshot only slow state rows so normal draw overhead stays tiny.
                if (row.stateMs >= 2.0)
                {
                    if (osg::StateSet* stateSet = _parent ? _parent->getStateSet() : nullptr)
                    {
                        const auto& attributes = stateSet->getTextureAttributeList();
                        auto recordTexture = [&](const osg::Texture2D* texture) {
                            if (!texture) return;
                            const osg::Image* image = texture->getImage();
                            const std::uint64_t bytes = image ? image->getTotalSizeInBytesIncludingMipmaps() : 0;
                            const std::string& label = image && !image->getFileName().empty()
                                ? image->getFileName() : texture->getName();
                            auto store = [&](std::array<char, 128>& dest) {
                                const auto amount = (std::min)(label.size(), dest.size() - 1);
                                if (amount) std::memcpy(dest.data(), label.data(), amount);
                            };
                            if (bytes >= row.texture0Bytes)
                            {
                                row.texture1Bytes = row.texture0Bytes;
                                row.texture1 = row.texture0;
                                row.texture0Bytes = bytes;
                                row.texture0 = {};
                                store(row.texture0);
                            }
                            else if (bytes >= row.texture1Bytes)
                            {
                                row.texture1Bytes = bytes;
                                row.texture1 = {};
                                store(row.texture1);
                            }
                        };
                        for (unsigned unit = 0; unit < attributes.size(); ++unit)
                        {
                            osg::StateAttribute* attribute
                                = stateSet->getTextureAttribute(unit, osg::StateAttribute::TEXTURE);
                            osg::Texture* texture = attribute ? attribute->asTexture() : nullptr;
                            recordTexture(dynamic_cast<osg::Texture2D*>(texture));
                        }
                    }
                }
            }
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
            for (unsigned i = first; i < _currentReuseRenderLeafIndex && i < (mInstrumented ? PoolSize : 0); ++i)
            {
                auto* leaf = static_cast<Leaf*>(_reuseRenderLeafList[i].get());
                leaf->nodePathHash = 1469598103934665603ull;
                leaf->vertices = 0;
                leaf->primitiveSets = 0;
                leaf->cameraBucket = CameraUnknown;
                leaf->ownerName = {};
                leaf->ownerClass = {};

                if (const osg::Camera* camera = getCurrentCamera())
                    leaf->cameraBucket = classifyCamera(camera->getName());

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
