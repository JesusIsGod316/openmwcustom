#include <components/debug/diagnostictransport.hpp>

#include <barrier>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

using namespace Debug::V3Diagnostics;

namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    void boundedRing()
    {
        DiagnosticMpscQueue<std::string, 8> queue;
        for (unsigned round = 0; round < 32; ++round)
        {
            for (unsigned i = 0; i < 8; ++i)
            {
                auto row = std::to_string(round * 8 + i);
                require(queue.push(std::move(row)), "available slot was rejected");
            }
            std::string overflow = "overflow";
            require(!queue.push(std::move(overflow)), "full ring failed its hard bound");
            for (unsigned i = 0; i < 8; ++i)
            {
                std::string row;
                require(queue.pop(row) && row == std::to_string(round * 8 + i),
                    "wrap-around changed complete item order");
            }
            std::string empty;
            require(!queue.pending() && !queue.pop(empty), "drained ring still has a record");
        }
    }

    void byteBudgetAndPublication()
    {
        DiagnosticByteBudget<32> budget;
        require(budget.claim(24) && !budget.claim(9) && budget.pending() == 24,
            "byte budget did not reject and roll back overflow");
        require(budget.claim(8) && !budget.claim(1) && !budget.claim(33), "byte budget exceeded inclusive bound");
        budget.release(24);
        require(budget.claim(24) && budget.pending() == 32, "byte budget could not reuse released storage");
        budget.release(32);
        require(budget.pending() == 0, "byte budget did not drain");

        struct HeldItem
        {
            int value = 0;
            std::atomic<bool>* entered = nullptr;
            std::atomic<bool>* release = nullptr;
            HeldItem& operator=(HeldItem&& other) noexcept
            {
                if (other.entered)
                {
                    other.entered->store(true, std::memory_order_release);
                    while (!other.release->load(std::memory_order_acquire))
                        std::this_thread::yield();
                }
                value = other.value;
                return *this;
            }
        };
        DiagnosticMpscQueue<HeldItem, 8> queue;
        std::atomic<bool> entered{ false }, release{ false };
        std::thread first([&] {
            HeldItem row{ 1, &entered, &release };
            require(queue.push(std::move(row)), "held producer could not publish");
        });
        while (!entered.load(std::memory_order_acquire))
            std::this_thread::yield();
        HeldItem second{ 2 }, output;
        const bool accepted = queue.push(std::move(second));
        const bool unpublishedRead = queue.pop(output);
        release.store(true, std::memory_order_release);
        first.join();
        require(accepted && queue.pending() && !unpublishedRead, "consumer read an incomplete slot or blocked a second producer");
        require(queue.pop(output) && output.value == 1 && queue.pop(output) && output.value == 2,
            "out-of-order publication changed FIFO consumption");
    }

    void actualHub(const std::filesystem::path& directory)
    {
        constexpr unsigned Producers = 8, Rounds = 32, Rows = 128;
        DiagnosticWriterHub hub(true);
        const auto path = directory / "pressure.csv";
        auto channel = hub.registerChannel(path.string(), "producer,round,row,payload");
        require(static_cast<bool>(channel), "real channel registration failed");
        auto empty = hub.registerChannel((directory / "empty.csv").string(), "frame,value");
        auto rejected = hub.registerChannel((directory / "bounded.csv").string(), "value");
        std::barrier boundary(Producers + 1);
        std::vector<std::thread> producers;
        for (unsigned producer = 0; producer < Producers; ++producer)
            producers.emplace_back([&, producer] {
                for (unsigned round = 0; round < Rounds; ++round)
                {
                    boundary.arrive_and_wait();
                    for (unsigned row = 0; row < Rows; ++row)
                        hub.enqueue(channel, std::to_string(producer) + ',' + std::to_string(round) + ','
                            + std::to_string(row) + ",complete owned row");
                    boundary.arrive_and_wait();
                }
            });
        for (unsigned round = 0; round < Rounds; ++round)
        {
            boundary.arrive_and_wait();
            boundary.arrive_and_wait();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (channel->mPendingLines.load(std::memory_order_acquire)
                && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            require(channel->mPendingLines.load() == 0, "consumer did not drain bounded producer burst");
        }
        for (auto& producer : producers)
            producer.join();
        hub.enqueue(rejected, std::string(DiagnosticWriterHub::RowCapacity, 'x'));
        hub.enqueue(rejected, std::string(DiagnosticWriterHub::RowCapacity + 1, 'y'));
        hub.closeChannel(channel);
        hub.closeChannel(empty);
        hub.closeChannel(rejected);
        hub.finish();
        require(channel->mFinished.load() && channel->mDroppedLines.load() == 0,
            "mutex-free producer pressure lost rows below capacity");
        require(rejected->mDroppedLines.load() == 1, "oversized row loss was not explicitly accounted");
        std::ifstream input(path);
        std::string row;
        std::getline(input, row);
        std::set<std::string> records;
        bool footer = false;
        while (std::getline(input, row))
        {
            if (row.starts_with("#"))
            {
                if (row == "# v3_async_diagnostics_dropped_lines=0")
                    footer = true;
                continue;
            }
            require(records.insert(row).second, "duplicate producer record");
        }
        require(records.size() == Producers * Rounds * Rows && footer,
            "orderly finish lost complete owned producer records or zero-loss footer");
        for (unsigned producer = 0; producer < Producers; ++producer)
            for (unsigned round = 0; round < Rounds; ++round)
                for (unsigned index = 0; index < Rows; ++index)
                    require(records.contains(std::to_string(producer) + ',' + std::to_string(round) + ','
                        + std::to_string(index) + ",complete owned row"), "missing producer identity");
        std::ifstream status(path.string() + ".writer-status.txt");
        const std::string text((std::istreambuf_iterator<char>(status)), {});
        require(text.find("normal_finish=1\noutput_ok=1\nrows_dropped=0") != std::string::npos,
            "finished channel has no successful independent output status");
        std::ifstream emptyFile(directory / "empty.csv");
        std::getline(emptyFile, row);
        require(row == "frame,value", "empty enabled channel was not materialized on the disk worker");

        // A missing parent directory forces a real open failure. The inability
        // to create its sidecar must remain missing evidence, never success.
        DiagnosticWriterHub failedHub(true);
        auto failed = failedHub.registerChannel((directory / "absent-parent" / "failed.csv").string(), "value");
        failedHub.enqueue(failed, "one");
        failedHub.finish();
        require(failed->mFinished.load() && failed->mDroppedLines.load() == 1,
            "actual disk-open failure was not accounted as row loss");
    }
}

int main(int argc, char** argv)
try
{
    require(argc == 2, "fixture requires an owned output directory");
    const auto directory = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(directory);
    boundedRing();
    byteBudgetAndPublication();
    actualHub(directory);
    std::cout << "PASS: bounded MPSC wrap/full/byte budget and held publication, 8 actual producers with complete identities, orderly disk drain, "
        "empty channel, explicit oversized/open losses and output sidecars\n";
    return 0;
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
