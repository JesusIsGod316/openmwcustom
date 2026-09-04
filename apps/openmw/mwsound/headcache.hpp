#ifndef GAME_SOUND_HEADCACHE_H
#define GAME_SOUND_HEADCACHE_H

#include <cstddef>
#include <functional>
#include <ios>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <components/files/istreamptr.hpp>
#include <components/vfs/pathutil.hpp>

namespace VFS
{
    class Manager;
}

namespace MWSound
{
    struct HeadBuffer
    {
        VFS::Path::Normalized mName;
        std::vector<char> mHead;
        std::vector<char> mSuffix;
        std::streamoff mSuffixStart;
        std::streamoff mFileSize;

        HeadBuffer(VFS::Path::Normalized&& name, std::vector<char>&& head, std::vector<char>&& suffix,
            std::streamoff suffixStart, std::streamoff fileSize)
            : mName(std::move(name))
            , mHead(std::move(head))
            , mSuffix(std::move(suffix))
            , mSuffixStart(suffixStart)
            , mFileSize(fileSize)
        {
        }
    };

    Files::IStreamPtr makeHeadStream(std::shared_ptr<const HeadBuffer>&& buffer, const VFS::Manager& vfs);
    Files::IStreamPtr makeRecordingStream(Files::IStreamPtr&& impl);

    class HeadCache
    {
    public:
        explicit HeadCache(const VFS::Manager& vfs, std::size_t maxBytes);

        std::shared_ptr<const HeadBuffer> lookup(VFS::Path::NormalizedView name);
        void insert(VFS::Path::NormalizedView name, const std::istream& stream);

    private:
        using LruIt = std::list<std::shared_ptr<const HeadBuffer>>::iterator;

        void insert(VFS::Path::NormalizedView name, std::vector<char>&& head, std::vector<char>&& suffix,
            std::streamoff suffixStart, std::streamoff fileSize);

        const VFS::Manager& mVfs;
        const std::size_t mMaxBytes;
        std::mutex mMutex;
        std::list<std::shared_ptr<const HeadBuffer>> mLru;
        std::unordered_map<VFS::Path::Normalized, LruIt, VFS::Path::Hash, std::equal_to<>> mEntries;
        std::size_t mBytes = 0;
    };
}

#endif
