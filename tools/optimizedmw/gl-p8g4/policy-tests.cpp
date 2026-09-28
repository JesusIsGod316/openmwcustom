#include <components/sceneutil/groundcoverlod2policy.hpp>
#include <components/debug/v3hitchtelemetry.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <fstream>
namespace P=SceneUtil::GroundcoverPolicy;
namespace L=SceneUtil::GroundcoverLod2;
static unsigned checks=0;
static void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
static void env(const char* name,const std::string& value){
#ifdef _WIN32
    _putenv_s(name,value.c_str());
#else
    setenv(name,value.c_str(),1);
#endif
}
int main(int argc,char** argv)
try {
    require(argc==2,"test output directory required");
    const std::filesystem::path out(argv[1]);std::filesystem::create_directories(out);
    const auto file=out/"frames.csv";
    std::filesystem::remove(file);
    env("OPENMW_P8G4_CLEAN_CAPTURE","1");env("OPENMW_V3_FRAME_FILE",file.string());
    env("OPENMW_V3_HITCH_FILE",(out/"hitches.csv").string());
    Debug::DeferredCapture::Buffer<unsigned> buffer;buffer.prepare(3);
    for(unsigned i=0;i<8;++i)buffer.push(i);
    require(buffer.size()==3 && buffer.dropped()==5,"bounded buffer lost overflow accounting");
    require(buffer.begin()[0]==0 && buffer.begin()[2]==2,"overflow changed retained prefix");
    buffer.prepare(20);require(buffer.capacity()==3,"buffer must not reallocate after prepare");
    Debug::V3HitchTelemetry::State capture;capture.prepare();capture.beginFrame(100);
    capture.recordStage(0,1.0);capture.beginFrame(101);
    require(!std::filesystem::exists(file),"clean capture wrote a file during gameplay");
    capture.finish();require(std::filesystem::exists(file),"finish did not write frames");
    std::ifstream status(file.string()+".capture-status.txt");
    const std::string text((std::istreambuf_iterator<char>(status)),{});
    require(text.find("dropped=0")!=std::string::npos && text.find("normal_finish=1")!=std::string::npos,"completion status incorrect");
    std::ifstream data(file);std::string line;unsigned rows=0;while(std::getline(data,line))++rows;
    require(rows==3,"first and final frame identities not retained");
    P::Options options;options.nearDistance=3000;options.farDistance=10000;
    auto shortView=L::effectiveOptions(options,2000);
    require(shortView.minimumDensity==1.f,"short view must not erode near protection");
    auto normal=L::effectiveOptions(options,9000);
    require(normal.farDistance==8100.f && normal.nearDistance==3000.f,"fade alignment incorrect");
    require(L::radiusBucket(-1.f)==L::RadiusBuckets,"invalid radius not protected");
    for(float radius=.01f;radius<3000.f;radius*=1.07f){
        auto bucket=L::radiusBucket(radius);
        require(bucket==L::RadiusBuckets || L::radiusForBucket(bucket)>=radius,"radius bucket underestimates plant");
    }
    std::vector<P::Instance> instances(4096);
    for(unsigned i=0;i<instances.size();++i){instances[i].identity=i;instances[i].rank=P::rank(i);}
    P::sortRanks(instances);
    require(L::tierCount(instances,0)==instances.size(),"full tier omitted a rank");
    for(unsigned t=1;t<4;++t)require(L::tierCount(instances,t)<L::tierCount(instances,t-1),"tiers must reduce real instance counts");
    for(unsigned d=0;d<=16000;d+=31){
        const float density=P::density(static_cast<float>(d),.003f,normal);
        unsigned desired=L::desiredTier(density);
        require(d>3000 || desired==0,"near density reduced");
        for(unsigned previous=0;previous<4;++previous){
            const auto tier=L::hystereticTier(previous,desired,density);
            require(tier<=desired,"hysteresis reduced more detail than safe target");
            auto count=L::tierCount(instances,tier);
            if(count<instances.size())require(instances[count].rank>density+P::FadeWidth,"CPU omitted visible shader rank");
        }
    }
    require(L::desiredTier(.80f)==1,"new early tier does not engage");
    require(L::desiredTier(.62f)==2,"middle tier does not engage");
    require(L::desiredTier(.45f)==3,"far tier does not engage");
    std::cout<<checks<<" P8G4 policy/capture checks passed\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
