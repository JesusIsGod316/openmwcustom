#include <components/resource/derivedassetcache.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void require(bool condition,const char* message)
    { if(!condition) throw std::runtime_error(message); }
}
int main()
{
    using namespace Resource::DerivedAssets;
    try
    {
        require(digest(std::string_view{})=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855","SHA256 empty");
        require(digest(std::string_view("abc"))=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 abc");
        require(digest(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))
            =="248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1","SHA256 multi-block");
        std::string million(1000000,'a');
        require(digest(std::string_view(million))=="cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0","SHA256 million");
        require(key("ab","c")!=key("a","bc"),"recipe framing");
        require(key(digest("source"),"role=diffuse")!=key(digest("source"),"role=height"),"role identity");
        require(key(digest("source1"),"same")!=key(digest("source2"),"same"),"content identity");

        const auto path=std::filesystem::temp_directory_path()/(
            "optimizedmw-derived-cache-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Config config; config.directory=path; config.maximumBytes=512; config.maximumEntries=2;
        config.maximumEntryBytes=64; config.freeReserve=0;
        Cache store(config);
        const auto k=key(digest("asset"),"recipe-v1");
        Bytes payload{0,1,2,255,11};
        require(!store.read(k),"initial miss");
        require(store.write(k,payload),"atomic write");
        require(store.read(k)==payload,"round trip");
        require(!store.read(k,4),"bounded read");
        require(store.read(k)==payload,"bounded refusal preserves valid artifact");
        require(!store.write("../../unsafe",payload),"unsafe key");
        require(!store.write(key("large","v1"),Bytes(65)),"oversize entry");
        const auto artifact=path/(k+".omwcache");
        {
            std::fstream file(artifact,std::ios::in|std::ios::out|std::ios::binary);
            file.seekp(144); file.put('\x40');
        }
        require(!store.read(k),"checksum corruption");
        require(store.write(k,payload) && store.read(k)==payload,"corrupt artifact recooked");
        require(!store.write(k,Bytes{9}),"immutable recipe collision refused");
        // Simulate source winner or recipe change at the same logical path.
        const auto changed=key(digest("new-winner"),"recipe-v1");
        require(!store.read(changed),"winner change misses");
        require(store.write(changed,payload),"winner artifact");
        require(store.read(changed)==payload,"winner read");
        {
            std::ofstream unrelated(path/"user-file.txt"); unrelated<<"preserve";
        }
        const auto another=key(digest("third"),"recipe-v2");
        require(store.write(another,payload),"quota eviction write");
        const auto beforeDuplicate= Counters::evictions.load();
        require(store.write(another,payload) && Counters::evictions.load()==beforeDuplicate,"duplicate does not evict");
        require(std::filesystem::exists(path/"user-file.txt"),"preserve unrelated namespace");
        std::vector<std::thread> workers;
        std::atomic_bool valid{true};
        for(unsigned i=0;i<8;++i) workers.emplace_back([&] {
            for(unsigned n=0;n<10;++n)
                if(!store.write(another,payload) || store.read(another)!=payload) valid=false;
        });
        for(auto& worker:workers) worker.join();
        require(valid,"serialized duplicate request");
        Config denied=config; denied.directory=path/"denied"; denied.freeReserve=UINT64_MAX;
        Cache noSpace(denied); require(!noSpace.write(k,payload),"free-space reserve");
        Cache disabled(Config{}); require(!disabled.write(k,payload) && !disabled.read(k),"disabled original fallback");
        Config longPath=config; longPath.directory=std::filesystem::path(std::string(1025,'x'));
        Cache excessivePath(longPath);
        require(excessivePath.config().directory.empty() && !excessivePath.write(k,payload),
            "bounded metadata refuses excessive directory path before filesystem allocation");
        // Delete only this fixture's individually enumerated files. No recursive
        // shell deletion and no walk outside its freshly created test directory.
        for(const auto& entry:std::filesystem::directory_iterator(path))
            if(entry.is_regular_file()) std::filesystem::remove(entry.path());
        std::filesystem::remove(path/"denied"); std::filesystem::remove(path);
        std::cout<<"derived asset cache: SHA256 vectors, recipes, corruption, bounds, quotas, concurrency and fallback passed\n";
        return 0;
    }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
