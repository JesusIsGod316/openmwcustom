#ifndef OPENMW_DEBUG_DEFERREDCAPTURE_H
#define OPENMW_DEBUG_DEFERREDCAPTURE_H

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <type_traits>

namespace Debug::DeferredCapture
{
    inline bool enabled()
    {
        static const bool value = [] {
            const char* setting = std::getenv("OPENMW_P8G4_CLEAN_CAPTURE");
            return setting && *setting == '1';
        }();
        return value;
    }

    // Single producer, startup allocation, no formatting, locks, allocation or I/O
    // in push(). Full buffers keep their prefix and expose EVERY dropped record.
    template <class Record> class Buffer
    {
        static_assert(std::is_trivially_copyable_v<Record>);
    public:
        void prepare(std::size_t capacity) noexcept
        {
            if (mPrepared) return;
            mPrepared = true;
            mCapacity = capacity;
            try { mRecords = std::make_unique<Record[]>(capacity); }
            catch (const std::bad_alloc&) { mAllocationFailed = true; return; }
            // Value initialization alone can use lazy zero pages. Touch each page
            // before the benchmark starts, not as the route fills the buffer.
            volatile unsigned char* bytes = reinterpret_cast<unsigned char*>(mRecords.get());
            for (std::size_t i = 0; i < capacity * sizeof(Record); i += 4096) bytes[i] = 0;
        }
        void push(const Record& record) noexcept
        {
            if (!mRecords || mSize == mCapacity) { ++mDropped; return; }
            mRecords[mSize++] = record;
        }
        const Record* begin() const noexcept { return mRecords.get(); }
        const Record* end() const noexcept { return mRecords ? mRecords.get() + mSize : nullptr; }
        std::size_t size() const noexcept { return mSize; }
        std::size_t capacity() const noexcept { return mCapacity; }
        std::size_t dropped() const noexcept { return mDropped; }
        bool allocationFailed() const noexcept { return mAllocationFailed; }
    private:
        std::unique_ptr<Record[]> mRecords;
        std::size_t mSize = 0, mCapacity = 0, mDropped = 0;
        bool mPrepared = false, mAllocationFailed = false;
    };
}
#endif
