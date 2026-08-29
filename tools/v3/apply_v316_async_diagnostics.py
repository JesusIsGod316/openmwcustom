import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} async-diagnostics match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.16 async diagnostics patched {rel} ({count} match(es))")


replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''#include <chrono>
#include <cstdlib>
#include <filesystem>''',
    '''#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>''',
)

replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''        CsvWriter(const char* environmentVariable, std::string header)
            : mEnvironmentVariable(environmentVariable)
            , mHeader(std::move(header))
        {
        }

        bool enabled()''',
    '''        CsvWriter(const char* environmentVariable, std::string header)
            : mEnvironmentVariable(environmentVariable)
            , mHeader(std::move(header))
        {
        }

        ~CsvWriter()
        {
            if (!mAttempted.load(std::memory_order_acquire))
                return;
            {
                std::lock_guard<std::mutex> lock(mQueueMutex);
                mStopping = true;
            }
            mQueueCondition.notify_one();
            if (mWriterThread.joinable())
                mWriterThread.join();
        }

        bool enabled()''',
)

replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mStream.is_open())
                return;

            std::lock_guard<std::mutex> lock(mMutex);
            mStream << line << '\\n';
            if (++mLinesSinceFlush >= 60)
            {
                mStream.flush();
                mLinesSinceFlush = 0;
            }
        }

    private:''',
    '''        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mEnabled.load(std::memory_order_acquire))
                return;

            {
                std::lock_guard<std::mutex> lock(mQueueMutex);
                // Diagnostics must never become a gameplay hitch source. If disk
                // I/O cannot keep up, drop diagnostic rows instead of blocking
                // the producer thread or allowing unbounded memory growth.
                if (mQueue.size() >= sMaxQueuedLines)
                {
                    ++mDroppedLines;
                    return;
                }
                mQueue.push_back(line);
            }
            mQueueCondition.notify_one();
        }

    private:''',
)

replace_exact(
    "components/debug/v3diagnostics.hpp",
    '''            std::lock_guard<std::mutex> lock(mMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                mPath = raw;
                mStream.open(std::filesystem::u8path(mPath), std::ios::out | std::ios::trunc);
                if (mStream.is_open())
                    mStream << mHeader << '\\n';
            }

            mAttempted.store(true, std::memory_order_release);
        }

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        std::ofstream mStream;
        std::mutex mMutex;
        std::atomic<bool> mAttempted{ false };
        unsigned mLinesSinceFlush = 0;
    };''',
    '''            std::lock_guard<std::mutex> lock(mInitMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                mPath = raw;
                mStream.open(std::filesystem::u8path(mPath), std::ios::out | std::ios::trunc);
                if (mStream.is_open())
                {
                    mStream << mHeader << '\\n';
                    mEnabled.store(true, std::memory_order_release);
                    mWriterThread = std::thread([this] { writerLoop(); });
                }
            }

            mAttempted.store(true, std::memory_order_release);
        }

        void writerLoop()
        {
            std::deque<std::string> local;
            unsigned linesSinceFlush = 0;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(mQueueMutex);
                    mQueueCondition.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                    if (mQueue.empty() && mStopping)
                        break;
                    local.swap(mQueue);
                }

                while (!local.empty())
                {
                    mStream << local.front() << '\\n';
                    local.pop_front();
                    if (++linesSinceFlush >= 240)
                    {
                        mStream.flush();
                        linesSinceFlush = 0;
                    }
                }
            }

            if (mDroppedLines > 0)
                mStream << "# v3_async_diagnostics_dropped_lines=" << mDroppedLines << '\\n';
            mStream.flush();
            mEnabled.store(false, std::memory_order_release);
        }

        static constexpr std::size_t sMaxQueuedLines = 4096;

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        std::ofstream mStream;
        std::mutex mInitMutex;
        std::mutex mQueueMutex;
        std::condition_variable mQueueCondition;
        std::deque<std::string> mQueue;
        std::thread mWriterThread;
        std::atomic<bool> mAttempted{ false };
        std::atomic<bool> mEnabled{ false };
        bool mStopping = false;
        std::size_t mDroppedLines = 0;
    };''',
)

print("V3.16 asynchronous diagnostic CSV transport applied")
