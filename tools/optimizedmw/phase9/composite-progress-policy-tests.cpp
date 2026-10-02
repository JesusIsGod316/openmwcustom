#include <components/resource/openmwcompileoperation.hpp>
#include <components/resource/v321classifiedcompileset.hpp>

#include <osg/FrameStamp>
#include <osg/ContextData>
#include <osg/GraphicsContext>
#include <osgViewer/Viewer>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    void progressEnvironment(const char* value)
    {
#ifdef _WIN32
        _putenv_s("OPENMW_HITCH_COMPOSITE_PROGRESS", value);
#else
        setenv("OPENMW_HITCH_COMPOSITE_PROGRESS", value, 1);
#endif
    }
    class CountingManager final : public osg::GraphicsObjectManager
    {
    public:
        explicit CountingManager(unsigned context) : GraphicsObjectManager("progress deletion probe", context) {}
        void flushDeletedGLObjects(double, double&) override { ++flushes; }
        void flushAllDeletedGLObjects() override {}
        void deleteAllGLObjects() override {}
        void discardAllGLObjects() override {}
        unsigned flushes = 0;
    };
    struct PressureProbe
    {
        Resource::OpenGlPressure pressure = Resource::OpenGlPressure::Normal;
        unsigned samples = 0;
    };
    Resource::OpenGlPressureSample pressureSample(void* owner)
    {
        auto& probe = *static_cast<PressureProbe*>(owner);
        ++probe.samples;
        Resource::OpenGlPressureSample result;
        result.decision.state = probe.pressure;
        return result;
    }
    class Probe final : public Resource::OpenMWCompositeCompileOp
    {
    public:
        Kind kind() const override { return type; }
        std::size_t resourceBytes() const override { return bytes; }
        bool required() const override { return demand; }
        bool cancelled() const override { return cancel; }
        bool reusable(const osgUtil::IncrementalCompileOperation::CompileInfo&) const override { return ready; }
        bool tryCompleteWithoutGL(osgUtil::IncrementalCompileOperation::CompileInfo&) override
        {
            ++proofs;
            return ready && !decline;
        }
        bool bootstrapReady(const osgUtil::IncrementalCompileOperation::CompileInfo&) const override
        {
            ++bootstrapChecks;
            if (delayDispatchProof && bootstrapChecks > 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return bootstrap && !(withdrawDispatchProof && bootstrapChecks > 1);
        }
        double estimatedTimeForCompile(osgUtil::IncrementalCompileOperation::CompileInfo&) const override { return estimateSeconds; }
        bool compile(osgUtil::IncrementalCompileOperation::CompileInfo& info) override
        {
            ++dispatches;
            --info.maxNumObjectsToCompile;
            return true;
        }
        Kind type = Kind::Texture;
        std::size_t bytes = 1024u * 1024u;
        double estimateSeconds = .00025;
        bool ready = false, decline = false, bootstrap = false, demand = false, cancel = false;
        bool delayDispatchProof = false, withdrawDispatchProof = false;
        unsigned proofs = 0, dispatches = 0;
        mutable unsigned bootstrapChecks = 0;
    };
    void add(Resource::OpenMWIncrementalCompileOperation& ico, osg::GraphicsContext* context, Probe* probe)
    {
        auto lifetime = std::make_shared<Resource::V321ResourceCompileLifetime>();
        osg::ref_ptr<Resource::V321ClassifiedCompileSet> set = new Resource::V321ClassifiedCompileSet(
            std::move(lifetime), Resource::V321CompileClass::Terrain, Resource::V321CompileUrgency::Background);
        set->_compileMap[context].add(probe);
        ++set->_numberCompileListsToCompile;
        ico.add(set, false);
    }
}

int main() try
{
    Resource::P9DiscretionaryAdmission policy;
    const int a = 0, b = 1;
    require(!policy.takeBootstrap(2, 10, false), "unstarted bootstrap was admitted");
    policy.begin(&a, 1, 2);
    policy.charge(.1);
    require(!policy.takeBootstrap(2, 2.05, false), "bootstrap ignored already charged work");
    require(!policy.takeBootstrap(2, 100, true), "handoff suppression admitted bootstrap");
    require(!policy.takeBootstrap(std::numeric_limits<double>::quiet_NaN(), 100, false), "unknown cost admitted bootstrap");
    require(policy.takeBootstrap(2, 2.2, false), "positive maintenance prevented qualified equal-cap bootstrap");
    require(policy.bootstrapUsed() && !policy.takeProgress(500, 120) && !policy.takeExceptionalService(),
        "bootstrap did not share its token with aged/heavy service");
    policy.begin(&a, 1, 100);
    require(!policy.takeBootstrap(2, 100, false) && policy.remainingMs() == 1.9,
        "repeated traversal minted budget or bootstrap token");
    policy.begin(&a, 2, 2);
    require(policy.takeProgress(120, 120) && !policy.takeBootstrap(2, 100, false),
        "aged service did not consume bootstrap token");
    policy.begin(&a, 3, 2);
    require(policy.takeExceptionalService() && !policy.takeBootstrap(2, 100, false),
        "heavy service did not consume bootstrap token");
    policy.begin(&b, 3, 0);
    require(policy.takeNoGlAttempt(2, .25), "zero allocating budget blocked CPU-only proof");
    policy.chargeNoGlAttempt(.01);
    require(policy.takeNoGlAttempt(2, .25) && !policy.takeNoGlAttempt(2, .25), "CPU-only attempts were not object bounded");
    policy.begin(&b, 3, 2);
    require(!policy.takeNoGlAttempt(2, .25), "repeated traversal reset CPU-only attempt count");
    policy.begin(&b, 4, 0);
    require(policy.takeNoGlAttempt(12, .25), "next frame did not wake CPU-only lane");
    policy.chargeNoGlAttempt(.26);
    require(!policy.takeNoGlAttempt(12, .25) && policy.chargedMs() == .26,
        "CPU-only overrun was hidden or admitted another proof");
    policy.begin(&a, 5, 2);
    for (unsigned i = 0; i < 16; ++i)
        require(policy.takeCompositeMaintenanceVisit(), "bounded maintenance refused an available visit");
    policy.chargeCompositeMaintenance(.1);
    policy.chargeCompositeMaintenance(.05, false);
    policy.begin(&a, 5, 2);
    require(!policy.takeCompositeMaintenanceVisit() && policy.compositeMaintenanceVisits() == 16
            && policy.compositeMaintenanceMs() > .149 && policy.compositeMaintenanceMs() < .151
            && policy.chargedMs() == .1,
        "repeated traversal reset maintenance bounds or charged bookkeeping twice");
    policy.begin(&a, 6, 2);
    require(policy.takeCompositeMaintenanceVisit() && policy.compositeMaintenanceVisits() == 1
            && policy.compositeMaintenanceMs() == 0 && policy.chargedMs() == 0,
        "next owner frame did not reset maintenance admission and accounting");

    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 16; traits->height = 16; traits->windowDecoration = false; traits->doubleBuffer = false;
    osg::ref_ptr<osg::GraphicsContext> context = osg::GraphicsContext::createGraphicsContext(traits);
    require(context && context->realize() && context->makeCurrent(), "native GL context unavailable");
    osg::ref_ptr<osg::FrameStamp> stamp = new osg::FrameStamp;
    context->getState()->setFrameStamp(stamp);
    osg::ref_ptr<CountingManager> manager = new CountingManager(context->getState()->getContextID());
    osg::getOrCreateContextData(context->getState()->getContextID())->set(manager.get());
    unsigned frame = 10;
    auto scheduler = [&](bool enabled, double budget = 2.0, PressureProbe* pressure = nullptr) {
        progressEnvironment(enabled ? "1" : "0");
        Resource::OpenMWCompileSchedulerConfig config;
        config.mMode = 1; config.mTargetFrameRate = 60; config.mMaxBudgetMs = budget;
        config.mCompositePreparation = true; config.mDeleteBudgetMs = 0;
        if (pressure)
        {
            config.mDeleteBudgetMs = .15;
            config.mPressureContext = {nullptr, &pressureSample, pressure};
        }
        config.mResidencySchedulerMode = 1; config.mMaxObjectsPerFrame = 12;
        osg::ref_ptr<Resource::OpenMWIncrementalCompileOperation> ico
            = new Resource::OpenMWIncrementalCompileOperation(config);
        osgUtil::IncrementalCompileOperation::Contexts contexts{context.get()};
        ico->assignContexts(contexts);
        return ico;
    };
    auto begin = [&](Resource::OpenMWIncrementalCompileOperation& ico, double budget, double charge = 0.0) {
        stamp->setFrameNumber(++frame);
        context->clear();
        ico.compositeAdmission()->begin(context, frame, budget);
        ico.compositeAdmission()->charge(charge);
        Resource::OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(0);
    };
    auto detach = [&](Resource::OpenMWIncrementalCompileOperation& ico) {
        osgUtil::IncrementalCompileOperation::Contexts contexts{context.get()};
        ico.removeContexts(contexts);
    };
    {
        auto ico = scheduler(true, 0);
        osg::ref_ptr<Probe> ready = new Probe; ready->ready = true;
        add(*ico, context, ready);
        begin(*ico, 0); (*ico)(context);
        require(ready->proofs == 1 && ready->dispatches == 0 && ico->getToCompile().empty(),
            "zero-budget completion dispatched GL or failed to advance the actual ICO list");
        detach(*ico);
    }
    {
        auto ico = scheduler(true, 0);
        osg::ref_ptr<Probe> stale = new Probe; stale->ready = true; stale->decline = true;
        add(*ico, context, stale);
        begin(*ico, 0); (*ico)(context);
        require(stale->proofs == 1 && stale->dispatches == 0 && !ico->getToCompile().empty(),
            "failed CPU proof fell back to allocating compile at zero budget");
        detach(*ico);
    }
    {
        auto ico = scheduler(true, 0);
        std::vector<osg::ref_ptr<Probe>> ready(20);
        for (auto& probe : ready)
        {
            probe = new Probe; probe->ready = true;
            add(*ico, context, probe);
        }
        begin(*ico, 0); (*ico)(context); (*ico)(context);
        unsigned completed = 0;
        for (const auto& probe : ready)
        {
            completed += probe->proofs;
            require(probe->dispatches == 0, "bounded repeated CPU lane dispatched GL");
        }
        require(completed == 12, "repeated ICO traversal exceeded owner-frame no-GL object cap");
        begin(*ico, 0); (*ico)(context);
        require(ico->getToCompile().empty(), "next owner frame did not drain remaining ready operations");
        detach(*ico);
    }
    {
        auto ico = scheduler(false, 0);
        osg::ref_ptr<Probe> ready = new Probe; ready->ready = true; ready->bootstrap = true;
        add(*ico, context, ready);
        begin(*ico, 0); (*ico)(context);
        require(!ico->progressRepairEnabled() && ready->proofs == 0 && ready->dispatches == 0,
            "OFF control executed the repaired lane");
        detach(*ico);
    }
    {
        auto ico = scheduler(true, 0);
        osg::ref_ptr<Probe> unknown = new Probe; unknown->ready = true; unknown->bootstrap = true;
        add(*ico, context, unknown);
        begin(*ico, 0);
        context->getState()->setFrameStamp(nullptr);
        (*ico)(context);
        require(unknown->proofs == 0 && unknown->dispatches == 0,
            "unknown owner frame entered repaired completion/bootstrap lane");
        context->getState()->setFrameStamp(stamp);
        detach(*ico);
    }
    {
        auto ico = scheduler(true);
        osg::ref_ptr<Probe> first = new Probe, second = new Probe;
        first->bootstrap = second->bootstrap = true;
        add(*ico, context, first); add(*ico, context, second);
        begin(*ico, 2, .1); (*ico)(context); (*ico)(context);
        require(first->dispatches + second->dispatches == 1 && ico->compositeAdmission()->bootstrapUsed(),
            "cold equal-cap target failed bootstrap or repeated traversal allocated twice");
        begin(*ico, 2, .1); (*ico)(context);
        require(first->dispatches + second->dispatches == 2, "next owner frame did not reset bootstrap token");
        detach(*ico);
    }
    {
        auto ico = scheduler(true);
        osg::ref_ptr<Probe> source = new Probe; source->bytes = 8u * 1024u * 1024u;
        add(*ico, context, source);
        begin(*ico, 2, .1); (*ico)(context);
        require(source->dispatches == 0 && !ico->compositeAdmission()->bootstrapUsed(),
            "oversize cold source was granted target-only bootstrap");
        detach(*ico);
    }
    {
        auto ico = scheduler(true);
        osg::ref_ptr<Probe> busy = new Probe; busy->bootstrap = true;
        add(*ico, context, busy);
        begin(*ico, 2, .1);
        Resource::OpenMWIncrementalCompileOperation::publishRenderingTraversalMs(50);
        (*ico)(context);
        require(busy->dispatches == 0 && !ico->compositeAdmission()->bootstrapUsed(),
            "fresh handoff suppression was ignored by bootstrap dispatch");
        detach(*ico);
    }
    for (bool delayed : {false, true})
    {
        auto ico = scheduler(true);
        osg::ref_ptr<Probe> late = new Probe; late->bootstrap = true;
        late->delayDispatchProof = delayed; late->withdrawDispatchProof = !delayed;
        add(*ico, context, late);
        begin(*ico, 2, .1); (*ico)(context);
        require(late->bootstrapChecks >= 2 && late->dispatches == 0 && !ico->compositeAdmission()->bootstrapUsed(),
            "dispatch did not recheck owner proof and headroom after selection");
        detach(*ico);
    }
    {
        PressureProbe pressure;
        auto ico = scheduler(false, 0, &pressure);
        const auto before = manager->flushes;
        begin(*ico, 0); (*ico)(context);
        require(manager->flushes == before + 1 && pressure.samples == 0,
            "OFF control sampled pressure or changed ordinary deletion dispatch");
        detach(*ico);
    }
    {
        PressureProbe pressure;
        auto ico = scheduler(true, 2, &pressure);
        auto cold = [&] {
            osg::ref_ptr<Probe> target = new Probe; target->bootstrap = true; target->estimateSeconds = .002;
            add(*ico, context, target);
            begin(*ico, 2, .1); (*ico)(context);
            require(target->dispatches == 1 && ico->compositeAdmission()->bootstrapUsed(),
                "cleanup fixture did not enter qualified bootstrap");
        };
        const auto before = manager->flushes;
        cold(); (*ico)(context);
        require(manager->flushes == before, "Normal optional deletion followed bootstrap or repeated traversal");
        pressure.pressure = Resource::OpenGlPressure::Critical;
        (*ico)(context);
        require(manager->flushes == before + 1, "same-frame pressure did not bypass bootstrap deletion deferral");
        pressure.pressure = Resource::OpenGlPressure::Normal;
        begin(*ico, 2); (*ico)(context);
        require(manager->flushes == before + 2, "ordinary completion frame did not discharge implicit cleanup debt");
        for (unsigned i = 0; i < 30; ++i) cold();
        require(manager->flushes == before + 2, "bootstrap cleanup deferral did not retain its finite window");
        cold();
        require(manager->flushes == before + 3, "persistent bootstrap load starved bounded cleanup progress");
        detach(*ico);
    }
    progressEnvironment("0");
    context->releaseContext();
    std::cout << "PASS: shared bootstrap/aged/heavy token, zero-budget no-GL ICO completion and stale refusal, OFF control, equal-cap target service with positive maintenance, cold-source refusal, repeated traversal bounds, dispatch proof/headroom recheck, bootstrap deletion deferral with pressure bypass and finite progress\n";
    return 0;
}
catch (const std::exception& error)
{
    progressEnvironment("0");
    std::cerr << error.what() << '\n';
    return 1;
}
