#include <components/sceneutil/occlusionculling.hpp>
#include <apps/openmw/mwrender/objectpaging.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>
#include <components/esm3/loadstat.hpp>
#include <components/files/collections.hpp>
#include <components/resource/assetgpubudget.hpp>
#include <components/resource/resourcesystem.hpp>
#include <components/resource/scenemanager.hpp>
#include <components/sceneutil/assetpaging.hpp>
#include <components/sceneutil/lightmanager.hpp>
#include <components/sceneutil/shadow.hpp>
#include <components/settings/parser.hpp>
#include <components/settings/values.hpp>
#include <components/shader/indirectprojectioncontract.hpp>
#include <components/shader/shadermanager.hpp>
#include <components/vfs/manager.hpp>
#include <components/vfs/registerarchives.hpp>
#include <osg/NodeCallback>
#include <osg/Program>
#include <osgViewer/Viewer>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

// This target compiles ObjectPaging itself with one consistent class definition.
// The provider substitutes collected references only; getChunk quality/cache,
// original SceneManager loading, CopyOp, private admission, strong optimizer,
// real helpers, ICO completion and main-camera selectors are production code.
namespace A = SceneUtil::AssetPaging;
namespace L = SceneUtil::GeneratedStaticLod;
namespace R = Resource;
namespace
{
    void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
    void phase(const char* name,const char* event) { std::cout<<"CHAIN_PHASE name="<<name<<" event="<<event<<'\n'; }
    std::uint64_t consumed(unsigned mask);
    struct Callback final : osg::NodeCallback {};
    struct Registered
    {
        R::ResourceSystem& resources;
        R::BaseResourceManager* manager;
        Registered(R::ResourceSystem& owner,R::BaseResourceManager* value) : resources(owner),manager(value) { resources.addResourceManager(manager); }
        ~Registered() { resources.removeResourceManager(manager); }
    };
    struct Pause
    {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered=false,released=false;
        std::atomic_uint calls{0};
        void operator()()
        { if (calls.fetch_add(1)!=0) return; std::unique_lock lock(mutex); entered=true; changed.notify_all(); changed.wait(lock,[&]{return released;}); }
        void wait()
        { std::unique_lock lock(mutex); require(changed.wait_for(lock,std::chrono::seconds(30),[&]{return entered;}),"deterministic production checkpoint did not arrive"); }
        void resume()
        { std::lock_guard lock(mutex); released=true; changed.notify_all(); }
    };
    struct Join
    {
        std::thread& thread;
        Pause& pause;
        ~Join() { pause.resume(); if (thread.joinable()) thread.join(); }
    };
    std::uint64_t hostOwners(const R::SpeculativeBudget::Stats& stats)
    { return stats.knownOwnerBytes+stats.estimatedOwnerBytes+stats.pendingOwnerBytes; }
    struct Selectors final : osg::NodeVisitor
    {
        Selectors() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::vector<L::Group*> values;
        void apply(osg::Node& node) override
        { if (auto* selector = dynamic_cast<L::Group*>(&node)) values.push_back(selector); traverse(node); }
    };
    struct Draws final : osg::Drawable::DrawCallback
    {
        std::shared_ptr<std::uint64_t> count;
        osg::ref_ptr<osg::Drawable::DrawCallback> delegate;
        void drawImplementation(osg::RenderInfo& info, const osg::Drawable* drawable) const override
        { ++*count; if (delegate) delegate->drawImplementation(info,drawable); else drawable->drawImplementation(info); }
    };
    struct ControlDraws final : osg::NodeVisitor
    {
        ControlDraws() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::shared_ptr<std::uint64_t> count = std::make_shared<std::uint64_t>(0);
        void apply(osg::Node& node) override
        { if (auto* selector = dynamic_cast<L::Group*>(&node)) selector->original()->accept(*this); else traverse(node); }
        void apply(osg::Geometry& geometry) override
        { osg::ref_ptr<Draws> receipt = new Draws; receipt->count=count; receipt->delegate=geometry.getDrawCallback(); geometry.setDrawCallback(receipt); }
    };
    struct Programs final : osg::NodeVisitor
    {
        Programs() : osg::NodeVisitor(TRAVERSE_ALL_CHILDREN) {}
        std::set<osg::Program*> values;
        void apply(osg::Node& node) override
        { if (auto* state=node.getStateSet()) if (auto* program=dynamic_cast<osg::Program*>(state->getAttribute(osg::StateAttribute::PROGRAM))) values.insert(program); traverse(node); }
    };
    struct Pixels final : osg::Camera::DrawCallback
    {
        mutable std::array<unsigned char,128*128*4> data{};
        mutable GLenum error=GL_NO_ERROR;
        mutable std::uint64_t reads=0;
        void operator()(osg::RenderInfo&) const override
        { glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,data.data()); error=glGetError(); ++reads; }
    };
    unsigned rgb(const std::array<unsigned char,128*128*4>& pixels)
    {
        unsigned result=0;
        for (unsigned i=0;i<pixels.size();i+=4) result+=pixels[i]>4 || pixels[i+1]>4 || pixels[i+2]>4;
        return result;
    }
    struct SecondaryReceipt
    {
        unsigned mask=0;
        std::shared_ptr<std::uint64_t> controls;
        std::uint64_t beforeDerived=0,beforeOriginal=0,derived=0,original=0;
        unsigned pixels=0,pixelReads=0;
        GLenum error=GL_NO_ERROR;
        bool drawn=false;
    };
    struct SecondaryDraw final : osg::Camera::DrawCallback
    {
        SecondaryDraw(std::shared_ptr<SecondaryReceipt> value,bool begin) : receipt(std::move(value)),before(begin) {}
        std::shared_ptr<SecondaryReceipt> receipt;
        bool before;
        void operator()(osg::RenderInfo&) const override
        {
            if (before) { receipt->beforeDerived=consumed(receipt->mask); receipt->beforeOriginal=*receipt->controls; }
            else
            {
                receipt->derived=consumed(receipt->mask)-receipt->beforeDerived;
                receipt->original=*receipt->controls-receipt->beforeOriginal;
                std::array<unsigned char,128*128*4> pixels{};
                glReadPixels(0,0,128,128,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
                receipt->pixels=rgb(pixels); ++receipt->pixelReads; receipt->error=glGetError(); receipt->drawn=true;
            }
        }
    };
    std::uint64_t built(unsigned mask)
    { return (mask&A::Lod ? L::Counters::built.load() : 0) + (mask&A::Hlod ? SceneUtil::HlodCounters::built.load() : 0); }
    std::uint64_t consumed(unsigned mask)
    { return (mask&A::Lod ? L::Counters::derivedDraws.load() : 0) + (mask&A::Hlod ? SceneUtil::HlodCounters::consumedDraws.load() : 0); }
    std::uint64_t ready(unsigned mask)
    { return (mask&A::Lod ? A::Counters::lod.ready.load() : 0) + (mask&A::Hlod ? A::Counters::hlod.ready.load() : 0); }
    void sampleGpu()
    {
        Debug::V3GpuMemory::sampleIfDue(false,true,7000,7800);
        const auto sample=Debug::V3GpuMemory::admissionSample();
        require(sample.valid && sample.pressure==Debug::V3GpuMemory::PressureState::Comfortable
            && sample.headroomBytes>R::AssetGpuBudget::HeadroomReserve,"actual adapter headroom must qualify; fixture cannot bypass production GPU admission");
    }
    osg::ref_ptr<osg::Node> upgrade(MWRender::ObjectPaging& paging,R::ResourceSystem& resources,
        std::atomic_bool& abort,std::uint64_t stress=0)
    {
        R::SpeculativeScope scope(resources.speculativeBudget(),&R::ResourceSystem::speculativeSample,&resources,R::SpeculativePriority::NearFuture);
        std::unique_ptr<R::SpeculativeScope::Stage> extra;
        if (stress) extra=std::make_unique<R::SpeculativeScope::Stage>(stress,0);
        A::OptionalUpgradeScope optional(&abort);
        return paging.getChunk(1,{0,0},0,0,false,{0,0,0},true);
    }
    void qualityContracts()
    {
        A::Quality quality; A::Outcome deferred; deferred.defer(A::Lod); deferred.finish(A::Lod);
        require(!quality.satisfies(A::Lod,10),"unattempted quality cannot impersonate a built tier");
        for (unsigned i=0;i<A::MaximumAttempts;++i)
        {
            quality=quality.after(deferred,i*10000);
            require(quality.satisfies(A::Lod,i*10000+1),"transient failure must defer a busy retry");
            if (i+1<A::MaximumAttempts) require(!quality.satisfies(A::Lod,i*10000+A::RetryDelayMilliseconds),"finite delayed retry must become available");
        }
        require((quality.exhausted&A::Lod) && quality.satisfies(A::Lod,100000),"same source generation must exhaust after exactly three attempts");
        A::Quality original; original.completed=A::Hlod; original.attempted=A::Hlod;
        require((original.after(deferred,1).completed&A::Hlod)!=0,"retry of one lane must conserve the other lane's outcome");
        A::Outcome submitted; submitted.completed=A::Hlod; submitted.readiness=std::make_shared<L::Readiness>(); submitted.finish(A::Hlod);
        const auto pending=A::Quality{}.after(submitted,10);
        require(!(pending.settled()&A::Hlod) && pending.satisfies(A::Hlod,11)
            && !pending.satisfies(A::Hlod,10+A::RetryDelayMilliseconds),"CPU/ICO submission must defer then retry when exact GL readiness never arrives");
        auto expired=pending.withoutOwner();
        require(!expired.completed && expired.attempts[1]==1 && (expired.transient&A::Hlod)
            && expired.satisfies(A::Hlod,11) && !expired.satisfies(A::Hlod,10+A::RetryDelayMilliseconds),
            "eviction of an unready owner must conserve finite attempts and backoff");
        for (unsigned i=1;i<A::MaximumAttempts;++i) expired=expired.after(submitted,i*10000).withoutOwner();
        require((expired.exhausted&A::Hlod) && expired.attempts[1]==A::MaximumAttempts,
            "failed/expired ICO readiness must exhaust after exactly three generation attempts");
        std::cout<<"CHAIN_QUALITY attempts="<<unsigned(quality.attempts[0])<<" exhausted="<<quality.exhausted<<" delay_ms="<<A::RetryDelayMilliseconds<<'\n';
        std::cout<<"CHAIN_ICO pending_settled=0 finite_failed_attempts="<<unsigned(expired.attempts[1])<<" eviction_retains_backoff=1\n";
    }
}

int main(int argc,char** argv) try
{
    std::cout<<std::unitbuf;
    require(argc>=6,"usage: p9-asset-paging-chain mask openmw.cfg sourceRoot activePlugin mesh1 [mesh2 ...] [--retry]");
    const unsigned mask=static_cast<unsigned>(std::stoul(argv[1]));
    require(mask==A::Lod || mask==A::Hlod,"chain consumption fixture isolates L1 or H1; combined runtime acceptance requires user captures");
#ifdef _WIN32
    _putenv_s("OPENMW_P9_ASSET_MASK",std::to_string(mask).c_str());
#else
    setenv("OPENMW_P9_ASSET_MASK",std::to_string(mask).c_str(),1);
#endif
    bool retry=false;
    const auto sourceRoot=std::filesystem::u8path(argv[3]);
    Settings::SettingsFileParser settings;
    settings.loadSettingsFile(sourceRoot/"files/settings-default.cfg",Settings::Manager::mDefaultSettings);
    Settings::StaticValues::initDefaults(); Settings::Manager::mUserSettings=Settings::Manager::mDefaultSettings; Settings::StaticValues::init();
    // Frozen production strong optimizer. Disable unrelated experimental lanes
    // so the receipt attributes actual work to the isolated asset lane.
    Settings::cells().mV38WorldBatchingMode.set(3); Settings::cells().mV39BatchOptimizerMode.set(3);
    Settings::cells().mOptimizedMWParallelTemplatePrefetch.set(false);
    Settings::cells().mOptimizedMWShadowStaticBatching.set(false);
    Settings::camera().mOcclusionCulling.set(false);
    Files::PathContainer directories; std::vector<std::string> archives,content;
    std::ifstream config(std::filesystem::u8path(argv[2])); require(bool(config),"read-only loadout config unavailable");
    for (std::string line;std::getline(config,line);)
    {
        if (line.starts_with("data="))
        { auto value=line.substr(5); if (value.size()>1 && value.front()=='"' && value.back()=='"') value=value.substr(1,value.size()-2); const auto path=std::filesystem::u8path(value); if (std::filesystem::is_directory(path)) directories.push_back(path); }
        if (line.starts_with("fallback-archive=")) archives.push_back(line.substr(17));
        if (line.starts_with("content=")) content.push_back(line.substr(8));
    }
    require(!directories.empty(),"loadout has no source directories");
    phase("load","begin");
    VFS::Manager vfs; VFS::registerArchives(&vfs,Files::Collections(directories),archives,true,nullptr); vfs.buildIndex();
    const std::string plugin=argv[4]; require(std::find(content.begin(),content.end(),plugin)!=content.end(),"static source plugin must be active in the loadout");
    std::map<VFS::Path::Normalized,ESM::RefId> statics;
    std::map<VFS::Path::Normalized,VFS::Path::Normalized> recordModels;
    ESM::ESMReader records; records.open(vfs.get(VFS::Path::Normalized(plugin)),std::filesystem::u8path(plugin));
    unsigned scanned=0;
    while (records.hasMoreRecs())
    {
        require(++scanned<=250000,"bounded static record scan"); const auto name=records.getRecName(); records.getRecHeader();
        if (name.toInt()!=ESM::REC_STAT) {records.skipRecord();continue;}
        ESM::Static value; bool deleted=false; value.load(records,deleted);
        const VFS::Path::Normalized model("meshes/"+value.mModel.getNormalized().value());
        if (deleted) {statics.erase(model);recordModels.erase(model);}
        else
        {
            statics.insert_or_assign(model,value.mId);
            recordModels.insert_or_assign(model,VFS::Path::Normalized(value.mModel.getNormalized().value()));
        }
    }
    phase("load","done");
    R::ResourceSystem resources(&vfs,100,nullptr);
    R::SpeculativeBudget::Config limits; limits.transientLimit=R::AssetPolicy::CpuTransientLimit;
    limits.maximumJobs=4; limits.reservedNearFutureJobs=1;
    resources.enableOpenGlSpeculativeBudget({},limits);
    for (unsigned attempt=0;attempt<30 && !R::SpeculativeBudget::newOwnerWorkUsable(R::ResourceSystem::speculativeSample(&resources),R::SpeculativePriority::NearFuture);++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(R::SpeculativeBudget::newOwnerWorkUsable(R::ResourceSystem::speculativeSample(&resources),R::SpeculativePriority::NearFuture),"actual host memory sample must qualify production speculative budget");
    auto* scenes=resources.getSceneManager(); scenes->setShaderPath(sourceRoot/"files/shaders");
    scenes->setAutoUseNormalMaps(true); scenes->setNormalMapPattern("_n"); scenes->setNormalHeightMapPattern("_nh");
    scenes->setAutoUseSpecularMaps(true); scenes->setSpecularMapPattern("_s"); scenes->setFilterSettings("linear","linear","linear",16.f);
    osg::ref_ptr<SceneUtil::LightManager> lighting=new SceneUtil::LightManager;
    auto defines=Shader::getDefaultDefines();
    for (const auto& [name,value]:SceneUtil::ShadowManager::getShadowsDisabledDefines()) defines[name]=value;
    for (const auto& [name,value]:lighting->getLightDefines()) defines[name]=value;
    scenes->getShaderManager().setGlobalDefines(defines);
    std::vector<MWRender::ObjectPaging::FixtureReference> population;
    phase("certify","begin");
    for (int i=5;i<argc && i<21;++i)
    {
        if (std::string_view(argv[i])=="--retry") {retry=true;continue;}
        const auto model=VFS::Path::toNormalized(std::string_view(argv[i]));
        require(statics.contains(model),"fixture model must have a real live STAT record in the active loadout");
        auto original=scenes->getTemplate(model,false);
        const auto l1=L::certify(*original); const bool h1=SceneUtil::certifySelectiveHlodTemplate(*original) && SceneUtil::qualifySelectiveHlodTemplate(*original);
        std::cout<<"CHAIN_SOURCE model="<<model.value()<<" static_id="<<statics.at(model).toString()<<" plugin="<<plugin
            <<" winning="<<vfs.getArchive(model)<<" l1_certificate_ok="<<(l1==L::Refusal::None)<<" l1_refusal="<<static_cast<unsigned>(l1)<<" h1_certificate_ok="<<h1<<" l1_peak_bytes="<<L::cookPeakBytes(*original)<<'\n';
        require(mask==A::Lod ? l1==L::Refusal::None : h1,"real SceneManager source must pass the original certificate before CopyOp");
        // The production getModel contract returns STAT's relative model. Its
        // correctMeshPath stage supplies "meshes/"; preserve the actual record
        // field here rather than passing the already resolved VFS key twice.
        population.push_back({nullptr,recordModels.at(model),ESM::REC_STAT,osg::Vec3f(float(population.size())*original->getBound().radius()*3.f,0,0),1.f,false});
    }
    require(!population.empty() && (mask!=A::Hlod || population.size()>=2),"bounded real static source family required");
    phase("certify","done");
    qualityContracts();
    osg::ref_ptr<osg::GraphicsContext::Traits> traits=new osg::GraphicsContext::Traits;
    traits->readDISPLAY(); traits->width=128; traits->height=128; traits->pbuffer=true; traits->doubleBuffer=false; traits->windowDecoration=false;
    osg::ref_ptr<osg::GraphicsContext> context=osg::GraphicsContext::createGraphicsContext(traits); require(context.valid(),"real GL context required");
    osgViewer::Viewer viewer; viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);
    osg::ref_ptr<osg::Camera> retainedCamera=viewer.getCamera(); auto* camera=retainedCamera.get(); camera->setGraphicsContext(context); camera->setViewport(0,0,128,128);
    camera->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    camera->setProjectionMatrixAsPerspective(60,1,1,1000000); camera->setClearColor({0,0,0,0});
    auto* state=camera->getOrCreateStateSet();
    state->addUniform(new osg::Uniform("projectionMatrix",osg::Matrixf(camera->getProjectionMatrix())));
    state->addUniform(new osg::Uniform("screenRes",osg::Vec2(128,128))); state->addUniform(new osg::Uniform("near",1.f)); state->addUniform(new osg::Uniform("far",1000000.f));
    state->addUniform(new osg::Uniform("fogColor",osg::Vec4(0,0,0,1))); state->addUniform(new osg::Uniform("fogEnd",1000000.f)); state->addUniform(new osg::Uniform("fogStart",999999.f));
    osg::ref_ptr<SceneUtil::Light> sun=new SceneUtil::Light; sun->setPosition({-.3f,-.7f,.5f,0}); sun->setDiffuse({.7f,.7f,.7f,1}); sun->setAmbient({.5f,.5f,.5f,1}); sun->setSpecular({.15f,.15f,.15f,1}); lighting->setSunlight(sun);
    osg::ref_ptr<Pixels> pixels=new Pixels; camera->setFinalDrawCallback(pixels);
    osg::ref_ptr<osgUtil::IncrementalCompileOperation> ico=new osgUtil::IncrementalCompileOperation;
    viewer.setIncrementalCompileOperation(ico); scenes->setIncrementalCompileOperation(ico);
    phase("context","begin");
    osg::ref_ptr<osg::Group> sceneRoot=new osg::Group; sceneRoot->addChild(lighting);
    viewer.setSceneData(sceneRoot); viewer.realize();
    require(!ico->getContextSet().empty(),"actual ICO context registration required before optional chunk work");
    phase("context","done");
    MWRender::ObjectPaging paging(scenes,ESM::Cell::sDefaultWorldspaceId); paging.setAssetMainCamera(camera); paging.setFixturePopulation(population);
    Registered pagingRegistration(resources,&paging);
    std::atomic_bool abort{false};
    const auto beforeBuilt=built(mask);
    osg::ref_ptr<osg::Node> ordinary;
    phase("required_chunk","begin");
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); ordinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    phase("required_chunk","done");
    Selectors weak; ordinary->accept(weak); require(weak.values.empty() && built(mask)==beforeBuilt,"required readiness/control must not fabricate optional asset quality");
    if (retry)
    {
        phase("stressed_upgrade","begin");
        const auto privateDenied=(mask&A::Lod ? A::Counters::lod.privateCpuDenied.load() : A::Counters::hlod.privateCpuDenied.load());
        auto deferred=upgrade(paging,resources,abort,100*R::SpeculativeBudget::MiB);
        require(built(mask)==beforeBuilt,"228 MiB outer overlap must refuse an additional 32 MiB private clone without cooking");
        require((mask&A::Lod ? A::Counters::lod.privateCpuDenied.load() : A::Counters::hlod.privateCpuDenied.load())>privateDenied,"actual production private CPU gate must report refusal");
        auto backoff=upgrade(paging,resources,abort); require(backoff==deferred && built(mask)==beforeBuilt,"same generation must retain ordinary fallback during retry delay");
        std::this_thread::sleep_for(std::chrono::milliseconds(A::RetryDelayMilliseconds+100));
        std::cout<<"CHAIN_TRANSIENT same_generation=1 private_cpu_denied=1 fallback_during_delay=1\n";
        phase("stressed_upgrade","done");
    }
    sampleGpu(); const auto beforeReady=ready(mask), beforeDraw=consumed(mask);
    const auto ownersBefore=hostOwners(resources.speculativeBudget()->stats());
    phase("cook","begin");
    auto candidate=upgrade(paging,resources,abort); Selectors selected; candidate->accept(selected);
    phase("cook","done");
    require(candidate!=ordinary && !selected.values.empty() && built(mask)>beforeBuilt,"actual getChunk must replace weak cached quality with built optional tiers");
    require(ready(mask)==beforeReady,"CPU candidate cannot claim GL completion");
    require(hostOwners(resources.speculativeBudget()->stats())>ownersBefore,"actual private candidate must retain a host ownership charge after preparation stages unwind");
    ControlDraws controls; candidate->accept(controls); lighting->addChild(candidate);
    const auto bound=candidate->getBound(); double error=0; for (auto* selector:selected.values) error=std::max(error,double(selector->error()));
    const double distance=std::max(double(bound.radius())*4.,double(bound.radius())+error*std::sqrt(3.)*64./.6);
    camera->setViewMatrixAsLookAt(osg::Vec3d(bound.center())+osg::Vec3d(0,-distance,0),osg::Vec3d(bound.center()),{0,0,1});
    // First cull cannot consume a tier whose actual queued GL work has not run.
    // Hold only the queued ICO operation for one actual main draw. This tests
    // pending fallback deterministically without writing a readiness flag or
    // replacing any compile operator/completion callback.
    context->remove(ico.get());
    phase("pending_main_draw","begin");
    viewer.advance(); viewer.eventTraversal(); viewer.updateTraversal(); viewer.renderingTraversals();
    phase("pending_main_draw","done");
    require(consumed(mask)==beforeDraw && *controls.count>0,"pending production ICO tier must render ordinary geometry on its first main view");
    const auto controlPixels=pixels->data;
    context->add(ico.get());
    phase("ico_completion","begin");
    for (unsigned frame=0;frame<90 && ready(mask)==beforeReady;++frame) viewer.frame();
    phase("ico_completion","done");
    require(ready(mask)>beforeReady,"real ObjectPaging CompileCompleted must certify the queued GL objects");
    const auto drawStart=consumed(mask); phase("derived_draw","begin"); viewer.frame(); viewer.frame(); phase("derived_draw","done");
    require(consumed(mask)>drawStart,"actual main camera must consume production-generated geometry");
    Programs programs; candidate->accept(programs); require(!programs.values.empty(),"actual shipped object program required");
    for (auto* program:programs.values)
    { require(Shader::IndirectProjection::matches(*program),"stock source shader certificate required"); const auto* linked=program->getPCP(*context->getState()); require(linked && linked->isLinked(),"actual production object shader must link"); }
    unsigned controlRgb=0,tierRgb=0; for (unsigned i=0;i<controlPixels.size();i+=4)
    { controlRgb+=controlPixels[i]>4 || controlPixels[i+1]>4 || controlPixels[i+2]>4; tierRgb+=pixels->data[i]>4 || pixels->data[i+1]>4 || pixels->data[i+2]>4; }
    require(controlRgb>16 && tierRgb>16 && pixels->error==GL_NO_ERROR,"real stock shaded object pixels and clean GL required");
    if (mask==A::Lod) require(L::Counters::derivedIndices<L::Counters::originalIndices,"real L1 main draw must carry fewer indices");
    if (mask==A::Hlod) require(SceneUtil::HlodCounters::proxySubmissions<SceneUtil::HlodCounters::controlSubmissions,"real H1 must reduce strong optimized residual submissions");
    std::cout<<"CHAIN_CONSUMED mask="<<mask<<" ready="<<ready(mask)-beforeReady<<" draws="<<consumed(mask)-drawStart
        <<" control_rgb="<<controlRgb<<" tier_rgb="<<tierRgb<<" original_indices="<<L::Counters::originalIndices
        <<" derived_indices="<<L::Counters::derivedIndices<<" control_submissions="<<SceneUtil::HlodCounters::controlSubmissions
        <<" proxy_submissions="<<SceneUtil::HlodCounters::proxySubmissions<<" gpu_peak_debt="<<R::AssetGpuBudget::peakBytes<<'\n';
    // A real nested secondary draw must preserve the main Viewer/camera and
    // service its registered ICO. Isolate consumption within the secondary's
    // pre/post callbacks while the main draw independently continues afterward.
    auto secondaryReceipt=std::make_shared<SecondaryReceipt>(); secondaryReceipt->mask=mask; secondaryReceipt->controls=controls.count;
    osg::ref_ptr<osg::Camera> secondary=new osg::Camera;
    secondary->setReferenceFrame(osg::Transform::ABSOLUTE_RF); secondary->setRenderOrder(osg::Camera::PRE_RENDER);
    secondary->setProjectionMatrix(camera->getProjectionMatrix()); secondary->setViewMatrix(camera->getViewMatrix());
    secondary->setViewport(0,0,128,128); secondary->setComputeNearFarMode(osg::CullSettings::DO_NOT_COMPUTE_NEAR_FAR);
    secondary->setClearColor({0,0,0,0}); secondary->setClearMask(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    secondary->setPreDrawCallback(new SecondaryDraw(secondaryReceipt,true)); secondary->setPostDrawCallback(new SecondaryDraw(secondaryReceipt,false));
    secondary->addChild(lighting); sceneRoot->addChild(secondary); viewer.frame();
    std::cout<<"CHAIN_SECONDARY derived_draws="<<secondaryReceipt->derived<<" original_draws="<<secondaryReceipt->original
        <<" rgb="<<secondaryReceipt->pixels<<" pixel_reads="<<secondaryReceipt->pixelReads<<" gl_error="<<secondaryReceipt->error<<" drawn="<<secondaryReceipt->drawn<<'\n';
    require(secondaryReceipt->drawn && !secondaryReceipt->derived && secondaryReceipt->original>0
        && secondaryReceipt->pixels>16 && secondaryReceipt->error==GL_NO_ERROR,"actual secondary draw must consume visible original geometry without a derived draw");
    sceneRoot->removeChild(secondary); secondary->removeChild(lighting);
    candidate->releaseGLObjects(context->getState()); const auto beforeRelease=consumed(mask),beforeReleaseOriginal=*controls.count,beforeReleaseReads=pixels->reads; viewer.frame();
    std::cout<<"CHAIN_RELEASE derived_draws="<<consumed(mask)-beforeRelease<<" original_draws="<<*controls.count-beforeReleaseOriginal
        <<" rgb="<<rgb(pixels->data)<<" pixel_reads="<<pixels->reads-beforeReleaseReads<<" gl_error="<<pixels->error<<'\n';
    require(consumed(mask)==beforeRelease && *controls.count>beforeReleaseOriginal && pixels->reads>beforeReleaseReads && rgb(pixels->data)>16 && pixels->error==GL_NO_ERROR,
        "context release must draw visible original geometry without optional consumption");
    // Immutable original-source controller rejection must survive paging CopyOp.
    auto negativePopulation=population;
    auto templateNode=scenes->getTemplate(VFS::Path::Normalized("meshes/"+population.front().model.value()),false);
    auto callbackSource=osg::clone(templateNode.get(),osg::CopyOp::DEEP_COPY_NODES|osg::CopyOp::DEEP_COPY_DRAWABLES);
    callbackSource->setCullCallback(new Callback); negativePopulation.front().original=callbackSource;
    MWRender::ObjectPaging negative(scenes,ESM::Cell::sDefaultWorldspaceId); negative.setAssetMainCamera(camera); negative.setFixturePopulation(negativePopulation);
    Registered negativeRegistration(resources,&negative);
    const auto rejectionBefore=(mask&A::Lod ? A::Counters::lod.sourceRejected.load() : A::Counters::hlod.sourceRejected.load());
    const auto negativeBuilt=built(mask); sampleGpu(); auto refused=upgrade(negative,resources,abort);
    require((mask&A::Lod ? A::Counters::lod.sourceRejected.load() : A::Counters::hlod.sourceRejected.load())>rejectionBefore,"controller source must be refused before CopyOp strips its behavior");
    if (mask==A::Lod) require(built(mask)==negativeBuilt,"controller-only original must retain ordinary graph");
    negativePopulation=population; for (auto& ref:negativePopulation) ref.occluderOwner=true;
    MWRender::ObjectPaging owners(scenes,ESM::Cell::sDefaultWorldspaceId); owners.setAssetMainCamera(camera); owners.setFixturePopulation(negativePopulation);
    Registered ownerRegistration(resources,&owners);
    const auto ownerBuilt=built(mask); const auto ownerBefore=(mask&A::Lod ? A::Counters::lod.refusals[unsigned(A::Gate::Occluder)].load() : A::Counters::hlod.refusals[unsigned(A::Gate::Occluder)].load());
    auto unchanged=upgrade(owners,resources,abort);
    require(built(mask)==ownerBuilt && (mask&A::Lod ? A::Counters::lod.refusals[unsigned(A::Gate::Occluder)].load() : A::Counters::hlod.refusals[unsigned(A::Gate::Occluder)].load())>ownerBefore,"MSOC owners must keep original production topology");
    std::cout<<"CHAIN_NEGATIVE controller_original=1 occluder_owner=1 secondary_original=1 release_original=1\n";
    // Clear only the real cache owner, retaining the quality side table exactly
    // as expiry does. Required reconstruction must not inherit completion from
    // the evicted graph; an actual optional upgrade must cook/install again.
    using CacheBase=R::GenericResourceManager<MWRender::ChunkId>;
    phase("eviction","begin");
    paging.CacheBase::clearCache();
    osg::ref_ptr<osg::Node> rebuiltOrdinary;
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); rebuiltOrdinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    Selectors evicted; rebuiltOrdinary->accept(evicted); require(evicted.values.empty(),"cache-only eviction must reconstruct ordinary required topology");
    const auto rebuiltBefore=built(mask); sampleGpu(); auto rebuilt=upgrade(paging,resources,abort);
    require(rebuilt!=rebuiltOrdinary && built(mask)>rebuiltBefore,"evicted completion must not settle an ordinary rebuilt graph");
    std::cout<<"CHAIN_EVICTION cache_only=1 required_ordinary=1 optional_rebuilt=1\n";
    phase("eviction","done");

    // Complete this exact rebuilt owner before the independent publication
    // test evicts it. An unready eviction correctly retains its cooldown; it
    // cannot serve as the already-completed setup for a fresh upgrade race.
    phase("publication_ready","begin");
    lighting->removeChild(candidate); lighting->addChild(rebuilt);
    const auto rebuiltReady=ready(mask),rebuiltDraw=consumed(mask);
    for (unsigned frame=0;frame<90 && ready(mask)==rebuiltReady;++frame) viewer.frame();
    viewer.frame();
    Selectors rebuiltSelectors; rebuilt->accept(rebuiltSelectors);
    unsigned preparedSelectors=0;
    for (auto* selector:rebuiltSelectors.values) preparedSelectors+=selector->compilationReady();
    std::cout<<"CHAIN_SETUP_DIAGNOSTIC ready_before="<<rebuiltReady<<" ready_now="<<ready(mask)
        <<" draw_before="<<rebuiltDraw<<" draw_now="<<consumed(mask)<<" selectors="<<rebuiltSelectors.values.size()
        <<" prepared_selectors="<<preparedSelectors<<" ico_contexts="<<ico->getContextSet().size()
        <<" ico_pending="<<ico->getToCompile().size()<<" ico_completed="<<ico->getCompiled().size()<<'\n';
    unsigned icoOperations=0;
    for (const auto& operation:context->getOperationsQueue()) icoOperations+=operation.get()==ico.get();
    Programs rebuiltPrograms; rebuilt->accept(rebuiltPrograms); unsigned linkedPrograms=0,needsLinkPrograms=0;
    for (auto* program:rebuiltPrograms.values)
    { const auto* status=program->getPCP(*context->getState()); linkedPrograms+=status && status->isLinked(); needsLinkPrograms+=!status || status->needsLink(); }
    std::cout<<"CHAIN_CONTEXT_DIAGNOSTIC ico_operations="<<icoOperations<<" context_realized="<<context->isRealized()
        <<" context_cameras="<<context->getCameras().size()<<" main_camera="<<(viewer.getCamera()==camera)
        <<" camera_context="<<(camera->getGraphicsContext()==context)<<" scene_root="<<(viewer.getSceneData()==sceneRoot)
        <<" programs="<<rebuiltPrograms.values.size()<<" linked_programs="<<linkedPrograms<<" programs_need_link="<<needsLinkPrograms<<'\n';
    for (const auto& compiled:ico->getToCompile())
    {
        std::size_t operations=0;
        for (const auto& [owner,list]:compiled->_compileMap) operations+=list._compileOps.size();
        std::cout<<"CHAIN_ICO_DIAGNOSTIC exact_rebuilt="<<(compiled->_subgraphToCompile==rebuilt)
            <<" maps="<<compiled->_compileMap.size()<<" operations="<<operations<<" compiled="<<compiled->compiled()<<'\n';
    }
    require(ready(mask)>rebuiltReady && consumed(mask)>rebuiltDraw,
        "rebuilt publication setup must complete actual ICO and consume its own main tier");
    lighting->removeChild(rebuilt); lighting->addChild(candidate);
    std::cout<<"CHAIN_PUBLICATION_READY ready="<<ready(mask)-rebuiltReady<<" draws="<<consumed(mask)-rebuiltDraw<<'\n';
    phase("publication_ready","done");

    // Force a stale advisory node across a newer publication. getChunk's
    // authoritative node/quality pair must return the newly published owner.
    phase("publication","begin");
    paging.CacheBase::clearCache();
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); rebuiltOrdinary=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    Pause publication; paging.setFixtureBeforeCache([&]{publication();});
    osg::ref_ptr<osg::Node> raced; std::exception_ptr raceError;
    std::thread demand([&]{try {raced=paging.getChunk(1,{0,0},0,0,false,{0,0,0},false);} catch(...) {raceError=std::current_exception();}});
    Join demandJoin{demand,publication};
    publication.wait(); sampleGpu(); auto published=upgrade(paging,resources,abort); publication.resume(); demand.join(); paging.setFixtureBeforeCache({});
    if (raceError) std::rethrow_exception(raceError);
    require(published!=rebuiltOrdinary && raced==published,"old advisory node cannot be returned with newer strong quality metadata");
    std::cout<<"CHAIN_PUBLICATION paired_owner=1 stale_advisory_refused=1\n";
    phase("publication","done");

    // An old-VFS caller pauses before cache invalidation while a real new index
    // generation publishes its required owner. It must cancel without deleting
    // that fresh owner, even though its initial advisory pointer was old.
    phase("epoch","begin");
    Pause epoch; paging.setFixtureBeforeCache([&]{epoch();}); std::atomic_bool staleCancelled{false}; std::exception_ptr staleError;
    std::thread stale([&]{try {paging.getChunk(1,{0,0},0,0,false,{0,0,0},false);} catch(const SceneUtil::PagingWorkCancelled&) {staleCancelled=true;} catch(...) {staleError=std::current_exception();}});
    Join staleJoin{stale,epoch};
    epoch.wait(); vfs.buildIndex();
    osg::ref_ptr<osg::Node> fresh;
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); fresh=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    epoch.resume(); stale.join(); paging.setFixtureBeforeCache({}); if (staleError) std::rethrow_exception(staleError);
    require(staleCancelled && paging.getChunk(1,{0,0},0,0,false,{0,0,0},false)==fresh,"stale source epoch cannot remove the freshly published cache owner");
    std::cout<<"CHAIN_EPOCH cancelled_old=1 fresh_owner_preserved=1\n";
    phase("epoch","done");

    // Hold a fully built, queued private candidate immediately before cache
    // publication. A new source epoch wins with ordinary required topology;
    // the losing candidate's CompileSet still retains CPU storage after its
    // worker/outer stages finish. Its host receipt must survive until ICO drops
    // the exact graph, without a global CPU retain.
    phase("queued_loser","begin");
    for (unsigned step=0;step<30;++step) viewer.frame();
    const auto heldBefore=hostOwners(resources.speculativeBudget()->stats());
    Pause queued; paging.setFixtureBeforePublish([&]{queued();}); std::atomic_bool queuedCancelled{false}; std::exception_ptr queuedError;
    context->remove(ico.get()); sampleGpu();
    std::thread producer([&]{try {upgrade(paging,resources,abort);} catch(const SceneUtil::PagingWorkCancelled&) {queuedCancelled=true;} catch(...) {queuedError=std::current_exception();}});
    Join producerJoin{producer,queued};
    queued.wait(); vfs.buildIndex();
    { SceneUtil::PagingWorkScope required(&abort,SceneUtil::PagingWorkScope::Phase::RequiredReadiness); fresh=paging.getChunk(1,{0,0},0,0,false,{0,0,0},true); }
    queued.resume(); producer.join(); paging.setFixtureBeforePublish({}); if (queuedError) std::rethrow_exception(queuedError);
    const auto held=hostOwners(resources.speculativeBudget()->stats());
    require(queuedCancelled && held>heldBefore && resources.speculativeBudget()->stats().futureBytes==0,
        "losing queued candidate must retain host ownership after worker stages unwind");
    context->add(ico.get()); for (unsigned step=0;step<90 && hostOwners(resources.speculativeBudget()->stats())>=held;++step) viewer.frame();
    const auto drained=hostOwners(resources.speculativeBudget()->stats()); require(drained<held,"actual ICO graph release must retire the uncached host ownership charge");
    std::cout<<"CHAIN_QUEUED_LOSER host_before="<<heldBefore<<" host_held="<<held<<" host_after_ico="<<drained<<" worker_future=0\n";
    phase("queued_loser","done");
    viewer.setSceneData(nullptr); viewer.setIncrementalCompileOperation(nullptr); scenes->setIncrementalCompileOperation(nullptr);
    viewer.stopThreading(); resources.releaseGLObjects(context->getState()); context->close(); R::AssetGpuBudget::discardAfterContextsClosed();
    require(resources.speculativeBudget()->stats().futureBytes==0,"production preparation stages must release bounded CPU overlap");
    std::cout<<"CHAIN_PASS source_population_substituted=1 source_loader_substituted=0 cook_substituted=0 readiness_substituted=0 cpu_limit="<<R::AssetPolicy::CpuTransientLimit<<'\n';
    return 0;
}
catch (const std::exception& error) { std::cerr<<"CHAIN_FAILURE "<<error.what()<<'\n'; return 1; }
