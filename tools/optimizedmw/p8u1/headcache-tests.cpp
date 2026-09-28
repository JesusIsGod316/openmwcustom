#include <apps/openmw/mwsound/headcache.hpp>
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
int main()
try
{
    constexpr VFS::Path::NormalizedView a("sound/a.wav"),b("sound/b.wav");
    MemoryFile af(std::string(60,'a')),bf(std::string(60,'b'));
    VFS::Manager vfs;
    auto archive=std::make_unique<MemoryArchive>();
    archive->files.emplace(a,&af);archive->files.emplace(b,&bf);
    vfs.addArchive(std::move(archive));vfs.buildIndex();
    MWSound::HeadCache cache(vfs,100);
    auto recording=MWSound::makeRecordingStream(vfs.get(a));char head[10]{};recording->read(head,10);
    cache.insert(a,*recording);
    auto old=cache.lookup(a);require(old && old->mHead.size()==10,"head recording failed");
    require(cache.warmWholeFile(a),"whole-file upgrade failed");
    require(old->mHead.size()==10,"active old head mutated during upgrade");
    require(cache.cachedBytes()==60,"upgrade byte charge wrong");
    const int before=af.opens;
    auto stream=MWSound::makeHeadStream(cache.lookup(a),vfs);char all[60]{};stream->read(all,60);
    require(stream->gcount()==60 && std::string(all,60)==af.data && af.opens==before,"warmed replay touched backing storage");
    require(!cache.warmWholeFile(b),"speculative warmer evicted playback data");
    require(cache.contains(a)&&!cache.contains(b)&&cache.cachedBytes()==60,"failed admission corrupted cache");
    // Existing demand recording is still allowed to replace an LRU cache entry.
    auto demand=MWSound::makeRecordingStream(vfs.get(b));demand->read(all,60);cache.insert(b,*demand);
    require(cache.contains(b)&&!cache.contains(a)&&cache.cachedBytes()==60,"demand LRU behavior changed");
    require(old->mHead.size()==10,"eviction invalidated existing stream owner");
    MWSound::HeadCache zero(vfs,0);require(!zero.warmWholeFile(a)&&zero.cachedBytes()==0,"zero budget admitted data");
    std::cout<<"PASS: actual VFS/head cache recording, whole-file upgrade, cache-only replay, non-evicting speculation, demand LRU, retained owners, zero budget\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
