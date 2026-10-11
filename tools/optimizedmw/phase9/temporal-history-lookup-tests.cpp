#include <components/sceneutil/temporalhistorylookup.hpp>
#include <vector>
#include <iostream>
#include <stdexcept>
namespace HL=SceneUtil::TemporalHistoryLookup;
struct Surface { std::uint64_t instance; unsigned material, pose; };
void require(bool value,const char* text) {if(!value) throw std::runtime_error(text);}
std::uint64_t comparisons = 0;
std::unique_ptr<HL::Scratch> contextScratch;
std::vector<unsigned> run(const std::vector<Surface>& previous,const std::vector<Surface>& current,bool indexed)
{
    HL::enabled=indexed;
    HL::Read read(previous,[](const auto& surface){return surface.instance;},contextScratch);
    std::vector<unsigned> results;
    for (const auto& surface:current)
    {
        const auto found=read.find(surface.instance,[&](const auto& value){
            ++comparisons;
            return value.instance==surface.instance && value.material==surface.material && value.pose==surface.pose;
        });
        results.push_back(static_cast<unsigned>(found-previous.begin()));
    }
    return results;
}
int main() try
{
    std::vector<Surface> previous{{7,1,2},{3,1,2},{7,2,2},{7,1,2},{0,0,0},{3,1,9}};
    std::vector<Surface> current{{7,1,2},{7,2,2},{3,1,9},{0,0,0},{11,1,2},{7,2,9}};
    const auto initialControl=run(previous,current,false);
    require(!contextScratch,"disabled control allocated index scratch");
    require(initialControl==run(previous,current,true),"identity/material/pose match or original tie order changed");
    const auto* owner=contextScratch.get();
    require(run(previous,current,true)==std::vector<unsigned>({0,2,5,4,6,6}),"duplicate instance first match changed");
    require(run({},current,false)==run({},current,true),"empty history mismatch");
    previous.clear(); current.clear();
    for (unsigned i=0;i<HL::MaximumSurfaces;++i) previous.push_back({i+1,1,1});
    current=previous;
    const auto before=comparisons;
    const auto control=run(previous,current,false);
    const auto linear=comparisons-before;
    const auto candidate=run(previous,current,true);
    const auto indexed=comparisons-before-linear;
    require(control==candidate && indexed==HL::MaximumSurfaces && linear==2098176,"actual bounded lookup did not remove the expected search work");
    require(contextScratch.get()==owner,"draw context scratch was reallocated on a subsequent frame");
    previous.push_back({9000,1,1}); current.push_back({9000,1,1});
    const auto lookups=HL::indexedLookups.load();
    require(run(previous,current,false)==run(previous,current,true),"over-cap fallback result changed");
    require(HL::indexedLookups.load()==lookups,"over-cap data silently used an incomplete index");
    std::cout<<"TEMPORAL_HISTORY_LOOKUP_PASS first_compatible=1 duplicates=1 missing=1 zero_identity=1 material_pose=1 over_capacity_fallback=1 linear_comparisons="<<linear<<" indexed_comparisons="<<indexed<<"\n";
    return 0;
} catch (const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
