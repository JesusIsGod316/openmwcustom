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
    '''        bool enabled()
        {
            ensureOpen();
            return mStream.is_open();
        }''',
    '''        bool enabled()
        {
            ensureOpen();
            // Only the writer thread owns the stream after initialization. Avoid
            // reading std::ofstream state concurrently from gameplay threads.
            return mEnabled.load(std::memory_order_acquire);
        }''',
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

            // Never wait behind the diagnostic writer on a gameplay thread. A
            // lost diagnostic row is preferable to manufacturing a frametime
            // spike in the instrumentation used to measure frametime spikes.
            std::unique_lock<std::mutex> lock(mQueueMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (mQueue.size() >= sMaxQueuedLines)
            {
                mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            mQueue.push_back(line);
            lock.unlock();
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

            const std::size_t dropped = mDroppedLines.load(std::memory_order_relaxed);
            if (dropped > 0)
                mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\\n';
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
        std::atomic<std::size_t> mDroppedLines{ 0 };
        bool mStopping = false;
    };''',
)

print("V3.16 asynchronous diagnostic CSV transport applied")

# This is deliberately last in the V3.16 source stack. The audio-head and SFX
# predecode implementations are generated as new files, and plain `git diff`
# would otherwise omit untracked files from the authoritative source snapshot.
# Mark/validate those files before apply_lab_packaging_v316 captures the diff.
snapshot_qc = Path(__file__).with_name("apply_v316_generated_file_qc.py")
exec(
    compile(snapshot_qc.read_text(encoding="utf-8"), str(snapshot_qc), "exec"),
    {"__file__": str(snapshot_qc), "__name__": "__main__"},
)
