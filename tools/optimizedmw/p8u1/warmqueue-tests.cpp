#include <apps/openmw/mwsound/warmqueue.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
template<class F> void waitFor(F f){for(int i=0;i<5000;++i){if(f())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}throw std::runtime_error("worker timeout");}
int main()
try
{
    std::promise<void> release;auto gate=release.get_future().share();
    std::atomic_int calls{0},whole{0};std::atomic_bool entered=false;
    MWSound::WarmQueue q([&](const MWSound::WarmQueue::Item& item){
        ++calls;if(item.wholeFile)++whole;
        if(item.path=="sound/a.wav" && !item.wholeFile){entered=true;gate.wait();}
        if(item.path=="bad")throw std::runtime_error("expected decoder failure");
        return true;
    },{},3,100);
    require(q.enqueue({"sound/a.wav",false})==MWSound::WarmQueue::Result::Accepted,"enqueue failed");
    waitFor([&]{return entered.load();});
    require(q.enqueue({"sound/a.wav",true,true})==MWSound::WarmQueue::Result::Duplicate,"in-flight dedup failed");
    q.enqueue({"sound/b.wav",true});q.enqueue({"bad",false});
    require(q.enqueue({"overflow",false})==MWSound::WarmQueue::Result::Full,"queue exceeded bound");
    release.set_value();waitFor([&]{return q.stats().pending==0;});
    const auto stats=q.stats();
    require(calls==4 && whole==2,"in-flight whole-file upgrade lost or duplicated");
    require(stats.failed==1 && stats.processed==4,"failure accounting mismatch");
    require(stats.peakPending==3 && stats.pathBytes==0,"bounded accounting leaked");
    require(q.enqueue({"",false})==MWSound::WarmQueue::Result::Invalid,"invalid path accepted");
    std::cout<<"PASS: bounded worker, owned paths, in-flight dedup/upgrade, overflow, failure isolation, drain and join\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
