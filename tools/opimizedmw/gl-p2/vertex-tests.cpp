#include <components/sceneutil/pagingvertexcache.hpp>
#include <components/sceneutil/pagingwork.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    unsigned checks = 0;
    void check(bool ok, const char* name)
    { if (!ok) throw std::runtime_error(name); ++checks; }
    auto triangles(const std::vector<unsigned>& indices)
    {
        std::vector<std::array<unsigned,3>> result;
        for (std::size_t i=0;i<indices.size();i+=3)
            if (indices[i]!=indices[i+1] && indices[i+1]!=indices[i+2] && indices[i]!=indices[i+2])
                result.push_back({indices[i],indices[i+1],indices[i+2]});
        std::sort(result.begin(),result.end()); return result;
    }
}
int main()
{
    using namespace SceneUtil;
    using namespace PagingVertexCache;
    try
    {
        std::mt19937 random(937);
        for (unsigned count : {0u,1u,2u,3u,7u,32u,65u,500u})
        {
            std::vector<unsigned> input;
            for (unsigned i=0;i<count;++i)
                for (unsigned j=0;j<3;++j) input.push_back(random()%97);
            std::vector<unsigned> output{999}, second;
            Statistics stats;
            check(optimize(input,97,output,[]{return false;},stats)==Result::Complete,"valid mesh completes");
            check(triangles(input)==triangles(output),"triangles/winding/duplicates preserved");
            Statistics repeat;
            check(optimize(input,97,second,[]{return false;},repeat)==Result::Complete && output==second,"deterministic");
        }
        std::vector<unsigned> invalid{1,2,100}, result{71}; Statistics stats;
        check(optimize(invalid,20,result,[]{return false;},stats)==Result::Invalid && result==std::vector<unsigned>{71},"index range transactional");
        invalid={1,2};
        check(optimize(invalid,20,result,[]{return false;},stats)==Result::Invalid,"incomplete triangle rejected");
        std::vector<unsigned> disconnected;
        for(unsigned i=0;i<3000;++i) disconnected.insert(disconnected.end(),{3*i,3*i+1,3*i+2});
        stats={};
        check(optimize(disconnected,9000,result,[]{return false;},stats)==Result::Complete,"disconnected mesh");
        check(result==disconnected,"restart equal-score order matches first-index scan");
        check(stats.restartSelections==3000,"disconnected restart count");
        check(stats.heapComparisons<3000*100,"restart selection not quadratic whole-mesh scans");
        for (unsigned cancelAt : {1u,10u,100u,1000u,10000u,25000u})
        {
            unsigned calls=0; result={71}; stats={};
            const auto status=optimize(disconnected,9000,result,[&]{return ++calls>=cancelAt;},stats);
            check(status==Result::Cancelled && result==std::vector<unsigned>{71},"cancel leaves output unchanged");
        }
        std::atomic<bool> cancelled{true}, healthy{false};
        check(!PagingWorkScope::active(),"no ambient scope");
        {
            PagingWorkScope outer(&cancelled);
            check(PagingWorkScope::cancelled(),"outer cancelled");
            { PagingWorkScope inner(&healthy); check(!PagingWorkScope::cancelled(),"nested isolation"); }
            check(PagingWorkScope::cancelled(),"nested restore");
            bool threadIsolated=false;
            std::thread worker([&]{threadIsolated=!PagingWorkScope::active();});worker.join();
            check(threadIsolated,"worker-local token");
            bool threw=false;try{PagingWorkScope::checkpoint();}catch(const PagingWorkCancelled&){threw=true;}
            check(threw,"cooperative exception");
        }
        check(!PagingWorkScope::active(),"scope unwound");
        std::cout<<checks<<" P2 vertex/cancellation checks passed\n";
    }
    catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return EXIT_FAILURE;}
}
