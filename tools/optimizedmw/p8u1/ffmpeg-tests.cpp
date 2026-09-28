#include <apps/openmw/mwsound/headcache.hpp>
#include <apps/openmw/mwsound/ffmpegdecoder.hpp>
#include <apps/openmw/mwsound/warmqueue.hpp>
#include <chrono>
#include <thread>
#include <components/vfs/archive.hpp>
#include <components/vfs/file.hpp>
#include <components/vfs/manager.hpp>
#include <atomic>
#include <iostream>
#include <sstream>
#include <stdexcept>
void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
struct MemoryFile : VFS::File
{
    std::string data;std::atomic_int opens{0};
    explicit MemoryFile(std::string d):data(std::move(d)){}
    Files::IStreamPtr open() override {++opens;return std::make_unique<std::istringstream>(data);}
    std::filesystem::file_time_type getLastModified()const override{return {};}
    std::string getStem()const override{return "test";}
};
struct MemoryArchive : VFS::Archive
{
    VFS::FileMap files;
    void listResources(VFS::FileMap& out) override {out=files;}
    bool contains(VFS::Path::NormalizedView file) const override{return files.contains(file);}
    std::string getDescription()const override{return "native-test-memory";}
};

std::string wav()
{
    std::string out;
    auto le=[&](std::uint32_t v,int n){for(int i=0;i<n;++i)out.push_back(static_cast<char>(v>>(8*i)));};
    out="RIFF"; le(36+8000,4); out+="WAVEfmt ";le(16,4);le(1,2);le(1,2);
    le(8000,4);le(16000,4);le(2,2);le(16,2);out+="data";le(8000,4);
    for(unsigned i=0;i<4000;++i)le(i*137,2);
    return out;
}
int main()
try
{
    constexpr VFS::Path::NormalizedView path("sound/test.wav");
    MemoryFile file(wav());
    VFS::Manager vfs;
    auto archive=std::make_unique<MemoryArchive>();archive->files.emplace(path,&file);
    vfs.addArchive(std::move(archive));vfs.buildIndex();
    auto decode=[&](MWSound::HeadCache* cache,bool record) {
        MWSound::FFmpegDecoder actual(&vfs,cache,record,false);
        MWSound::SoundDecoder& decoder=actual; // overrides are intentionally private
        decoder.open(path);std::vector<char> pcm;decoder.readAll(pcm);return pcm;
    };
    auto reference=decode(nullptr,false);require(reference.size()==8000,"unexpected PCM extent");
    MWSound::HeadCache cache(vfs,4*1024*1024);
    {
        MWSound::WarmQueue queue([&](const MWSound::WarmQueue::Item&) {
            MWSound::FFmpegDecoder actual(&vfs,&cache,true,false);
            static_cast<MWSound::SoundDecoder&>(actual).open(path);
            return cache.contains(path);
        });
        queue.enqueue({std::string(path.value()),false});
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
        while(queue.stats().processed==0 && std::chrono::steady_clock::now()<deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        require(queue.stats().processed==1 && queue.stats().failed==0 && cache.contains(path),
            "real FFmpeg warm worker did not populate cache");
    }
    require(decode(&cache,true)==reference,"head replay changed decoded PCM");
    require(cache.warmWholeFile(path),"whole-file preparation failed");
    const int before=file.opens;
    require(decode(&cache,false)==reference,"cached buffered decode changed PCM");
    require(file.opens==before,"full-file cached decode reopened storage");
    MWSound::HeadCache tiny(vfs,1);
    require(decode(&tiny,true)==reference && tiny.cachedBytes()==0,"cache budget miss broke playback");
    std::cout<<"PASS: actual FFmpeg warm/open/decode, byte-identical PCM, whole-file no-I/O replay and tiny-budget fallback\n";
}
catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
