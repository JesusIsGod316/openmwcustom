#include <components/debug/v36gpuprofiler.hpp>
#include <osg/FrameStamp>
#include <iostream>
#include <map>
#include <stdexcept>

namespace
{
    unsigned frame = 0, nextId = 1, availabilityCalls = 0, resultCalls = 0;
    bool unavailable = false, reverseResult = false;
    struct Query { unsigned source = 0; GLuint64 timestamp = 0; };
    std::map<GLuint, Query> queries;
    void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
    void GL_APIENTRY gen(GLsizei count, GLuint* ids) { while (count--) *ids++ = nextId++; }
    void GL_APIENTRY counter(GLuint id, GLenum target)
    {
        require(target == Debug::V36GpuProfiler::QueryTimestamp, "Unexpected timestamp target");
        queries[id] = {frame, static_cast<GLuint64>(frame) * 1000000 + (id % 2 ? 0 : 100000)};
    }
    void GL_APIENTRY available(GLuint id, GLenum target, GLuint* value)
    {
        require(target == Debug::V36GpuProfiler::QueryResultAvailable, "Availability call requested blocking result");
        ++availabilityCalls;
        if (Debug::V36GpuProfiler::deferredCollectionRequested())
            require(frame >= queries.at(id).source + 2, "Deferred policy polled a fresh query");
        *value = !unavailable && frame >= queries.at(id).source + 2;
    }
    void GL_APIENTRY result(GLuint id, GLenum target, GLuint64* value)
    {
        require(target == Debug::V36GpuProfiler::QueryResult && !unavailable
            && frame >= queries.at(id).source + 2, "Result read before availability");
        ++resultCalls;
        *value = reverseResult && id % 2 == 0 ? queries.at(id).timestamp - 200000 : queries.at(id).timestamp;
    }
}

int main(int argc, char** argv) try
{
    require(argc == 2, "gpu-query-tests MODE");
    const std::string mode = argv[1];
    require(mode == "normal" || mode == "available" || mode == "unavailable" || mode == "reversed" || mode == "unjoined",
        "Invalid GPU query fixture mode");
    const bool normal = mode == "normal";
    unavailable = mode == "unavailable";
    reverseResult = mode == "reversed";
    osg::ref_ptr<osg::State> state = new osg::State;
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
    osg::ref_ptr<osg::GLExtensions> gl = new osg::GLExtensions(0);
    gl->glGenQueries = gen; gl->glQueryCounter = counter;
    gl->glGetQueryObjectuiv = available; gl->glGetQueryObjectui64v = result;
    state->set<osg::GLExtensions>(gl); state->setFrameStamp(stamp);
    osg::ref_ptr<osg::Camera> camera = new osg::Camera;
    osg::RenderInfo info(state, nullptr); info.pushCamera(camera);
    osg::ref_ptr<Debug::V36GpuProfiler::PassTracker> tracker = new Debug::V36GpuProfiler::PassTracker("query-contract-long-name-owned");
    for (frame = 100; frame < 140; ++frame)
    {
        stamp->setFrameNumber(frame);
        const unsigned before = availabilityCalls;
        tracker->begin(info); tracker->end(info);
        const unsigned once = availabilityCalls;
        tracker->begin(info); tracker->end(info);
        if (Debug::V36GpuProfiler::deferredCollectionRequested())
        {
            require(availabilityCalls == once, "Same-frame/end polling bypassed deferred policy");
        }
        if (frame < 102 && Debug::V36GpuProfiler::deferredCollectionRequested())
            require(availabilityCalls == before, "Minimum two-frame latency missing");
    }
    if (normal) require(availabilityCalls == 0 && resultCalls == 0 && queries.empty(), "Normal mode entered query work");
    if (unavailable) require(resultCalls == 0 && nextId == 33, "Unavailable/full ring read or overwrote query results");
    if (!normal && !unavailable) require(resultCalls > 0, "Completed queries not collected");
    Debug::V36GpuProfiler::finish(mode != "unjoined");
    Debug::V3Diagnostics::DiagnosticWriterHub::instance().finish();
    std::cout << "PASS " << mode << " availability_calls=" << availabilityCalls << " result_calls=" << resultCalls << '\n';
}
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
