#include <components/sceneutil/workqueue.hpp>
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>

using Queue = SceneUtil::WorkQueue;
void require(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct Job final : SceneUtil::WorkItem
{
    explicit Job(unsigned id, std::atomic_uint* runs = nullptr) : id(id), runs(runs) {}
    void doWork() override { ++calls; if (runs) ++*runs; }
    unsigned id;
    std::atomic_uint* runs;
    std::atomic_uint calls{0};
};
int main() try
{
    auto queue = osg::ref_ptr<Queue>(new Queue(0));
    std::array<osg::ref_ptr<Job>, 12> jobs;
    for (unsigned i = 0; i < jobs.size(); ++i) jobs[i] = new Job(i);
    queue->addWorkItem(jobs[0]); queue->addWorkItem(jobs[1]);
    queue->addWorkItem(jobs[2], false, Queue::Priority::NearFuture);
    queue->addWorkItem(jobs[3], false, Queue::Priority::NearFuture);
    queue->addWorkItem(jobs[4], true); queue->addWorkItem(jobs[5], true);
    require(queue->promoteWorkItem(jobs[1]), "queued cell was not promoted");
    require(!queue->promoteWorkItem(jobs[1]) && !queue->promoteWorkItem(jobs[6]),
        "promotion duplicated an admission or promoted an unqueued owner");
    require(queue->getNumItems() == 6, "promotion changed queued owner count");
    for (unsigned id : {5u, 4u, 2u, 3u, 1u, 0u})
    {
        SceneUtil::WorkTraceMetadata trace;
        auto taken = queue->removeWorkItem(&trace);
        require(taken.get() == jobs[id].get(), "urgent/FIFO admission order or original owner changed");
        require(!queue->promoteWorkItem(taken), "started work was requeued by promotion");
    }
    queue = new Queue(0);
    queue->addWorkItem(jobs[0]);
    for (unsigned i = 1; i <= 8; ++i) queue->addWorkItem(jobs[i], false, Queue::Priority::NearFuture);
    for (unsigned i = 1; i <= Queue::MaximumNearFutureBurst; ++i)
        require(queue->removeWorkItem().get() == jobs[i].get(), "near-future order was not FIFO");
    queue->addWorkItem(jobs[9], true);
    require(queue->removeWorkItem().get() == jobs[9].get(), "fairness delayed an existing urgent job");
    require(queue->removeWorkItem().get() == jobs[0].get(), "near-future flood starved background work");
    for (unsigned i = 5; i <= 8; ++i)
        require(queue->removeWorkItem().get() == jobs[i].get(), "fairness dropped/repeated a near-future owner");
    queue = new Queue(0);
    osg::ref_ptr<Job> backlogTail;
    for (unsigned i = 0; i < 257; ++i)
    {
        backlogTail = new Job(i);
        queue->addWorkItem(backlogTail);
    }
    require(!queue->promoteWorkItem(backlogTail), "unbounded promotion inspection was admitted");
    queue->addWorkItem(jobs[10], false, Queue::Priority::NearFuture);
    for (unsigned i = 0; i < 257; ++i)
        require(static_cast<Job*>(queue->removeWorkItem().get())->id == i, "backlog fallback changed ordinary FIFO");
    require(queue->removeWorkItem().get() == jobs[10].get(), "backlog fallback lost original near-future job");
    queue->stop();
    require(!queue->promoteWorkItem(jobs[10]), "stopped queue accepted a promotion");
    std::atomic_uint runs{0};
    queue = new Queue(0);
    std::array<osg::ref_ptr<Job>, 80> threaded;
    for (unsigned i = 0; i < threaded.size(); ++i)
    {
        threaded[i] = new Job(i, &runs);
        queue->addWorkItem(threaded[i], false, i % 3 ? Queue::Priority::Background : Queue::Priority::NearFuture);
    }
    for (unsigned i = 1; i < threaded.size(); i += 5) queue->promoteWorkItem(threaded[i]);
    queue->start(3);
    for (auto& item : threaded)
    {
        item->waitTillDone();
        require(item->calls == 1, "an individual parallel owner ran more or less than once");
    }
    queue->stop();
    require(runs == threaded.size() && queue->getNumItems() == 0, "parallel consumption duplicated/lost work");
    std::cout << "PRELOAD_PRIORITY_PROOF original_owners=1 urgent_preserved=1 fifo=1 burst_bound=4"
                 " backlog_fallback=1 no_new_workers=1 parallel_exactly_once=80\n";
    return 0;
}
catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
