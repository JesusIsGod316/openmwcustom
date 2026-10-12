#include "constrainedfilestreambuf.hpp"
#include "constrainedreadpolicy.hpp"
#include "conversion.hpp"
#include <components/debug/hitchtrace.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace Files
{
    namespace File = Platform::File;
    namespace Policy = ConstrainedReadPolicy;

    struct ConstrainedFileStreamBuf::TraceState
    {
        std::string physicalPath;
        std::uint64_t openSpan = 0;
        std::uint64_t readCalls = 0, returnedBytes = 0, readNs = 0, seekCalls = 0;
        std::uint64_t nativeTellCalls = 0, nativeSeekCalls = 0, zeroReads = 0, residentSeekHits = 0;
        std::uint64_t capHits = 0, allocationFailures = 0;
        std::uint64_t netConsumed = 0, netPutback = 0, discardedUnread = 0;
        char* checkpoint = nullptr;
    };

    struct ConstrainedFileStreamBuf::CandidateState
    {
        Policy::Reservation stateReservation;
        Policy::Reservation bufferReservation;
        std::unique_ptr<char[]> buffer;
        std::size_t nativePosition;
        std::size_t windowStart = 0, windowBytes = 0;
        bool windowValid = false, bufferAttempted = false, positionKnown = true;
        CandidateState(Policy::Reservation reservation, std::size_t origin)
            : stateReservation(std::move(reservation)), nativePosition(origin)
        {
        }
    };

    void ConstrainedFileStreamBuf::CandidateDeleter::operator()(CandidateState* value) const
    {
        if (!value) return;
        // Keep the state lease alive through operator delete. Releasing a
        // reservation from a member destructor would occur just before that
        // allocation is freed and could transiently undercount concurrent use.
        auto reservation = std::move(value->stateReservation);
        delete value;
    }

    namespace
    {
        // The Windows File wrapper's size/tell contract is still limited to its
        // low DWORD. This candidate does not expand that platform contract.
#ifdef _WIN32
        constexpr std::size_t MaximumPosition = std::numeric_limits<std::uint32_t>::max();
#else
        constexpr std::size_t MaximumPosition = static_cast<std::size_t>(std::numeric_limits<std::streamoff>::max());
#endif

        bool addOffset(std::size_t base, std::streamoff offset, std::size_t limit, std::size_t& result)
        {
            if (base > limit) return false;
            if (offset < 0)
            {
                const auto magnitude = static_cast<std::uint64_t>(-(offset + 1)) + 1;
                if (magnitude > base) return false;
                result = base - static_cast<std::size_t>(magnitude);
            }
            else
            {
                const auto magnitude = static_cast<std::uint64_t>(offset);
                if (magnitude > limit - base) return false;
                result = base + static_cast<std::size_t>(magnitude);
            }
            return true;
        }
    }

    ConstrainedFileStreamBuf::ConstrainedFileStreamBuf(
        const std::filesystem::path& fname, std::size_t start, std::size_t length)
        : mOrigin(start)
    {
        const auto& options = Policy::Options::startup();
        mLargeRefillRequested = options.refillBytes == Policy::CandidateBytes;
        mResidentSeekRequested = options.residentSeek;
        const auto physicalPath = Debug::HitchTrace::requested() ? Files::pathToUnicodeString(fname) : std::string{};
        Debug::HitchTrace::Scope open("file.open_size", Debug::HitchTrace::CpuTiming::Scoped, {}, physicalPath);
        open.retain();
        open.setMetric(0, "region_origin", start);
        open.setMetric(1, "region_size", length);
        open.setMetric(2, "opened", 0);
        mFile = File::open(fname);
        if (start > MaximumPosition)
            throw std::runtime_error("Constrained file region exceeds the supported file position range.");
        if (length == std::numeric_limits<std::size_t>::max())
        {
            const auto physicalSize = File::size(mFile);
            if (physicalSize > MaximumPosition || start > physicalSize)
                throw std::runtime_error("Constrained file origin is outside the supported file.");
            mSize = physicalSize - start;
        }
        else
        {
            if (length > MaximumPosition - start)
                throw std::runtime_error("Constrained file region exceeds the supported file position range.");
            mSize = length;
            if (mLargeRefillRequested || mResidentSeekRequested)
            {
                const auto physicalSize = File::size(mFile);
                if (physicalSize > MaximumPosition || start > physicalSize || length > physicalSize - start)
                    throw std::runtime_error("Constrained file region is outside the supported file.");
            }
        }
        open.setMetric(1, "region_size", mSize);
        open.setMetric(2, "opened", 1);
        if (open.id())
        {
            mTrace = std::make_unique<TraceState>();
            mTrace->physicalPath = physicalPath;
            mTrace->openSpan = open.id();
        }
        if (start != 0)
        {
            // Preserve the constructor's existing initial native seek. The
            // stream_totals seek_calls historically excluded this operation.
            if (mTrace) ++mTrace->nativeSeekCalls;
            File::seek(mFile, start);
        }
        setg(nullptr, nullptr, nullptr);
    }

    void ConstrainedFileStreamBuf::ensureCandidate()
    {
        if (mCandidateAttempted || !(mLargeRefillRequested || mResidentSeekRequested)) return;
        mCandidateAttempted = true;
        Policy::Reservation reservation(sizeof(CandidateState) + Policy::StateOverheadBytes, false);
        if (!reservation)
        {
            if (mTrace) ++mTrace->capHits;
            return;
        }
        try
        {
#ifdef P9_FILE_STREAM_TEST_HOOKS
            if (Policy::TestHooks::take(Policy::TestHooks::allocationFailures)) throw std::bad_alloc();
#endif
            mCandidate.reset(new CandidateState(std::move(reservation), mOrigin));
        }
        catch (const std::bad_alloc&)
        {
            if (mTrace) ++mTrace->allocationFailures;
            // The local reservation rolls back if allocation fails. A stream
            // that has fallen back never repeatedly retries an allocation.
        }
    }

    void ConstrainedFileStreamBuf::ensureRefillBuffer()
    {
        if (!mCandidate || !mLargeRefillRequested || mCandidate->bufferAttempted) return;
        mCandidate->bufferAttempted = true;
        Policy::Reservation reservation(Policy::CandidateBytes + Policy::BufferOverheadBytes, true);
        if (!reservation)
        {
            if (mTrace) ++mTrace->capHits;
        }
        else
        {
            try
            {
#ifdef P9_FILE_STREAM_TEST_HOOKS
                if (Policy::TestHooks::take(Policy::TestHooks::allocationFailures)) throw std::bad_alloc();
#endif
                auto buffer = std::make_unique_for_overwrite<char[]>(Policy::CandidateBytes);
                mCandidate->buffer = std::move(buffer);
                mCandidate->bufferReservation = std::move(reservation);
            }
            catch (const std::bad_alloc&)
            {
                if (mTrace) ++mTrace->allocationFailures;
            }
        }
        // I1 fallback has the exact inline-buffer/native-position path. I2 is
        // independent and can retain its resident window with the 8 KiB buffer.
        if (!mCandidate->buffer && !mResidentSeekRequested) mCandidate.reset();
    }

    std::size_t ConstrainedFileStreamBuf::nativeTell()
    {
        if (mTrace) ++mTrace->nativeTellCalls;
#ifdef P9_FILE_STREAM_TEST_HOOKS
        if (Policy::TestHooks::take(Policy::TestHooks::tellFailures))
            throw std::runtime_error("fixture native tell failure");
#endif
        return File::tell(mFile);
    }

    void ConstrainedFileStreamBuf::nativeSeek(std::size_t position)
    {
        if (mTrace) ++mTrace->nativeSeekCalls;
#ifdef P9_FILE_STREAM_TEST_HOOKS
        if (Policy::TestHooks::take(Policy::TestHooks::seekFailures))
            throw std::runtime_error("fixture native seek failure");
#endif
        try
        {
            File::seek(mFile, position);
#ifdef P9_FILE_STREAM_TEST_HOOKS
            if (Policy::TestHooks::take(Policy::TestHooks::seekFailuresAfterAdvance))
                throw std::runtime_error("fixture seek failure after advancing handle");
#endif
        }
        catch (...)
        {
            if (mCandidate && mResidentSeekRequested)
            {
                try
                {
                    const auto actual = nativeTell();
                    if (!mCandidate->positionKnown || actual != mCandidate->nativePosition) clearWindow();
                    mCandidate->nativePosition = actual;
                    mCandidate->positionKnown = true;
                }
                catch (...) { clearWindow(); mCandidate->positionKnown = false; }
            }
            throw;
        }
        if (mTrace) ++mTrace->seekCalls;
    }

    void ConstrainedFileStreamBuf::checkpointConsumption()
    {
        if (!mTrace || !mTrace->checkpoint || !gptr()) return;
        // std::streambuf services most get/putback operations directly. These
        // are NET pointer deltas between checkpoints, not every byte service.
        const auto delta = gptr() - mTrace->checkpoint;
        if (delta >= 0) mTrace->netConsumed += static_cast<std::uint64_t>(delta);
        else mTrace->netPutback += static_cast<std::uint64_t>(-delta);
        mTrace->checkpoint = gptr();
    }

    void ConstrainedFileStreamBuf::clearWindow()
    {
        checkpointConsumption();
        if (mTrace)
        {
            if (gptr()) mTrace->discardedUnread += static_cast<std::uint64_t>(egptr() - gptr());
            mTrace->checkpoint = nullptr;
        }
        setg(nullptr, nullptr, nullptr);
        if (mCandidate) mCandidate->windowValid = false;
    }

    ConstrainedFileStreamBuf::~ConstrainedFileStreamBuf()
    {
        checkpointConsumption();
        if (!mTrace) return;
        const auto unread = gptr() ? static_cast<std::uint64_t>(egptr() - gptr()) : 0;
        const auto owned = mCandidate ? mCandidate->stateReservation.bytes() + mCandidate->bufferReservation.bytes() : 0;
        const auto effectiveRefill = mCandidate && mCandidate->buffer ? Policy::CandidateBytes : Policy::ControlBytes;
        const bool effectiveResident = mCandidate && mResidentSeekRequested;
        const auto emit = [&](const char* label, const char* a, std::uint64_t av, const char* b, std::uint64_t bv,
                              const char* c, std::uint64_t cv, const char* d, std::uint64_t dv) {
            Debug::HitchTrace::Scope row(label, Debug::HitchTrace::CpuTiming::Root, {}, mTrace->physicalPath);
            row.retain();
            row.setLink(mTrace->openSpan);
            row.setMetric(0, a, av); row.setMetric(1, b, bv); row.setMetric(2, c, cv); row.setMetric(3, d, dv);
        };
        // Keep the original fields and interpretation: logical wrapper calls,
        // including failed/zero-byte reads, rather than physical disk service.
        emit("file.stream_totals", "read_calls", mTrace->readCalls, "returned_bytes", mTrace->returnedBytes,
            "read_call_ns", mTrace->readNs, "seek_calls", mTrace->seekCalls);
        emit("file.stream_policy", "requested_refill_bytes", mLargeRefillRequested ? Policy::CandidateBytes : Policy::ControlBytes,
            "effective_refill_bytes", effectiveRefill, "requested_resident_seek", mResidentSeekRequested,
            "effective_resident_seek", effectiveResident);
        emit("file.stream_buffer", "owned_candidate_bytes", owned, "buffer_cap_hits", mTrace->capHits,
            "buffer_allocation_failures", mTrace->allocationFailures, "budget_limit_bytes", Policy::Options::startup().budgetBytes);
        emit("file.stream_io", "native_tell_calls", mTrace->nativeTellCalls, "native_seek_calls", mTrace->nativeSeekCalls,
            "zero_byte_read_calls", mTrace->zeroReads, "resident_seek_hits", mTrace->residentSeekHits);
        emit("file.stream_consumption", "net_consumed_bytes", mTrace->netConsumed, "net_putback_bytes", mTrace->netPutback,
            "discarded_unread_bytes", mTrace->discardedUnread, "unread_at_close_bytes", unread);
        if (mLargeRefillRequested || mResidentSeekRequested)
        {
            setg(nullptr, nullptr, nullptr);
            mCandidate.reset();
            const auto snapshot = Policy::Budget::instance().snapshot();
            emit("file.stream_budget", "budget_in_use_bytes", snapshot.used, "budget_peak_bytes", snapshot.peak,
                "active_candidate_streams", snapshot.states, "active_candidate_buffers", snapshot.buffers);
        }
    }

    std::streambuf::int_type ConstrainedFileStreamBuf::underflow()
    {
        if (gptr() == egptr())
        {
            ensureCandidate();
            checkpointConsumption();
            Debug::HitchTrace::Scope read("file.read", Debug::HitchTrace::CpuTiming::Scoped, {},
                mTrace ? std::string_view(mTrace->physicalPath) : std::string_view{});
            const auto position = mCandidate && mResidentSeekRequested && mCandidate->positionKnown
                ? mCandidate->nativePosition : nativeTell();
            if (position < mOrigin || position > mOrigin + mSize)
                throw std::runtime_error("Constrained file position is outside its region.");
            if (position < mOrigin + mSize) ensureRefillBuffer();
            char* const buffer = mCandidate && mCandidate->buffer ? mCandidate->buffer.get() : mBuffer;
            const auto capacity = mCandidate && mCandidate->buffer ? Policy::CandidateBytes : Policy::ControlBytes;
            auto toRead = std::min(mOrigin + mSize - position, capacity);
            read.setLink(mTrace ? mTrace->openSpan : 0);
            read.setMetric(0, "file_offset", position);
            read.setMetric(1, "requested_bytes", toRead);
            read.setMetric(2, "returned_bytes", 0);
            read.setMetric(3, "read_completed", 0);
            const auto begin = mTrace ? Debug::HitchTrace::Clock::now() : Debug::HitchTrace::Clock::time_point{};
            if (mTrace) ++mTrace->readCalls;
            std::size_t got;
            try
            {
#ifdef P9_FILE_STREAM_TEST_HOOKS
                if (Policy::TestHooks::take(Policy::TestHooks::readFailures))
                    throw std::runtime_error("fixture native read failure");
                const auto limit = Policy::TestHooks::nextReadLimit.exchange(0);
                if (limit) toRead = std::min(toRead, limit);
                got = Policy::TestHooks::take(Policy::TestHooks::zeroReads) ? 0 : File::read(mFile, buffer, toRead);
                if (Policy::TestHooks::take(Policy::TestHooks::readFailuresAfterAdvance))
                    throw std::runtime_error("fixture read failure after advancing handle");
#else
                got = File::read(mFile, buffer, toRead);
#endif
            }
            catch (...)
            {
                read.retain();
                if (mTrace)
                    mTrace->readNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                        Debug::HitchTrace::Clock::now() - begin).count();
                if (mCandidate && mResidentSeekRequested)
                {
                    // A failing platform read is not assumed to leave the
                    // handle position unchanged. Preserve the original error
                    // while recovering position for a clear()/retry.
                    try
                    {
                        const auto actual = nativeTell();
                        clearWindow();
                        mCandidate->nativePosition = actual;
                        mCandidate->positionKnown = true;
                    }
                    catch (...) { clearWindow(); mCandidate->positionKnown = false; }
                }
                throw;
            }
            if (mTrace)
            {
                mTrace->returnedBytes += got;
                mTrace->zeroReads += got == 0;
                mTrace->readNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                    Debug::HitchTrace::Clock::now() - begin).count();
            }
            read.setMetric(2, "returned_bytes", got);
            read.setMetric(3, "read_completed", 1);
            // A successful zero-byte read has not overwritten the previous
            // exhausted window. I2 can retain that read-only putback/seek
            // window, including at EOF; clear()/retry still issues a read.
            if (got == 0 && mCandidate && mResidentSeekRequested && mCandidate->windowValid)
                return traits_type::eof();
            if (mCandidate)
            {
                mCandidate->nativePosition = position + got;
                mCandidate->positionKnown = true;
                mCandidate->windowStart = position - mOrigin;
                mCandidate->windowBytes = got;
                mCandidate->windowValid = true;
            }
            // A zero/short read remains retryable after the caller clears its
            // stream flags. Advance by the actual count, never the request.
            setg(buffer, buffer, buffer + got);
            if (mTrace) mTrace->checkpoint = gptr();
        }
        return gptr() == egptr() ? traits_type::eof() : traits_type::to_int_type(*gptr());
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekTo(std::size_t position)
    {
        if (mCandidate && mResidentSeekRequested)
        {
            if (mCandidate->windowValid && position >= mCandidate->windowStart
                && position - mCandidate->windowStart <= mCandidate->windowBytes)
            {
                checkpointConsumption();
                const auto displacement = position - mCandidate->windowStart;
                setg(eback(), eback() + displacement, eback() + mCandidate->windowBytes);
                if (mTrace) { ++mTrace->residentSeekHits; mTrace->checkpoint = gptr(); }
                return static_cast<off_type>(position);
            }
            // A tell before the first read does not create or invalidate a
            // window and does not need a redundant native seek.
            if (!mCandidate->windowValid && mCandidate->positionKnown
                && mOrigin + position == mCandidate->nativePosition)
                return static_cast<off_type>(position);
        }
        nativeSeek(mOrigin + position);
        clearWindow(); // only after success: a failing seek preserves the window
        if (mCandidate) { mCandidate->nativePosition = mOrigin + position; mCandidate->positionKnown = true; }
        return static_cast<off_type>(position);
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekoff(
        off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in)) return pos_type(off_type(-1));
        if (whence != std::ios_base::beg && whence != std::ios_base::cur && whence != std::ios_base::end)
            return pos_type(off_type(-1));
        ensureCandidate();
        std::size_t base = 0;
        if (whence == std::ios_base::cur)
        {
            if (mCandidate && mResidentSeekRequested && mCandidate->positionKnown)
                base = mCandidate->windowValid ? mCandidate->windowStart + static_cast<std::size_t>(gptr() - eback())
                                              : mCandidate->nativePosition - mOrigin;
            else
            {
                const auto native = nativeTell();
                const auto unread = gptr() ? static_cast<std::size_t>(egptr() - gptr()) : 0;
                if (native < mOrigin || unread > native - mOrigin) return pos_type(off_type(-1));
                base = native - mOrigin - unread;
            }
        }
        else if (whence == std::ios_base::end) base = mSize;
        std::size_t position;
        if (!addOffset(base, offset, mSize, position)) return pos_type(off_type(-1));
        return seekTo(position);
    }

    std::streambuf::pos_type ConstrainedFileStreamBuf::seekpos(pos_type pos, std::ios_base::openmode mode)
    {
        if ((mode & std::ios_base::out) || !(mode & std::ios_base::in)) return pos_type(off_type(-1));
        const auto offset = static_cast<off_type>(pos);
        std::size_t position;
        if (!addOffset(0, offset, mSize, position)) return pos_type(off_type(-1));
        ensureCandidate();
        return seekTo(position);
    }
}
