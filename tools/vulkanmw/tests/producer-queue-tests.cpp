#include <components/misc/producerqueue.hpp>
#include <components/sceneutil/rendermutation.hpp>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace
{
    void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
    struct Source : SceneUtil::RenderMutationSource
    {
        unsigned mask = SceneUtil::RenderTransform;
        unsigned renderMutationMask() const noexcept override { return mask; }
        void mutate() { publishRenderMutation(); }
        void invalidate() { invalidateRenderMutationBindings(); }
    };
}
int main()
{
    unsigned count = 0;
    auto test = [&](const char* name, auto body) {
        body(); ++count; std::cout << "PASS " << name << '\n';
    };
    try
    {
        test("new producer has exactly one initial delta", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add();
            const auto first = queue.take();
            require(first.size() == 1 && first.front().token == ticket->token(), "initial registration lost");
            for (unsigned i = 0; i < 1000; ++i) require(queue.take().empty(), "clean producer inspected");
        });
        test("coalesced mutations preserve reasons", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); queue.take();
            for (unsigned i = 0; i < 10000; ++i) ticket->notify(i % 2 ? 2 : 4);
            const auto changes = queue.take();
            require(changes.size() == 1 && changes[0].reasons == 6, "coalescing lost mask or duplicated work");
        });
        test("mutation during consumption is not acknowledged away", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add();
            auto snapshot = queue.take(); ticket->notify(8);
            require(snapshot.size() == 1, "seed missing");
            const auto next = queue.take();
            require(next.size() == 1 && next[0].reasons == 8, "reentrant notification lost");
        });
        test("continuous compatibility and dirty notification deduplicate", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); ticket->continuous(true);
            require(queue.take().size() == 1, "initial continuous double visit");
            ticket->notify(2); require(queue.take().size() == 1, "continuous dirty double visit");
            require(queue.take().size() == 1, "compatibility work stopped");
            ticket->continuous(false); require(queue.take().empty(), "supported producer still scanned");
        });
        test("cancel and recycled generation reject stale events", [] {
            Misc::ProducerQueue queue(1); auto old = queue.add(); const auto token = old->token();
            old->cancel(); auto current = queue.add();
            require(current && current->token() != token, "slot generation not changed");
            old->notify(); old->continuous(true); old->cancel();
            auto changes = queue.take();
            require(changes.size() == 1 && changes[0].token == current->token(), "stale event changed new owner");
            require(!queue.valid(token) && queue.valid(current->token()), "stale generation still valid");
        });
        test("retirement after snapshot invalidates the dispatch", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); auto snapshot = queue.take();
            ticket->cancel(); require(!queue.valid(snapshot[0].token), "removed object can still dispatch");
        });
        test("queue budget fallback and recovery", [] {
            Misc::ProducerQueue queue(2); auto a = queue.add(), b = queue.add();
            require(!queue.add(), "bounded queue exceeded budget");
            a.reset(); auto c = queue.add();
            require(c && queue.size() == 2 && queue.take().size() == 2, "retirement did not free capacity");
            Misc::ProducerQueue zero(0); require(!zero.add(), "zero budget ignored");
        });
        test("tokens can outlive renderer queue", [] {
            std::shared_ptr<Misc::ProducerQueue::Ticket> ticket;
            { Misc::ProducerQueue queue; ticket = queue.add(); }
            ticket->notify(); ticket->continuous(true); ticket->cancel(); ticket.reset();
        });
        test("controller mutation and destruction schedule the owner", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); queue.take();
            auto signal = std::make_shared<Source::Subscription>();
            const std::weak_ptr<Misc::ProducerQueue::Ticket> weak = ticket;
            signal->wake = [weak] { if (auto live = weak.lock()) live->notify(16); };
            {
                Source source; source.subscribeRenderMutations(signal);
                source.mutate(); require(queue.take().size() == 1, "controller failed to schedule");
                signal->changed = false;
                Source copy(source); copy.mutate();
                require(queue.take().empty() && !signal->changed, "copy retained a subscriber");
                source.invalidate(); require(queue.take().size() == 1 && signal->invalidated, "rebind not scheduled");
                signal->invalidated = false;
            }
            require(queue.take().size() == 1 && signal->invalidated, "destroyed source left sleeping owner");
        });
        test("unknown controller change invalidates rather than sleeping", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); queue.take();
            auto signal = std::make_shared<Source::Subscription>();
            signal->wake = [ticket] { ticket->notify(); };
            Source source; source.subscribeRenderMutations(signal);
            source.mask = SceneUtil::RenderUntracked; source.mutate();
            require(signal->invalidated && queue.take().size() == 1, "unknown mutation silently accepted");
        });
        test("cold inventory, steady work proportional to dirty plus fallback", [] {
            Misc::ProducerQueue queue; std::vector<std::shared_ptr<Misc::ProducerQueue::Ticket>> objects;
            for (unsigned i = 0; i < 10000; ++i) objects.push_back(queue.add());
            for (unsigned i = 0; i < 4; ++i) objects[i]->continuous(true);
            require(queue.take().size() == 10000, "initial inventory incomplete");
            for (unsigned frame = 0; frame < 100; ++frame)
            {
                objects[500]->notify(); objects[500]->notify();
                require(queue.take().size() == 5, "known clean objects reentered the broad scan");
            }
            queue.invalidateAll(); require(queue.take().size() == 10000, "epoch/configuration rebind incomplete");
            require(queue.take().size() == 4, "global invalidation became recurring inventory scan");
        });
        test("concurrent signalling and cancellation", [] {
            Misc::ProducerQueue queue; auto ticket = queue.add(); queue.take();
            std::atomic_bool done{false};
            std::thread producer([&] { for (unsigned i = 0; i < 100000; ++i) ticket->notify(2); done = true; });
            while (!done.load()) for (const auto& change : queue.take())
                require(change.token == ticket->token(), "concurrent signal corrupted identity");
            producer.join(); queue.take();
            ticket->notify(4); const auto last = queue.take();
            require(last.size() == 1 && last[0].reasons == 4, "last mutation lost");
            std::thread late([ticket] { for (unsigned i = 0; i < 1000; ++i) ticket->notify(); });
            ticket->cancel(); late.join(); require(queue.take().empty(), "retired producer retained work");
        });
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << count << "/" << count << " producer queue tests passed\n";
}
