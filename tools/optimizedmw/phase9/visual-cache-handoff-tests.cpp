#include <components/resource/visualcachehandoff.hpp>
#include <components/resource/objectcache.hpp>
#include <components/resource/sharedstatecache.hpp>
#include <osg/Group>
#include <osg/Material>
#include <osg/Texture2D>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace
{
    unsigned checks=0;
    void require(bool result,const char* message)
    { ++checks; if(!result) throw std::runtime_error(message); }
    class Item final : public SceneUtil::WorkItem
    {
    public:
        Item(Resource::VisualCacheHandoffPolicy& policy,Resource::VisualCacheHandoffPolicy::Admission token,
            std::atomic<unsigned>& work,bool fail=false) : p(policy),t(token),w(work),f(fail) {}
        void doWork() override
        {
            result=p.runOwned(t,[]{return std::uint64_t{1050};},[&]{++w;if(f) throw std::runtime_error("optional failure");});
        }
        Resource::VisualCacheHandoffPolicy::Outcome result=Resource::VisualCacheHandoffPolicy::Outcome::Skipped;
    private:
        Resource::VisualCacheHandoffPolicy& p;
        Resource::VisualCacheHandoffPolicy::Admission t;
        std::atomic<unsigned>& w;
        bool f;
    };
}
int main() try
{
    using P=Resource::VisualCacheHandoffPolicy;
    P disabled(false); disabled.setLoading(true,1000);disabled.setLoading(false,1001);
    require(!disabled.tryBegin(1050,true),"Ordinary disabled control cannot admit work");
    P p(true); require(!p.tryBegin(1000,true),"No handoff before an actual loading transition");
    require(p.setLoading(true,1000),"Outer loading-on reports its actual edge");
    require(!p.tryBegin(1001,true),"Required loading rejects handoff work");
    require(!p.setLoading(true,1001),"Repeated loading-on is not a new boundary");
    require(p.setLoading(false,1001),"Outer loading-off permits immediate publication");
    require(!p.setLoading(false,1002),"Repeated loading-off cannot renew the finite window");
    require(!p.tryBegin(1050,false),"Missing worker rejects handoff");
    auto first=p.tryBegin(1050,true); require(bool(first),"Actual handoff admits one bounded pass");
    require(!p.tryBegin(1100,true),"Pending pass cannot be duplicated");
    p.setLoading(true,1100);
    unsigned called=0;
    require(p.runOwned(first,[]{return std::uint64_t{1101};},[&]{++called;})==P::Outcome::Skipped && !called,
        "New loading invalidates a queued old handoff without a frame wait");
    p.setLoading(false,1102);
    auto next=p.tryBegin(1150,true); require(bool(next),"New handoff cannot inherit the old task");
    require(p.runOwned(next,[]{return std::uint64_t{1150};},[]{throw std::runtime_error("failure");})==P::Outcome::Error,
        "Optional worker exceptions are contained and retire admission");
    require(!p.tryBegin(1151,true),"Cadence rejects immediate duplicate work");
    auto resumed=p.tryBegin(1166,true); require(bool(resumed),"Failure does not leave pending authority stuck");
    require(p.runOwned(resumed,[]{return std::uint64_t{9000};},[&]{++called;})==P::Outcome::Skipped,
        "Expired queued work cannot reclaim unrelated later content");
    require(!p.tryBegin(9000,true),"Expired phase cannot publish new work");

    P limited(true);limited.setLoading(true,1);limited.setLoading(false,2);
    for(unsigned i=0;i<P::MaximumPasses;++i)
    {
        const auto now=100+i*P::CadenceMilliseconds;
        auto token=limited.tryBegin(now,true);require(bool(token),"Every finite phase slot admits once");
        require(limited.runOwned(token,[=]{return now;},[]{})==P::Outcome::Completed,"Real owned pass completes");
    }
    require(!limited.tryBegin(3000,true),"Maximum phase work cannot grow without bound");
    P timed(true);timed.setLoading(true,100);timed.setLoading(false,101);
    auto token=timed.tryBegin(110,true);
    require(timed.runOwned(token,[]()->std::uint64_t{throw std::runtime_error("clock");},[]{})==P::Outcome::Error,
        "Clock/diagnostic exception retires owned admission");
    require(bool(timed.tryBegin(126,true)),"Clock failure preserves next valid admission");

    P queued(true);queued.setLoading(true,1000);queued.setLoading(false,1001);
    Resource::VisualCacheHandoffQueueOwner owner(queued);
    osg::ref_ptr<SceneUtil::WorkQueue> queue=new SceneUtil::WorkQueue(1);
    owner.attach(queue);
    std::atomic<unsigned> work{0};
    auto qt=queued.tryBegin(1050,owner.available());
    osg::ref_ptr<Item> item=new Item(queued,qt,work,true);
    owner.publish(item);item->waitTillDone();
    require(work==1 && item->result==P::Outcome::Error,"Actual existing one-thread queue contains optional failure");
    require(!owner.busy(),"Real failed task returns terminal ownership");
    osg::ref_ptr<SceneUtil::WorkQueue> otherQueue=new SceneUtil::WorkQueue(0);
    require(!owner.detach(otherQueue),"Mismatched shutdown cannot detach another queue");
    require(owner.detach(queue),"Exact shutdown closes and joins its own worker task");
    require(!owner.available() && !queued.tryBegin(1100,true),"Shutdown permanently removes old authority");

    osg::ref_ptr<Resource::GenericObjectCache<unsigned>> cache=new Resource::GenericObjectCache<unsigned>;
    osg::ref_ptr<osg::Group> live=new osg::Group;
    cache->addEntryToObjectCache(0,live);
    for(unsigned i=1;i<=20;++i) cache->addEntryToObjectCache(i,new osg::Group);
    for(unsigned pass=0;pass<8;++pass)
    {
        Resource::CacheMaintenanceBudget budget(8,2,std::chrono::seconds(1));
        Resource::CacheMaintenanceScope scope(budget);
        const auto removed=cache->trimUnused(2);
        require(removed<=2 && budget.scanned<=8,"Actual object-cache retirement respects caller scan/release bounds");
        require(cache->getRefFromObjectCache(0)==live,"Actual external graph owner is never reclaimed");
    }
    require(cache->getStats().mSize<21,"Actual cache-only retirement occurs before ordinary residence expiry");

    osg::ref_ptr<Resource::SharedStateManager> shared=new Resource::SharedStateManager;
    osg::ref_ptr<osg::Group> graph=new osg::Group;
    graph->getOrCreateStateSet()->setDataVariance(osg::Object::STATIC);
    osg::ref_ptr<osg::Image> image=new osg::Image; image->allocateImage(4,4,1,GL_RGBA,GL_UNSIGNED_BYTE);
    graph->getStateSet()->setTextureAttribute(0,new osg::Texture2D(image));
    shared->share(graph);
    auto* state=graph->getStateSet();
    std::vector<osg::ref_ptr<osg::Object>> release;
    {Resource::CacheMaintenanceBudget b(128,8,std::chrono::seconds(1));shared->pruneBudgeted(release,b);}
    require(release.empty() && shared->getNumSharedStateSets()==1,"Actual live graph protects its shared material state");
    require(graph->getStateSet()==state,"Pruning cannot rewrite the live graph's material");
    for(unsigned i=0;i<12;++i)
    {
        osg::ref_ptr<osg::Group> unused=new osg::Group;
        unused->getOrCreateStateSet()->setDataVariance(osg::Object::STATIC);
        osg::ref_ptr<osg::Material> material=new osg::Material;
        material->setDiffuse(osg::Material::FRONT_AND_BACK,osg::Vec4(float(i)/12,0,0,1));
        unused->getStateSet()->setAttribute(material);
        shared->share(unused);
    }
    {Resource::CacheMaintenanceBudget b(1,1,std::chrono::seconds(1));shared->pruneBudgeted(release,b);}
    release.clear();
    shared->prune(); // Ordinary path removes the bounded cursor's old targets.
    {Resource::CacheMaintenanceBudget b(128,8,std::chrono::seconds(1));shared->pruneBudgeted(release,b);}
    require(shared->getNumSharedStateSets()==1 && graph->getStateSet()==state,
        "Interleaved ordinary pruning safely resets bounded cursors and protects the live material");
    release.clear();
    graph=nullptr;
    {Resource::CacheMaintenanceBudget b(128,8,std::chrono::seconds(1));shared->pruneBudgeted(release,b);}
    require(!release.empty(),"Unused shared-state owner actually detaches");
    release.clear(); // final owner release outside the shared list mutex
    {Resource::CacheMaintenanceBudget b(128,8,std::chrono::seconds(1));shared->pruneBudgeted(release,b);}
    release.clear();
    require(shared->getNumSharedStateSets()==0 && shared->getNumSharedTextures()==0,"Dependent unused material/texture owners retire safely");
    std::cout<<"PASS visual cache handoff: "<<checks<<" assertions; actual one-thread publication, invalidation, failure, bounds, external graphs and shared state\n"
             <<"LIMITATIONS: native policy/cache gate; full SceneManager/ResourceSystem engine compilation and actual clone/loading route engagement remain required. No RAM/FPS savings claim.\n";
    return 0;
}
catch(const std::exception& error){std::cerr<<"FAIL visual cache handoff: "<<error.what()<<'\n';return 1;}
