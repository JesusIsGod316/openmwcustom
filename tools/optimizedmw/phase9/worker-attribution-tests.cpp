// Real WorkQueue admission/execution/wait/cancellation plus the production
// physics cycle/barrier wrapper. No copied queue or scheduler algorithms.
#include <apps/openmw/mwphysics/physicstrace.hpp>
#include <components/debug/hitchtrace.hpp>
#include <components/misc/barrier.hpp>
#include <components/sceneutil/workqueue.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }

    class CountingWork final : public SceneUtil::WorkItem
    {
    public:
        void doWork() override
        {
            if (Debug::HitchTrace::requested())
            {
                require(Debug::HitchTrace::current != nullptr, "Missing production worker scope");
                mFrames.push_back(Debug::HitchTrace::current->frame());
            }
            ++mCalls;
        }
        void abort() override { ++mAborts; }
        unsigned mCalls = 0, mAborts = 0;
        std::vector<unsigned> mFrames;
    };

    class BlockingWork final : public SceneUtil::WorkItem
    {
    public:
        void doWork() override
        {
            std::unique_lock lock(mMutex);
            mStarted = true;
            mCondition.notify_all();
            require(mCondition.wait_for(lock, std::chrono::seconds(5), [&] { return mReleased; }),
                "Blocking production work timed out");
        }
        void waitStarted()
        {
            std::unique_lock lock(mMutex);
            require(mCondition.wait_for(lock, std::chrono::seconds(5), [&] { return mStarted; }),
                "Worker did not start");
        }
        void release()
        {
            const std::lock_guard lock(mMutex);
            mReleased = true;
            mCondition.notify_all();
        }
    private:
        std::mutex mMutex;
        std::condition_variable mCondition;
        bool mStarted = false, mReleased = false;
    };

    std::vector<std::string> splitCsv(const std::string& line)
    {
        std::vector<std::string> fields;
        std::string value;
        bool quoted = false;
        for (std::size_t i = 0; i < line.size(); ++i)
        {
            const char ch = line[i];
            if (ch == '"')
            {
                if (quoted && i + 1 < line.size() && line[i + 1] == '"') { value += ch; ++i; }
                else quoted = !quoted;
            }
            else if (ch == ',' && !quoted) { fields.push_back(value); value.clear(); }
            else value += ch;
        }
        fields.push_back(value);
        return fields;
    }

    void verifyCapture(const std::filesystem::path& path, std::uint64_t reusedCompletion)
    {
        std::ifstream input(path);
        require(bool(input), "Capture output missing");
        using Row = std::vector<std::string>;
        std::map<std::uint64_t, Row> rows;
        std::string line;
        bool complete = false;
        while (std::getline(input, line))
        {
            if (line.starts_with("# dropped=")) require(line == "# dropped=0", "Capture dropped rows");
            if (line.starts_with("# active_at_finish=")) require(line == "# active_at_finish=0", "Live spans at finish");
            if (line == "# normal_finish=1") complete = true;
            if (line.empty() || line[0] == '#' || line.starts_with("span,")) continue;
            auto row = splitCsv(line);
            require(row.size() == 24, "Unexpected trace schema");
            rows.emplace(std::stoull(row[0]), std::move(row));
        }
        require(complete, "Capture incomplete");
        require(rows.contains(reusedCompletion) && rows.at(reusedCompletion)[7] == "workqueue.enqueue"
            && rows.at(reusedCompletion)[2] == "41", "Reused done item no longer identifies its first wait satisfier");
        unsigned executions = 0, cancellations = 0, blockingWaits = 0, physicsCompletions = 0;
        std::vector<unsigned> reuseFrames;
        std::vector<Row> frameAnchors;
        for (const auto& [id, row] : rows)
        {
            const auto parent = std::stoull(row[1]);
            const auto link = std::stoull(row[15]);
            if (parent) require(rows.contains(parent) && rows.at(parent)[3] == row[3], "Invalid nesting parent");
            if (link) require(rows.contains(link), "Dropped causal source");
            if (row[7] == "workqueue.execute")
            {
                ++executions;
                require(parent == 0, "Worker uses cross-thread nesting parent");
                require(link && rows.at(link)[7] == "workqueue.enqueue", "Wrong worker admission link");
                require(row[2] == rows.at(link)[2], "Worker lost originating frame");
                if (row[2] == "41" || row[2] == "42") reuseFrames.push_back(std::stoul(row[2]));
            }
            if (row[7] == "workqueue.cancel_before_start")
            {
                ++cancellations;
                require(link && rows.at(link)[7] == "workqueue.enqueue", "Cancellation has no admission");
            }
            if (row[7] == "workqueue.wait" && link && rows.at(link)[2] == "43") ++blockingWaits;
            if (row[7] == "frame.wall_interval")
            {
                require(parent == 0 && link == 0 && row[13] == "-1.000000", "Frame anchor became work or nesting");
                frameAnchors.push_back(row);
            }
            if (row[7] == "physics.cycle.complete")
            {
                ++physicsCompletions;
                require(link && rows.at(link)[7] == "physics.cycle.enqueue", "Physics completion has no cycle");
                require(row[2] == rows.at(link)[2], "Physics cycle frame changed");
            }
        }
        require(executions == 3 && reuseFrames == std::vector<unsigned>({ 41, 42 }), "Reuse lost separate admissions");
        require(cancellations == 1, "Never-run cancellation receipt missing");
        require(blockingWaits >= 1, "Wait does not link actual completing admission");
        require(physicsCompletions == 2, "Physics threaded/serial cycle completion missing");
        require(frameAnchors.size() == 2 && frameAnchors[0][2] == "61" && frameAnchors[1][2] == "62",
            "Prior or final frame anchor ownership changed");
        require(frameAnchors[0][3] == frameAnchors[1][3] && frameAnchors[0][5] == frameAnchors[1][4],
            "Production frame intervals do not share their exact sampled boundary");
        std::ifstream frames(path.string() + ".frames.csv");
        require(bool(frames), "Production frame output missing");
        require(bool(std::getline(frames, line)) && line.starts_with("frame,epoch_ms,wall_ms,"), "Frame CSV schema missing");
        for (const auto& anchor : frameAnchors)
        {
            require(bool(std::getline(frames, line)), "Production frame CSV lost an interval");
            const auto frame = splitCsv(line);
            require(frame.size() >= 3 && frame[0] == anchor[2] && frame[1] == anchor[6]
                && std::abs(std::stod(frame[2]) - std::stod(anchor[10])) <= 0.000501,
                "Frame anchor does not describe the actual rounded frame CSV interval");
        }
        require(!std::getline(frames, line), "Unexpected additional frame interval");
    }

    std::uint64_t queueFixture(bool armed)
    {
        SceneUtil::WorkQueue queue(0);
        osg::ref_ptr<CountingWork> reusable = new CountingWork;
        {
            Debug::HitchTrace::Scope origin("fixture.origin", {}, {}, 41);
            queue.addWorkItem(reusable);
        }
        {
            Debug::HitchTrace::Scope origin("fixture.origin", {}, {}, 42);
            queue.addWorkItem(reusable);
        }
        queue.start(1);
        reusable->waitTillDone();
        // stop joins running work and preserves the second queued admission by
        // waiting until the real queue has dequeued it before shutdown.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (queue.getNumItems() != 0)
        {
            require(std::chrono::steady_clock::now() < deadline, "Reuse worker queue timed out");
            std::this_thread::yield();
        }
        queue.stop();
        require(reusable->mCalls == 2, "Reuse changed WorkQueue execution");
        if (armed) require(reusable->mFrames == std::vector<unsigned>({ 41, 42 }), "Worker frame inheritance failed");
        require(reusable->mAborts == 0 && reusable->isDone(), "Reuse changed completion/abort semantics");
        reusable->waitTillDone(); // real already-done path after both admissions completed
        const auto reusedCompletion = reusable->diagnosticCompletionEnqueue();
        require(armed ? reusedCompletion != 0 : reusedCompletion == 0, "Completion oracle changed Normal/Deep gating");

        SceneUtil::WorkQueue cancelledQueue(0);
        osg::ref_ptr<CountingWork> neverRun = new CountingWork;
        cancelledQueue.addWorkItem(neverRun, true);
        cancelledQueue.stop();
        require(neverRun->mCalls == 0 && neverRun->mAborts == 0 && !neverRun->isDone(), "Cancellation semantics changed");

        SceneUtil::WorkQueue blockingQueue(1);
        osg::ref_ptr<BlockingWork> blocking = new BlockingWork;
        {
            Debug::HitchTrace::Scope origin("fixture.origin", {}, {}, 43);
            blockingQueue.addWorkItem(blocking);
        }
        blocking->waitStarted();
        std::thread release([&] {
            // Deliberately stall the real work so waitTillDone must take its
            // pending path. This is a synchronization fixture, not a benchmark.
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            blocking->release();
        });
        blocking->waitTillDone();
        release.join();
        blockingQueue.stop();
        require(blocking->isDone(), "Production completion failed");
        return reusedCompletion;
    }

    void frameFixture()
    {
        Debug::V3HitchTelemetry::State frames;
        frames.beginFrame(61);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        frames.beginFrame(62);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        frames.finish();
    }

    void physicsFixture()
    {
        MWPhysics::PhysicsTraceCycle cycle;
        cycle.begin(51, 3, 1, 2);
        Misc::Barrier barrier(2);
        std::atomic<unsigned> callbacks{ 0 };
        const auto worker = [&] {
            MWPhysics::PhysicsTraceStage execution(cycle, "physics.fixture_worker", Debug::HitchTrace::CpuTiming::Scoped);
            MWPhysics::tracedPhysicsBarrier(cycle, "physics.fixture_barrier", barrier, [&] {
                ++callbacks;
                MWPhysics::PhysicsTraceStage complete(cycle, "physics.cycle.complete");
                complete.retain();
            });
        };
        std::thread first(worker), second(worker);
        first.join(); second.join();
        require(callbacks == 1, "Production barrier callback count changed");
        cycle.begin(52, 0, 0, 0);
        Misc::Barrier serial(0);
        MWPhysics::tracedPhysicsBarrier(cycle, "physics.fixture_barrier", serial, [&] {
            ++callbacks;
            MWPhysics::PhysicsTraceStage complete(cycle, "physics.cycle.complete");
            complete.retain();
        });
        require(callbacks == 2, "Serial barrier behavior changed");
    }
}

int main(int argc, char** argv)
{
    try
    {
        require(argc == 3, "Usage: p9-worker-attribution CAPTURE.csv armed|unarmed");
        const bool armed = std::string_view(argv[2]) == "armed";
        const std::string framePath = std::string(argv[1]) + ".frames.csv";
        const auto setEnvironment = [](const char* name, const char* value) {
#ifdef _WIN32
            require(_putenv_s(name, value) == 0, "Cannot set fixture environment");
#else
            require(setenv(name, value, 1) == 0, "Cannot set fixture environment");
#endif
        };
        setEnvironment("OPENMW_P9_HITCH_SPANS_FILE", armed ? argv[1] : "0");
        setEnvironment("OPENMW_P8G4_CLEAN_CAPTURE", armed ? "1" : "0");
        setEnvironment("OPENMW_V3_FRAME_FILE", armed ? framePath.c_str() : "0");
        setEnvironment("OPENMW_V3_HITCH_FILE", "0");
        const auto reusedCompletion = queueFixture(armed);
        physicsFixture();
        frameFixture();
        Debug::HitchTrace::Capture::instance().finish();
        if (armed) verifyCapture(argv[1], reusedCompletion);
        else require(!std::filesystem::exists(argv[1]) && !std::filesystem::exists(framePath), "Normal mode emitted trace/frame output");
        std::cout << "PASS: production queue, physics barrier and exact frame attribution\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
