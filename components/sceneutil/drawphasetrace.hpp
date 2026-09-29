#ifndef OPENMW_SCENEUTIL_DRAWPHASETRACE_H
#define OPENMW_SCENEUTIL_DRAWPHASETRACE_H

#include "glcalltrace.hpp"
#include <osg/FrameStamp>
#include <osg/Geometry>
#include <osg/State>
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
#include <typeinfo>
#include <stdexcept>

namespace SceneUtil::DrawPhaseTrace
{
    // Diagnostic-only. Timers are CPU envelopes and may include driver waits;
    // none is a GPU timer. No output, formatting or allocation during a draw.
    class Capture
    {
    public:
        struct Row
        {
            unsigned frame = 0, context = 0;
            std::uint64_t drawable = 0;
            double matricesMs = 0, stateMs = 0, drawMs = 0, retireMs = 0;
            bool dynamic = false;
            std::array<char, 96> name{};
            std::array<char, 64> camera{};
            std::array<char, 32> kind{};
        };
        struct Totals
        {
            std::uint64_t calls = 0;
            double matricesMs = 0, stateMs = 0, drawMs = 0, retireMs = 0;
        };
        struct FrameTotals : Totals
        {
            unsigned frame = 0, context = 0;
            bool valid = false;
        };
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
                auto& t = mTotals[row.context];
                ++t.calls; t.matricesMs += row.matricesMs; t.stateMs += row.stateMs;
                t.drawMs += row.drawMs; t.retireMs += row.retireMs;
                auto& f = mCurrent[row.context];
                if (!f.valid || f.frame != row.frame)
                {
                    flushFrame(f);
                    f = {}; f.frame = row.frame; f.context = row.context; f.valid = true;
                }
                ++f.calls; f.matricesMs += row.matricesMs; f.stateMs += row.stateMs;
                f.drawMs += row.drawMs; f.retireMs += row.retireMs;
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
            for (auto& f : mCurrent) flushFrame(f);
            try
            {
                std::ofstream frames(mPath + ".frames.csv");
                frames << "frame,context,calls,matrices_ms,state_ms,draw_ms,retire_ms\n" << std::setprecision(10);
                for (std::size_t i = 0; i < (std::min)(mFrameCount.load(), FrameCapacity); ++i)
                {
                    const auto& f = (*mFrames)[i];
                    frames << f.frame << ',' << f.context << ',' << f.calls << ',' << f.matricesMs
                        << ',' << f.stateMs << ',' << f.drawMs << ',' << f.retireMs << '\n';
                }
                std::ofstream out(mPath);
                out << "frame,context,drawable,matrices_ms,state_ms,draw_ms,retire_ms,dynamic,name,camera,kind\n" << std::setprecision(8);
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
                    out << "\",\"";
                    for (char c : r.camera) { if (!c) break; if (c == '"') out << '"'; out << c; }
                    out << "\",\"";
                    for (char c : r.kind) { if (!c) break; if (c == '"') out << '"'; out << c; }
                    out << "\"\n";
                }
                std::ofstream status(mPath + ".status.txt");
                const auto n = mCount.load();
                status << "rows_dropped=" << (n > Capacity ? n - Capacity : 0)
                    << "\nuninstrumented_pool_overflow=" << mUncovered.load()
                    << "\nvisitor_instances=" << mVisitors.load()
                    << "\nframes_dropped=" << (mFrameCount > FrameCapacity ? mFrameCount - FrameCapacity : 0)
                    << "\nscope=instrumented_render_leaves_not_renderstage_setup_swap_or_gpu_time\n";
                std::uint64_t totalCalls = 0;
                for (unsigned i = 0; i < mTotals.size(); ++i)
                {
                    const auto& t = mTotals[i];
                    totalCalls += t.calls;
                    if (t.calls) status << "context=" << i << " calls=" << t.calls << " matrices_ms=" << t.matricesMs
                        << " state_ms=" << t.stateMs << " draw_ms=" << t.drawMs << " retire_ms=" << t.retireMs << '\n';
                }
                status << "total_calls=" << totalCalls << "\ncoverage_valid=" << (totalCalls != 0) << '\n';
            }
            catch (...) {}
        }
    private:
        static constexpr std::size_t Capacity = 32768, FrameCapacity = 32768;
        void flushFrame(FrameTotals& frame)
        {
            if (!frame.valid || !mFrames) return;
            const auto index = mFrameCount.fetch_add(1, std::memory_order_relaxed);
            if (index < FrameCapacity) (*mFrames)[index] = frame;
            frame.valid = false;
        }
        Capture()
        {
            if (const char* value = std::getenv("OPENMW_P9_LEAF_TRACE_FILE"); value && *value)
            {
                mPath = value;
                mRows = std::make_unique<std::array<Row, Capacity>>();
                mFrames = std::make_unique<std::array<FrameTotals, FrameCapacity>>();
            }
        }
        std::string mPath;
        std::unique_ptr<std::array<Row, Capacity>> mRows;
        std::atomic<std::size_t> mCount{0};
        std::atomic<std::uint64_t> mUncovered{0};
        std::atomic<unsigned> mVisitors{0};
        std::array<Totals, 16> mTotals{};
        std::array<FrameTotals, 16> mCurrent{};
        std::unique_ptr<std::array<FrameTotals, FrameCapacity>> mFrames;
        std::atomic<std::size_t> mFrameCount{0};
    };

    class Leaf final : public osgUtil::RenderLeaf
    {
    public:
        Leaf(osg::Drawable* drawable, osg::RefMatrix* projection, osg::RefMatrix* modelview)
            : osgUtil::RenderLeaf(drawable, projection, modelview) {}
        std::array<char, 64> cameraName{};
        std::array<char, 32> sourceKind{};
        void render(osg::RenderInfo& info, osgUtil::RenderLeaf* previous) override
        {
            auto& capture = Capture::instance();
            if (!capture.enabled()) { osgUtil::RenderLeaf::render(info, previous); return; }
            osg::State& state = *info.getState();
            if (state.getAbortRendering()) return;
            using Clock = std::chrono::steady_clock;
            GLCallTrace::LeafScope glScope(reinterpret_cast<std::uintptr_t>(_drawable.get()));
            const auto start = Clock::now();
            state.applyProjectionMatrix(_projection.get());
            state.applyModelViewMatrix(_modelview.get());
            const auto matrices = Clock::now();
            glScope.phase(GLCallTrace::Phase::State);

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
            glScope.phase(GLCallTrace::Phase::Draw);
            _drawable->draw(info);
            const auto drawn = Clock::now();
            const auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
            const auto* stamp = state.getFrameStamp();
            Capture::Row row;
            row.frame = stamp ? stamp->getFrameNumber() : 0;
            row.context = state.getContextID();
            row.matricesMs = ms(start, matrices); row.stateMs = ms(matrices, applied);
            row.drawMs = ms(applied, drawn); row.dynamic = _dynamic;
            row.drawable = reinterpret_cast<std::uintptr_t>(_drawable.get());
            row.camera = cameraName; row.kind = sourceKind;
            // Copy scene-owned metadata BEFORE signalling dynamic completion.
            // The released update thread may then rename/rebind scene objects.
            if (row.matricesMs + row.stateMs + row.drawMs >= .25)
            {
                const auto& name = _drawable->getName();
                const auto size = (std::min)(name.size(), row.name.size() - 1);
                std::memcpy(row.name.data(), name.data(), size);
            }
            glScope.phase(GLCallTrace::Phase::Retire);
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
            const auto first = _currentReuseRenderLeafIndex;
            osgUtil::CullVisitor::apply(drawable);
            for (unsigned i = first; i < _currentReuseRenderLeafIndex && i < (mInstrumented ? PoolSize : 0); ++i)
            {
                auto* leaf = static_cast<Leaf*>(_reuseRenderLeafList[i].get());
                leaf->cameraName = {}; leaf->sourceKind = {};
                if (const auto* camera = getCurrentCamera())
                {
                    const auto& name = camera->getName();
                    std::memcpy(leaf->cameraName.data(), name.data(), (std::min)(name.size(), leaf->cameraName.size() - 1));
                }
                const char* kind = drawable.className();
                for (auto* node : getNodePath())
                    if (std::strcmp(node->className(), "RigGeometry") == 0
                        || std::strcmp(node->className(), "MorphGeometry") == 0) kind = node->className();
                std::memcpy(leaf->sourceKind.data(), kind, (std::min)(std::strlen(kind), leaf->sourceKind.size() - 1));
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
        if (!viewer.getCamera()->getRenderer())
            viewer.getCamera()->setRenderer(new osgViewer::Renderer(viewer.getCamera()));
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

    inline unsigned installRequired(osgViewer::Viewer& viewer, bool stereo)
    {
        if (!Capture::instance().enabled()) return 0;
        if (stereo) throw std::runtime_error("Phase 9 ROOT-CAUSE TRACE requires the supported mono view; no capture started");
        const unsigned installed = install(viewer);
        if (installed != 2)
            throw std::runtime_error("Phase 9 TRACE installation failed before realization (expected 2 scene views); no valid trace");
        return installed;
    }
}
#endif
