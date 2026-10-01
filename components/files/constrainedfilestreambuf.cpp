#include "constrainedfilestreambuf.hpp"
#include <components/debug/hitchtrace.hpp>
#include "conversion.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>

namespace Files
{
    namespace File = Platform::File;

    struct ConstrainedFileStreamBuf::TraceState
    {
        std::string physicalPath;
        std::uint64_t openSpan = 0;
        std::uint64_t readCalls = 0;
        std::uint64_t returnedBytes = 0;
        std::uint64_t readNs = 0;
        std::uint64_t seekCalls = 0;
    };

    ConstrainedFileStreamBuf::ConstrainedFileStreamBuf(
        const std::filesystem::path& fname, std::size_t start, std::size_t length)
        : mOrigin(start)
    {
        const auto physicalPath = Debug::HitchTrace::requested() ? Files::pathToUnicodeString(fname) : std::string{};
        Debug::HitchTrace::Scope open("file.open_size", Debug::HitchTrace::CpuTiming::Scoped, {}, physicalPath);
        open.retain();
        open.setMetric(0, "region_origin", start);
        open.setMetric(1, "region_size", length);
        open.setMetric(2, "opened", 0);
        mFile = File::open(fname);
        mSize = length != std::numeric_limits<std::size_t>::max() ? length : File::size(mFile) - start;
        open.setMetric(1, "region_size", mSize);
        open.setMetric(2, "opened", 1);
        if (open.id())
        {
            mTrace = std::make_unique<TraceState>();
            mTrace->physicalPath = physicalPath;
            mTrace->openSpan = open.id();
        }

        if (start != 0)
            File::seek(mFile, start);

        setg(nullptr, nullptr, nullptr);
    }

    ConstrainedFileStreamBuf::~ConstrainedFileStreamBuf()
    {
        if (!mTrace) return;
        // These logical stream totals include calls below the per-span detail
        // threshold. They are not physical disk I/O or cache-hit assertions.
        Debug::HitchTrace::Scope totals("file.stream_totals", Debug::HitchTrace::CpuTiming::Root, {}, mTrace->physicalPath);
        totals.retain();
        totals.setLink(mTrace->openSpan);
        totals.setMetric(0, "read_calls", mTrace->readCalls);
        totals.setMetric(1, "returned_bytes", mTrace->returnedBytes);
        totals.setMetric(2, "read_call_ns", mTrace->readNs);
        totals.setMetric(3, "seek_calls", mTrace->seekCalls);
    }

    std::streambuf::int_type ConstrainedFileStreamBuf::underflow()
    {
        if (gptr() == egptr())
        {
            Debug::HitchTrace::Scope read("file.read", Debug::HitchTrace::CpuTiming::Scoped, {},
                mTrace ? std::string_view(mTrace->physicalPath) : std::string_view{});
            const std::size_t position = File::tell(mFile);
            const std::size_t toRead = std::min((mOrigin + mSize) - position, sizeof(mBuffer));
            read.setLink(mTrace ? mTrace->openSpan : 0);
            read.setMetric(0, "file_offset", position);
            read.setMetric(1, "requested_bytes", toRead);
            read.setMetric(2, "returned_bytes", 0);
            read.setMetric(3, "read_completed", 0);
            // Read in the next chunk of data, and set the read pointers on success
            // Failure will throw exception.
            const auto begin = mTrace ? Debug::HitchTrace::Clock::now() : Debug::HitchTrace::Clock::time_point{};
            if (mTrace) ++mTrace->readCalls;
            std::size_t got;
            try
            {
                got = File::read(mFile, mBuffer, toRead);
            }
            catch (...)
            {
                read.retain();
                if (mTrace)
                    mTrace->readNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Debug::HitchTrace::Clock::now() - begin).count();
                throw;
            }
            if (mTrace)
            {
                mTrace->returnedBytes += got;
                mTrace->readNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Debug::HitchTrace::Clock::now() - begin).count();
            }
            read.setMetric(2, "returned_bytes", got);
            read.setMetric(3, "read_completed", 1);
            setg(mBuffer, mBuffer, mBuffer + got);
        }
        if (gptr() == egptr())
            return traits_type::eof();

        return traits_type::to_int_type(*gptr());
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekoff(
        off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();

        // new file position, relative to mOrigin
        size_t newPos;
        switch (whence)
        {
            case std::ios_base::beg:
                newPos = offset;
                break;
            case std::ios_base::cur:
                newPos = (File::tell(mFile) - mOrigin - (egptr() - gptr())) + offset;
                break;
            case std::ios_base::end:
                newPos = mSize + offset;
                break;
            default:
                return traits_type::eof();
        }

        if (newPos > mSize)
            return traits_type::eof();

        File::seek(mFile, mOrigin + newPos);
        if (mTrace) ++mTrace->seekCalls;

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(nullptr, nullptr, nullptr);

        return newPos;
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in))
            return traits_type::eof();

        if (static_cast<std::size_t>(pos) > mSize)
            return traits_type::eof();

        File::seek(mFile, mOrigin + pos);
        if (mTrace) ++mTrace->seekCalls;

        // Clear read pointers so underflow() gets called on the next read attempt.
        setg(nullptr, nullptr, nullptr);
        return pos;
    }
}
