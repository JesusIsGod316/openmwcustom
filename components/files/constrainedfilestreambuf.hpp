#ifndef OPENMW_CONSTRAINEDFILESTREAMBUF_H
#define OPENMW_CONSTRAINEDFILESTREAMBUF_H

#include <filesystem>
#include <memory>
#include <streambuf>

#include <components/platform/file.hpp>

namespace Files
{
    /// A file streambuf constrained to a specific region in the file, specified by the 'start' and 'length' parameters.
    class ConstrainedFileStreamBuf final : public std::streambuf
    {
    public:
        ConstrainedFileStreamBuf(const std::filesystem::path& fname, std::size_t start, std::size_t length);
        ~ConstrainedFileStreamBuf() override;

        int_type underflow() final;

        pos_type seekoff(off_type offset, std::ios_base::seekdir whence, std::ios_base::openmode mode) final;

        pos_type seekpos(pos_type pos, std::ios_base::openmode mode) final;

    private:
        void ensureCandidate();
        void ensureRefillBuffer();
        std::size_t nativeTell();
        void nativeSeek(std::size_t position);
        void checkpointConsumption();
        void clearWindow();
        pos_type seekTo(std::size_t position);
        std::size_t mOrigin;
        std::size_t mSize = 0;
        Platform::File::ScopedHandle mFile;
        char mBuffer[8192]{ 0 };
        bool mLargeRefillRequested = false;
        bool mResidentSeekRequested = false;
        bool mCandidateAttempted = false;
        struct CandidateState;
        struct CandidateDeleter
        {
            void operator()(CandidateState* value) const;
        };
        std::unique_ptr<CandidateState, CandidateDeleter> mCandidate;
        struct TraceState;
        // Identity/counters exist only in a requested diagnostic capture.
        std::unique_ptr<TraceState> mTrace;
    };
}

#endif
