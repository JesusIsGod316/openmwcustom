#ifndef OPENMW_RESOURCE_DERIVEDASSETCACHE_H
#define OPENMW_RESOURCE_DERIVEDASSETCACHE_H

#include "assetpolicy.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace Resource::DerivedAssets
{
    using Bytes = std::vector<std::uint8_t>;

    // SHA-256 over the actual winning source snapshot, not VFS timestamps or
    // a process-local index generation. This helper performs no source I/O.
    class Sha256
    {
    public:
        void append(std::span<const std::uint8_t> bytes)
        {
            mBytes += bytes.size();
            for (const auto byte : bytes)
            {
                mBlock[mUsed++] = byte;
                if (mUsed == 64) { transform(); mUsed = 0; }
            }
        }
        std::string finish()
        {
            const std::uint64_t bits = mBytes * 8;
            mBlock[mUsed++] = 0x80;
            if (mUsed > 56)
            {
                std::fill(mBlock.begin() + mUsed, mBlock.end(), 0);
                transform(); mUsed = 0;
            }
            std::fill(mBlock.begin() + mUsed, mBlock.begin() + 56, 0);
            for (unsigned i = 0; i < 8; ++i) mBlock[63 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
            transform();
            constexpr char hex[] = "0123456789abcdef";
            std::string output; output.reserve(64);
            for (const auto word : mState)
                for (int shift = 28; shift >= 0; shift -= 4) output.push_back(hex[(word >> shift) & 15]);
            return output;
        }
    private:
        static std::uint32_t rotate(std::uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
        void transform()
        {
            static constexpr std::array<std::uint32_t, 64> k{
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
            std::array<std::uint32_t, 64> w{};
            for (unsigned i = 0; i < 16; ++i)
                w[i] = (std::uint32_t(mBlock[i*4]) << 24) | (std::uint32_t(mBlock[i*4+1]) << 16)
                    | (std::uint32_t(mBlock[i*4+2]) << 8) | mBlock[i*4+3];
            for (unsigned i = 16; i < 64; ++i)
            {
                const auto a = rotate(w[i-15],7) ^ rotate(w[i-15],18) ^ (w[i-15] >> 3);
                const auto b = rotate(w[i-2],17) ^ rotate(w[i-2],19) ^ (w[i-2] >> 10);
                w[i] = w[i-16] + a + w[i-7] + b;
            }
            auto [a,b,c,d,e,f,g,h] = mState;
            for (unsigned i = 0; i < 64; ++i)
            {
                const auto s1 = rotate(e,6) ^ rotate(e,11) ^ rotate(e,25);
                const auto t1 = h + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
                const auto s0 = rotate(a,2) ^ rotate(a,13) ^ rotate(a,22);
                const auto t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
                h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
            }
            mState[0]+=a; mState[1]+=b; mState[2]+=c; mState[3]+=d;
            mState[4]+=e; mState[5]+=f; mState[6]+=g; mState[7]+=h;
        }
        std::array<std::uint32_t,8> mState{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        std::array<std::uint8_t,64> mBlock{};
        std::uint64_t mBytes=0;
        std::size_t mUsed=0;
    };
    inline std::string digest(std::span<const std::uint8_t> bytes) { Sha256 h; h.append(bytes); return h.finish(); }
    inline std::string digest(std::string_view text)
    { return digest(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())); }
    inline std::string key(std::string_view sourceDigest, std::string_view recipe)
    {
        // Length framing prevents ambiguous source/recipe concatenation.
        Sha256 h;
        const auto append = [&](std::string_view value) {
            std::array<std::uint8_t,8> size{};
            for (unsigned i=0;i<8;++i) size[i]=static_cast<std::uint8_t>(value.size() >> (i*8));
            h.append(size); h.append(std::span(reinterpret_cast<const std::uint8_t*>(value.data()),value.size()));
        };
        append("OptimizedMW.DerivedAsset.v1"); append(sourceDigest); append(recipe);
        return h.finish();
    }
    inline bool validKey(std::string_view keyValue)
    { return keyValue.size()==64 && std::all_of(keyValue.begin(),keyValue.end(),[](char c){
        return (c>='0'&&c<='9') || (c>='a'&&c<='f'); }); }

    struct Counters
    {
        inline static std::atomic_uint64_t hits{0}, misses{0}, writes{0}, corrupt{0},
            declines{0}, evictions{0}, readBytes{0}, writtenBytes{0};
    };
    struct Config
    {
        std::filesystem::path directory;
        std::uint64_t maximumBytes=AssetPolicy::DiskLimit;
        std::uint64_t freeReserve=AssetPolicy::DiskReserve;
        std::size_t maximumEntries=4096;
        std::size_t maximumEntryBytes=64*1024*1024;
    };

    // Directory metadata uses fixed keys, never thousands of retained full
    // paths. Callers include this conservative scratch ceiling in admission.
    inline constexpr std::uint64_t MetadataScratchBytes = 4 * AssetPolicy::MiB;

    // Worker-only artifact service. It never opens VFS, chooses quality, or
    // publishes live OSG/GL resources. A miss always leaves original behavior.
    class Cache
    {
    public:
        explicit Cache(Config config) : mConfig(std::move(config))
        { if (mConfig.directory.native().size() > 1024) mConfig.directory.clear(); }
        std::optional<Bytes> read(std::string_view keyValue,std::size_t maximumBytes=64*1024*1024)
        {
            std::lock_guard lock(mMutex);
            return readLocked(keyValue,maximumBytes);
        }
    private:
        std::optional<Bytes> readLocked(std::string_view keyValue,std::size_t maximumBytes)
        {
            if (mConfig.directory.empty() || !validKey(keyValue)) { ++Counters::misses; return {}; }
            const auto path=file(keyValue);
            const auto rejectCorrupt=[&]() -> std::optional<Bytes> {
                ++Counters::corrupt;
                std::error_code ec;
                // Remove only the exact artifact filename in our own namespace.
                // Symlink artifacts are never followed or removed here.
                if(!std::filesystem::is_symlink(path,ec)) std::filesystem::remove(path,ec);
                return {};
            };
            try
            {
                std::error_code ec;
                if (std::filesystem::is_symlink(mConfig.directory,ec) || std::filesystem::is_symlink(path,ec))
                { ++Counters::declines; return {}; }
                const auto size=std::filesystem::file_size(path,ec);
                if (ec) { ++Counters::misses; return {}; }
                if (size<HeaderBytes || size-HeaderBytes>mConfig.maximumEntryBytes)
                    return rejectCorrupt();
                if(size-HeaderBytes>maximumBytes) { ++Counters::declines; return {}; }
                std::ifstream input(path,std::ios::binary);
                std::array<char,HeaderBytes> header{};
                input.read(header.data(),header.size());
                std::uint64_t payloadSize=0;
                for(unsigned i=0;i<8;++i) payloadSize |= std::uint64_t(static_cast<unsigned char>(header[136+i]))<<(i*8);
                if (!input || std::memcmp(header.data(),Magic,8) || std::string_view(header.data()+8,64)!=keyValue
                    || payloadSize!=size-HeaderBytes)
                { input.close(); return rejectCorrupt(); }
                Bytes bytes(static_cast<std::size_t>(payloadSize));
                input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                const bool complete=bool(input); input.close();
                if (!complete || digest(bytes)!=std::string_view(header.data()+72,64))
                    return rejectCorrupt();
                // Access-time touch supports bounded LRU eviction; it is not identity.
                std::filesystem::last_write_time(path,std::filesystem::file_time_type::clock::now(),ec);
                ++Counters::hits; Counters::readBytes+=bytes.size(); return bytes;
            }
            catch (...) { ++Counters::declines; return {}; }
        }
    public:
        bool write(std::string_view keyValue,std::span<const std::uint8_t> bytes)
        {
            std::lock_guard lock(mMutex);
            if (mConfig.directory.empty() || !validKey(keyValue) || bytes.size()>mConfig.maximumEntryBytes
                || bytes.size()>mConfig.maximumBytes || HeaderBytes>mConfig.maximumBytes-bytes.size())
            { ++Counters::declines; return false; }
            std::filesystem::path temporary;
            try
            {
                std::error_code ec;
                std::filesystem::create_directories(mConfig.directory,ec);
                if (ec || std::filesystem::is_symlink(mConfig.directory,ec)) { ++Counters::declines; return false; }
                const auto destination=file(keyValue);
                if(std::filesystem::exists(destination,ec))
                {
                    const auto existing=readLocked(keyValue,mConfig.maximumEntryBytes);
                    if(existing) return existing->size()==bytes.size() && digest(*existing)==digest(bytes);
                    if(std::filesystem::exists(destination,ec)) { ++Counters::declines; return false; }
                }
                const auto space=std::filesystem::space(mConfig.directory,ec);
                const auto wanted=bytes.size()+HeaderBytes;
                if (ec || space.available<wanted || space.available-wanted<mConfig.freeReserve || !makeRoom(wanted,keyValue))
                { ++Counters::declines; return false; }
                std::array<char,HeaderBytes> header{};
                std::memcpy(header.data(),Magic,8); std::memcpy(header.data()+8,keyValue.data(),64);
                const auto checksum=digest(bytes); std::memcpy(header.data()+72,checksum.data(),64);
                for(unsigned i=0;i<8;++i) header[136+i]=static_cast<char>(bytes.size()>>(i*8));
                static std::atomic_uint64_t sequence{0};
                const auto time=std::chrono::steady_clock::now().time_since_epoch().count();
                temporary=mConfig.directory/(std::string(keyValue)+".tmp-"+std::to_string(time)+"-"+std::to_string(++sequence));
                std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
                output.write(header.data(),header.size());
                output.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
                output.flush(); const bool complete=bool(output); output.close();
                if (!complete) { std::filesystem::remove(temporary,ec); ++Counters::declines; return false; }
                // The process mutex serializes writers; never replace an existing
                // immutable artifact. Another process's valid result wins too.
                if (std::filesystem::exists(destination,ec))
                {
                    std::filesystem::remove(temporary,ec);
                    const auto existing=readLocked(keyValue,mConfig.maximumEntryBytes);
                    return existing && existing->size()==bytes.size() && digest(*existing)==digest(bytes);
                }
                std::filesystem::rename(temporary,destination,ec);
                if (ec) { std::filesystem::remove(temporary,ec); ++Counters::declines; return false; }
                ++Counters::writes; Counters::writtenBytes+=bytes.size(); return true;
            }
            catch (...)
            {
                std::error_code ec; if (!temporary.empty()) std::filesystem::remove(temporary,ec);
                ++Counters::declines; return false;
            }
        }
        const Config& config() const { return mConfig; }
    private:
        static constexpr std::size_t HeaderBytes=144;
        inline static constexpr char Magic[8]={'O','M','W','D','A','C','0','1'};
        std::filesystem::path file(std::string_view value) const
        { return mConfig.directory/(std::string(value)+".omwcache"); }
        bool makeRoom(std::uint64_t wanted,std::string_view protectedKey)
        {
            struct Entry { std::array<char,64> key; std::uint64_t bytes; std::filesystem::file_time_type time; };
            static_assert(sizeof(Entry)*8192 < MetadataScratchBytes/2);
            std::vector<Entry> entries; std::uint64_t total=0, temporaryBytes=0;
            entries.reserve(8192);
            std::size_t visited=0;
            std::error_code ec;
            for (std::filesystem::directory_iterator it(mConfig.directory,ec),end;!ec&&it!=end;it.increment(ec))
            {
                if(++visited>8192) return false;
                const auto& item=*it;
                if (!item.is_regular_file(ec) || item.is_symlink(ec)) continue;
                const auto filename=item.path().filename().string();
                if(filename.size()>69 && validKey(filename.substr(0,64)) && filename.substr(64,5)==".tmp-"
                    && std::all_of(filename.begin()+69,filename.end(),[](char c){return (c>='0'&&c<='9')||c=='-';}))
                {
                    const auto modified=item.last_write_time(ec); if(ec) return false;
                    if(std::filesystem::file_time_type::clock::now()-modified>std::chrono::hours(24))
                    { if(!std::filesystem::remove(item.path(),ec)||ec) return false; continue; }
                    const auto bytes=item.file_size(ec);
                    if(ec || bytes>128*AssetPolicy::MiB || temporaryBytes>128*AssetPolicy::MiB-bytes) return false;
                    temporaryBytes+=bytes;
                    continue;
                }
                if (filename.size()!=73 || filename.substr(64)!=".omwcache" || !validKey(filename.substr(0,64))) continue;
                const auto bytes=item.file_size(ec); if(ec || bytes>mConfig.maximumBytes || total>UINT64_MAX-bytes) return false;
                Entry entry{};
                std::copy_n(filename.data(),64,entry.key.data());
                entry.bytes=bytes; entry.time=item.last_write_time(ec);
                total+=bytes; entries.push_back(entry);
                if(ec) return false;
            }
            if(ec) return false;
            if(temporaryBytes>mConfig.maximumBytes || wanted>mConfig.maximumBytes-temporaryBytes) return false;
            total+=temporaryBytes;
            std::sort(entries.begin(),entries.end(),[](const Entry& a,const Entry& b){return a.time<b.time;});
            auto count=entries.size();
            for(const auto& entry:entries)
            {
                if(total<=mConfig.maximumBytes-wanted && count<mConfig.maximumEntries) return true;
                const std::string_view entryKey(entry.key.data(),entry.key.size());
                if(entryKey==protectedKey) continue;
                // Only this service's validated filename namespace is reclaimed.
                if(!std::filesystem::remove(file(entryKey),ec) || ec) return false;
                total-=entry.bytes; --count; ++Counters::evictions;
            }
            return total<=mConfig.maximumBytes-wanted && count<mConfig.maximumEntries;
        }
        Config mConfig;
        std::mutex mMutex;
    };
    inline Cache& cache()
    {
        static Cache instance([] {
            Config config;
            if(const char* directory=std::getenv("OPENMW_P9_DERIVED_CACHE_DIR"))
                config.directory=std::filesystem::u8path(directory);
            return config;
        }());
        return instance;
    }
}
#endif
