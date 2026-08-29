import os
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()

path = ROOT / "components/debug/v3diagnostics.hpp"
text = path.read_text(encoding="utf-8")

# This layer is intentionally applied after V3.16 async diagnostics. Fail closed
# if the expected per-CsvWriter worker implementation is not present.
for marker in (
    "sMaxQueuedLines = 4096",
    "mWriterThread",
    "void writerLoop()",
    "v3_async_diagnostics_dropped_lines=",
    "std::try_to_lock",
):
    if marker not in text:
        raise RuntimeError(f"V3.17 diagnostics hub expected V3.16 marker: {marker}")

old_includes = '''#include <deque>\n#include <filesystem>\n#include <fstream>'''
new_includes = '''#include <deque>\n#include <filesystem>\n#include <fstream>\n#include <memory>'''
if text.count(old_includes) != 1:
    raise RuntimeError("V3.17 diagnostics hub could not add memory include")
text = text.replace(old_includes, new_includes, 1)

old_thread_include = '''#include <thread>\n'''
new_thread_include = '''#include <thread>\n#include <vector>\n'''
if text.count(old_thread_include) != 1:
    raise RuntimeError("V3.17 diagnostics hub could not add vector include")
text = text.replace(old_thread_include, new_thread_include, 1)

start_marker = "    class CsvWriter\n"
end_marker = "    inline std::string csvQuote"
start = text.find(start_marker)
end = text.find(end_marker, start)
if start < 0 or end < 0 or text.find(start_marker, start + 1) >= 0:
    raise RuntimeError("V3.17 diagnostics hub could not uniquely locate CsvWriter block")
old_block = text[start:end]
for marker in ("~CsvWriter()", "mQueueCondition", "linesSinceFlush >= 240", "mDroppedLines"):
    if marker not in old_block:
        raise RuntimeError(f"V3.17 diagnostics hub found unexpected V3.16 CsvWriter block: missing {marker}")

new_block = r'''    struct DiagnosticChannel
    {
        DiagnosticChannel(std::string path, std::string header)
            : mPath(std::move(path))
            , mHeader(std::move(header))
        {
        }

        std::string mPath;
        std::string mHeader;
        std::ofstream mStream;
        std::atomic<std::size_t> mDroppedLines{ 0 };
        bool mOpenAttempted = false;
    };

    // V3.17: all V3 CsvWriter instances share one bounded, nonblocking producer
    // queue and one disk writer. V3.16 removed direct producer-thread I/O but
    // still created one writer thread per enabled CSV and periodically flushed
    // each stream. The shared hub eliminates that thread/flush periodicity.
    class DiagnosticWriterHub
    {
    public:
        using ChannelHandle = std::shared_ptr<DiagnosticChannel>;

        static DiagnosticWriterHub& instance()
        {
            static DiagnosticWriterHub hub;
            return hub;
        }

        DiagnosticWriterHub(const DiagnosticWriterHub&) = delete;
        DiagnosticWriterHub& operator=(const DiagnosticWriterHub&) = delete;

        ChannelHandle registerChannel(std::string path, std::string header)
        {
            auto channel = std::make_shared<DiagnosticChannel>(std::move(path), std::move(header));
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (mStopping)
                    return {};
                mChannels.push_back(channel);
                // Queue an open/header operation so even an enabled stream with
                // no data rows is materialized without producer-thread file I/O.
                mQueue.push_back({ channel, {}, true });
                if (!mWriterThread.joinable())
                    mWriterThread = std::thread([this] { writerLoop(); });
            }
            mCondition.notify_one();
            return channel;
        }

        void enqueue(const ChannelHandle& channel, const std::string& line)
        {
            if (!channel)
                return;

            // Instrumentation must never wait behind its own writer. A dropped
            // row is less harmful than contaminating the frametime being measured.
            std::unique_lock<std::mutex> lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (mStopping || mQueue.size() >= sMaxQueuedItems)
            {
                channel->mDroppedLines.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            mQueue.push_back({ channel, line, false });
            lock.unlock();
            mCondition.notify_one();
        }

        ~DiagnosticWriterHub()
        {
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mStopping = true;
            }
            mCondition.notify_one();
            if (mWriterThread.joinable())
                mWriterThread.join();
        }

    private:
        struct QueueItem
        {
            ChannelHandle mChannel;
            std::string mLine;
            bool mOpenOnly = false;
        };

        DiagnosticWriterHub() = default;

        static void openChannel(DiagnosticChannel& channel)
        {
            if (channel.mOpenAttempted)
                return;
            channel.mOpenAttempted = true;
            channel.mStream.open(std::filesystem::u8path(channel.mPath), std::ios::out | std::ios::trunc);
            if (channel.mStream.is_open())
                channel.mStream << channel.mHeader << '\n';
        }

        void writerLoop()
        {
            std::deque<QueueItem> local;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lock(mMutex);
                    mCondition.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                    if (mQueue.empty() && mStopping)
                        break;
                    local.swap(mQueue);
                }

                while (!local.empty())
                {
                    QueueItem item = std::move(local.front());
                    local.pop_front();
                    if (!item.mChannel)
                        continue;
                    openChannel(*item.mChannel);
                    if (!item.mOpenOnly && item.mChannel->mStream.is_open())
                        item.mChannel->mStream << item.mLine << '\n';
                }
            }

            // No gameplay-cadence flushes. Drain first, then record losses and
            // flush each enabled stream exactly once during orderly shutdown.
            for (const ChannelHandle& channel : mChannels)
            {
                if (!channel)
                    continue;
                openChannel(*channel);
                if (!channel->mStream.is_open())
                    continue;
                const std::size_t dropped = channel->mDroppedLines.load(std::memory_order_relaxed);
                if (dropped > 0)
                    channel->mStream << "# v3_async_diagnostics_dropped_lines=" << dropped << '\n';
                channel->mStream.flush();
            }
        }

        static constexpr std::size_t sMaxQueuedItems = 16384;

        std::mutex mMutex;
        std::condition_variable mCondition;
        std::deque<QueueItem> mQueue;
        std::vector<ChannelHandle> mChannels;
        std::thread mWriterThread;
        bool mStopping = false;
    };

    class CsvWriter
    {
    public:
        CsvWriter(const char* environmentVariable, std::string header)
            : mEnvironmentVariable(environmentVariable)
            , mHeader(std::move(header))
        {
        }

        bool enabled()
        {
            ensureOpen();
            return mEnabled.load(std::memory_order_acquire);
        }

        void writeLine(const std::string& line)
        {
            ensureOpen();
            if (!mEnabled.load(std::memory_order_acquire))
                return;
            DiagnosticWriterHub::instance().enqueue(mChannel, line);
        }

    private:
        void ensureOpen()
        {
            if (mAttempted.load(std::memory_order_acquire))
                return;

            std::lock_guard<std::mutex> lock(mInitMutex);
            if (mAttempted.load(std::memory_order_relaxed))
                return;

            const char* raw = std::getenv(mEnvironmentVariable.c_str());
            if (raw && pathEnabled(raw))
            {
                mPath = raw;
                mChannel = DiagnosticWriterHub::instance().registerChannel(mPath, mHeader);
                mEnabled.store(static_cast<bool>(mChannel), std::memory_order_release);
            }

            mAttempted.store(true, std::memory_order_release);
        }

        std::string mEnvironmentVariable;
        std::string mHeader;
        std::string mPath;
        DiagnosticWriterHub::ChannelHandle mChannel;
        std::mutex mInitMutex;
        std::atomic<bool> mAttempted{ false };
        std::atomic<bool> mEnabled{ false };
    };

'''

text = text[:start] + new_block + text[end:]

# Fail closed against retaining the per-writer worker/periodic-flush path.
for forbidden in (
    "linesSinceFlush >= 240",
    "static constexpr std::size_t sMaxQueuedLines = 4096",
):
    if forbidden in text:
        raise RuntimeError(f"V3.17 diagnostics hub failed to remove V3.16 path: {forbidden}")
for required in (
    "class DiagnosticWriterHub",
    "sMaxQueuedItems = 16384",
    "mQueue.push_back({ channel, {}, true })",
    "std::try_to_lock",
    "No gameplay-cadence flushes",
):
    if required not in text:
        raise RuntimeError(f"V3.17 diagnostics hub missing generated marker: {required}")

path.write_text(text, encoding="utf-8", newline="\n")
print("V3.17 consolidated diagnostic writer hub applied")
