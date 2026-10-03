#ifndef OPENMW_RESOURCE_NATIVETEXTURE_H
#define OPENMW_RESOURCE_NATIVETEXTURE_H

#include <osg/Image>
#include <osg/ref_ptr>
#include <components/vfs/pathutil.hpp>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace VFS { class Manager; }
namespace Resource
{
    enum class TextureRole : std::uint8_t { Diffuse, DiffuseSpecular, Normal, NormalHeight };
    enum class TexturePurpose : std::uint8_t { MainDraw, CompositeSource };
    enum class NativeTextureSkip : std::uint8_t
    {
        None, Disabled, Queue, HostAdmission, SourceSize, SourceRead, Stale, Format,
        IncompleteMips, Dimensions, Role, Backend, Encode, RetainedLimit, Cache, SourceProvider, Purpose, Count
    };
    struct NativeMip
    {
        std::uint32_t width = 0, height = 0, offset = 0, bytes = 0;
        bool operator==(const NativeMip&) const = default;
    };
    struct NativeMipPayload
    {
        // The charge is destroyed after the bytes and their clients.
        std::shared_ptr<void> retainedCharge;
        std::vector<std::uint8_t> bytes;
        std::vector<NativeMip> mips;
        std::string sourceDigest, recipeKey, path;
        unsigned format = 0;
        TextureRole role = TextureRole::Diffuse;
        TexturePurpose purpose = TexturePurpose::MainDraw;
        bool converted = false;
        std::size_t suffixBytes(unsigned first) const noexcept;
        unsigned coarseLevel() const noexcept;
    };
    struct NativeTextureStats
    {
        std::atomic_uint64_t requested{0}, eligible{0}, reusedDds{0}, converted{0}, ready{0},
            cacheHits{0}, cacheMisses{0}, cacheWrites{0}, nativeImages{0}, fullApplies{0}, fallback{0}, retainedBytes{0};
        std::array<std::atomic_uint64_t, static_cast<std::size_t>(NativeTextureSkip::Count)> skips{};
    };
    NativeTextureStats& nativeTextureStats();
    const char* nativeTextureSkipName(NativeTextureSkip);
    std::string nativeTextureKey(std::string_view path, TextureRole, TexturePurpose);

    // Strict 2D legacy S3TC parser. Complete mip table and exact block lengths
    // are required. Unsupported DX10/BC7/array/cube/volume data never converts.
    std::shared_ptr<NativeMipPayload> prepareNativeTexture(std::span<const std::uint8_t>,
        std::string_view path, TextureRole, TexturePurpose, NativeTextureSkip&);
    osg::ref_ptr<osg::Image> nativeTextureImage(std::shared_ptr<const NativeMipPayload>);

    struct NativeTextureEpoch { std::atomic_uint64_t value{1}; std::atomic_bool alive{true}; };
    class NativeTextureTicket
    {
    public:
        NativeTextureTicket(std::shared_ptr<NativeTextureEpoch>, std::uint64_t epoch,
            std::uint64_t vfsGeneration, std::string path, TextureRole, TexturePurpose);
        bool current() const noexcept;
        std::shared_ptr<const NativeMipPayload> payload() const;
        void publish(std::shared_ptr<const NativeMipPayload>);
        void decline(NativeTextureSkip);
        const std::string path;
        const TextureRole role;
        const TexturePurpose purpose;
        const std::uint64_t vfsGeneration;
        std::atomic_bool ordinaryAllocated{false};
        std::atomic_uint64_t lastDrawFrame{0}, firstCoarseDraw{0}, firstFineDraw{0};
        std::atomic<double> lastDrawTime{0};
    private:
        const std::shared_ptr<NativeTextureEpoch> mEpoch;
        const std::uint64_t mExpectedEpoch;
        mutable std::mutex mMutex;
        std::shared_ptr<const NativeMipPayload> mPayload;
    };

    // One bounded producer thread. Requests are shared by immutable role and
    // purpose, not by pointer or filename alone. Destruction joins before VFS
    // ownership is released; rebuild must likewise quiesce all readers.
    class NativeTexturePreparation
    {
    public:
        explicit NativeTexturePreparation(const VFS::Manager*);
        ~NativeTexturePreparation();
        std::shared_ptr<NativeTextureTicket> request(VFS::Path::NormalizedView, TextureRole, TexturePurpose);
        void clear();
    private:
        void run();
        const VFS::Manager* mVfs;
        std::shared_ptr<NativeTextureEpoch> mEpoch = std::make_shared<NativeTextureEpoch>();
        std::mutex mMutex;
        std::condition_variable mCondition;
        // Strong until consumed: a prefetch ticket must survive its worker
        // completion before the terrain pass is realized. Unused entries are
        // reclaimed before the bounded directory can grow.
        std::unordered_map<std::string, std::shared_ptr<NativeTextureTicket>> mTickets;
        std::deque<std::shared_ptr<NativeTextureTicket>> mQueue;
        bool mStopping = false;
        std::thread mWorker;
    };
}
#endif
