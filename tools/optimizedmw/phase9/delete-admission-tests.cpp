#include <components/resource/openmwcompileoperation.hpp>

#include <osg/ContextData>
#include <osg/FrameStamp>
#include <osg/GraphicsContext>
#include <osgViewer/Viewer>

#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    class CountingManager : public osg::GraphicsObjectManager
    {
    public:
        explicit CountingManager(unsigned context) : GraphicsObjectManager("test deletion queue", context) {}
        void flushDeletedGLObjects(double, double&) override { ++flushes; }
        void flushAllDeletedGLObjects() override { ++shutdown; }
        void deleteAllGLObjects() override {}
        void discardAllGLObjects() override {}
        unsigned flushes = 0, shutdown = 0;
    };
    Resource::OpenGlPressureSample sample(void* owner)
    {
        Resource::OpenGlPressureSample result;
        result.decision.state = *static_cast<Resource::OpenGlPressure*>(owner);
        return result;
    }
}

int main() try
{
    using Policy = Resource::P9DeleteAdmission;
    Policy policy;
    const int a = 0, b = 1;
    for (unsigned i = 1; i <= 30; ++i)
        require(!policy.select(&a, i, 0, 100, true, false, 30).admitted,
            "persistent load did not defer within its finite bound");
    require(policy.select(&a, 31, 0, 100, true, false, 30).reason == Policy::Reason::Progress,
        "persistent load starved required cleanup service");
    require(!policy.select(&a, 31, 1000, 0, false, true, 30).admitted,
        "same context/frame admitted twice");
    require(policy.select(&a, 32, 0, 100, true, true, 30).reason == Policy::Reason::Pressure,
        "pressure did not bypass optional deferral");
    require(!policy.select(&b, 32, 0, 100, true, false, 30).admitted,
        "context replacement inherited elapsed progress age");
    require(!policy.select(&b, 0, std::numeric_limits<double>::quiet_NaN(), 0, false, false, 30).admitted,
        "unknown headroom admitted cleanup");
    require(policy.select(&b, 1, 10, .15, false, false, 30).reason == Policy::Reason::Headroom,
        "usable headroom did not admit cleanup");
    require(!policy.select(&b, 2, 0, 100, true, false, 30).admitted,
        "congested frame did not defer");
    require(policy.select(&b, 2, 0, 100, true, true, 30).reason == Policy::Reason::Pressure,
        "same-frame pressure after deferral did not admit cleanup");
    require(!policy.select(&b, 2, 0, 100, true, true, 30).admitted,
        "same-frame pressure duplicated an admitted flush");

    // Exercise the actual ICO branch and OSG manager dispatch. This checks that
    // the admission result controls the real flush, rather than testing only
    // the policy in isolation; the driver operation itself is not time-capped.
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 16; traits->height = 16;
    traits->windowDecoration = false; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "native GL context unavailable");
    const auto contextId = context->getState()->getContextID();
    osg::ref_ptr<CountingManager> manager = new CountingManager(contextId);
    osg::getOrCreateContextData(contextId)->set(manager.get());
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
    context->getState()->setFrameStamp(stamp);
    Resource::OpenGlPressure pressure = Resource::OpenGlPressure::Normal;
    Resource::OpenMWCompileSchedulerConfig config;
    config.mMode = 2;
    config.mTargetFrameRate = 1;
    config.mDeleteAdmission = true;
    config.mMaxQueueAgeFrames = 2;
    config.mPressureContext = { nullptr, &sample, &pressure };
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico = new Resource::OpenMWIncrementalCompileOperation(config);
    osgUtil::IncrementalCompileOperation::Contexts contexts{ context.get() };
    ico->assignContexts(contexts);
    auto frame = [&](unsigned number) {
        stamp->setFrameNumber(number);
        context->clear();
        (*ico)(context);
    };
    Resource::OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(50);
    frame(1); frame(2);
    require(manager->flushes == 0, "ICO ignored congested-frame deferral");
    frame(3);
    require(manager->flushes == 1, "ICO did not dispatch bounded cleanup progress");
    frame(3);
    require(manager->flushes == 1, "repeated traversal duplicated deletion flush");
    Resource::OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(0);
    frame(4);
    require(manager->flushes == 2, "ICO did not use smooth headroom");
    Resource::OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(50);
    pressure = Resource::OpenGlPressure::Critical;
    frame(5);
    pressure = Resource::OpenGlPressure::Degraded;
    frame(6);
    require(manager->flushes == 4, "critical or unknown pressure retained cleanup backlog");
    pressure = Resource::OpenGlPressure::Normal;
    frame(7);
    pressure = Resource::OpenGlPressure::Critical;
    frame(7); frame(7);
    require(manager->flushes == 5, "same-frame pressure did not bypass a deferred ICO decision exactly once");
    context->getState()->setFrameStamp(nullptr);
    (*ico)(context); (*ico)(context);
    require(manager->flushes == 7, "unknown frame coordinate starved OSG cleanup");
    context->getState()->setFrameStamp(stamp);

    config.mDeleteAdmission = false;
    osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> control = new Resource::OpenMWIncrementalCompileOperation(config);
    control->assignContexts(contexts);
    (*control)(context); (*control)(context);
    require(manager->flushes == 9, "disabled control changed OSG deletion dispatch");
    osg::flushAllDeletedGLObjects(contextId);
    require(manager->shutdown == 1, "shutdown no longer drains deferred OSG managers");
    context->releaseContext(); context->close();
    std::cout << "Deletion admission policy, production dispatch, pressure, progress and control passed\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n'; return 1;
}
