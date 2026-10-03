#include <components/resource/sharedcontextpreparation.hpp>
#include <components/shader/programvariant.hpp>

#include <osgViewer/Viewer>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value)
            throw std::runtime_error(message);
    }

    class ProducerThread
    {
    public:
        explicit ProducerThread(osg::GraphicsContext& context)
            : mContext(&context), mThread([this] { run(); })
        {
        }
        ~ProducerThread() { stop(); }
        std::future<void> submit(std::function<void()> operation)
        {
            auto task = std::make_shared<std::packaged_task<void()>>([this, operation = std::move(operation)] {
                require(mContext->makeCurrent(), "dedicated producer context ownership failed");
                try
                {
                    operation();
                    mContext->releaseContext();
                }
                catch (...)
                {
                    mContext->releaseContext();
                    throw;
                }
            });
            auto result = task->get_future();
            {
                std::lock_guard lock(mMutex);
                require(!mStopped, "submission after producer stop");
                mWork.push([task] { (*task)(); });
            }
            mChanged.notify_one();
            return result;
        }
        void stop()
        {
            {
                std::lock_guard lock(mMutex);
                mStopped = true;
            }
            mChanged.notify_one();
            if (mThread.joinable())
                mThread.join();
        }
    private:
        void run()
        {
            for (;;)
            {
                std::function<void()> task;
                {
                    std::unique_lock lock(mMutex);
                    mChanged.wait(lock, [this] { return mStopped || !mWork.empty(); });
                    if (mWork.empty())
                        return;
                    task = std::move(mWork.front());
                    mWork.pop();
                }
                task();
            }
        }
        osg::ref_ptr<osg::GraphicsContext> mContext;
        std::mutex mMutex;
        std::condition_variable mChanged;
        std::queue<std::function<void()>> mWork;
        bool mStopped = false;
        // Starts only after every synchronization object above is initialized.
        std::thread mThread;
    };

    template <class Predicate>
    void eventually(Predicate predicate, const char* failure)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate())
        {
            require(std::chrono::steady_clock::now() < deadline, failure);
            // Fixture polling only. The transfer implementation uses timeout=0.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    void checkRealVariantReadiness(osg::GraphicsContext& context, osg::GraphicsContext& other)
    {
        require(Shader::exactProgramVariantsEnabled(), "set OPENMW_EXACT_PROGRAM_VARIANTS=1 for real GL variant fixture");
        require(context.makeCurrent(), "variant fixture context failed");
        auto& state = *context.getState();
        osg::ref_ptr<Shader::VariantProgram> program = new Shader::VariantProgram;
        program->addShader(new osg::Shader(osg::Shader::VERTEX,
            "#version 120\n#pragma import_defines(TEST_VALUE)\nvoid main(){gl_Position=gl_Vertex;}\n"));
        program->addShader(new osg::Shader(osg::Shader::FRAGMENT,
            "#version 120\nvoid main(){gl_FragColor=vec4(1.0);}\n"));
        program->setShaderDefines({ "TEST_VALUE" });
        osg::ref_ptr<osg::StateSet> first = new osg::StateSet;
        first->setDefine("TEST_VALUE", "1");
        state.pushStateSet(first);
        require(Shader::compileExactProgramVariant(*program, state, 123), "exact producer link was not completed");
        auto* firstPcp = program->getPCP(state);
        require(firstPcp && firstPcp->isLinked() && !firstPcp->needsLink(), "exact PCP is not linked");
        require(program->takePreparedFirstUse(state, *firstPcp).ready, "actual owner lost prepared readiness");
        require(!program->takePreparedFirstUse(state, *firstPcp).ready, "prepared first use counted twice");
        state.popStateSet();
        osg::ref_ptr<osg::StateSet> second = new osg::StateSet;
        second->setDefine("TEST_VALUE", "2");
        state.pushStateSet(second);
        auto* secondPcp = program->getPCP(state);
        require(secondPcp != firstPcp, "different effective defines reused the wrong PCP");
        require(!program->takePreparedFirstUse(state, *secondPcp).ready, "unprepared define variant was declared ready");
        require(Shader::compileExactProgramVariant(*program, state, 124), "second exact variant failed");
        program->invalidateVariantPreparations();
        require(!program->takePreparedFirstUse(state, *secondPcp).ready, "release/source epoch retained stale readiness");
        require(Shader::compileExactProgramVariant(*program, state, 125), "post-invalidation preparation failed");
        state.popStateSet();
        context.releaseContext();

        require(other.makeCurrent(), "negative context fixture failed");
        auto& otherState = *other.getState();
        otherState.pushStateSet(second);
        auto* otherPcp = program->getPCP(otherState);
        require(!program->takePreparedFirstUse(otherState, *otherPcp).ready,
            "sharing/context-ID equality bypassed the actual context lease");
        otherState.popStateSet();
        other.releaseContext();
        require(context.makeCurrent(), "variant cleanup owner failed");
        program->releaseGLObjects(&state);
        require(glGetError() == GL_NO_ERROR, "variant fixture GL error");
        context.releaseContext();
    }
}

int main() try
{
    osgViewer::Viewer anchor;
    osg::ref_ptr<osg::GraphicsContext::Traits> traits = new osg::GraphicsContext::Traits;
    traits->readDISPLAY();
    traits->setUndefinedScreenDetailsToDefaultScreen();
    traits->width = 32;
    traits->height = 32;
    traits->doubleBuffer = false;
    traits->windowDecoration = false;
    osg::ref_ptr<osg::GraphicsContext> consumer = osg::GraphicsContext::createGraphicsContext(traits);
    require(consumer && consumer->realize(), "actual consumer GL context unavailable");
    osg::ref_ptr<osg::GraphicsContext::Traits> producerTraits = new osg::GraphicsContext::Traits(*traits);
    producerTraits->sharedContext = consumer;
    osg::ref_ptr<osg::GraphicsContext> producer = osg::GraphicsContext::createGraphicsContext(producerTraits);
    require(producer && producer->realize(), "actual shared producer GL context unavailable");
    checkRealVariantReadiness(*consumer, *producer);

    using Queue = Resource::SharedContextPreparation;
    Queue queue(*producer, *consumer, 64u * 1024u, 2);
    ProducerThread worker(*producer);
    std::vector<unsigned char> input(32u * 32u * 4u);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<unsigned char>((i * 29u + 7u) & 255u);
    std::optional<Queue::Ticket> first, second;
    auto submitted = worker.submit([&] {
        first = queue.submitRGBA8(32, 32, input.data(), input.size());
        second = queue.submitRGBA8(32, 32, input.data(), input.size());
        require(first && second, "private producer did not submit actual shared textures");
        require(!queue.submitRGBA8(32, 32, input.data(), input.size()), "slot bound was bypassed");
        require(!queue.submitRGBA8(1024, 1024, input.data(), input.size()), "payload bound was bypassed");
        require(glGetError() == GL_NO_ERROR, "producer upload GL error");
    });
    require(consumer->makeCurrent(), "actual consumer ownership unavailable");
    // The render owner can continue issuing unrelated commands on its context
    // while the dedicated producer initiates resource work. No latency claim.
    glClear(GL_COLOR_BUFFER_BIT);
    glFlush();
    submitted.get();
    require(queue.chargedBytes() == input.size() * 2, "admitted payload capacity was not charged");
    eventually([&] {
        const auto result = queue.pollConsumer(*first);
        require(result != Queue::Poll::Failed && result != Queue::Poll::Stale, "shared visibility/context validation failed");
        return result == Queue::Poll::Ready;
    }, "producer fence did not become visible to consumer");
    const auto texture = queue.readyTexture(*first);
    require(texture != 0 && glIsTexture(texture), "consumer could not access the actual shared texture object");
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    std::vector<unsigned char> output(input.size());
    // Readback is fixture evidence, never part of the proposed preparation path.
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, output.data());
    require(output == input, "shared producer/consumer texture contents differ");
    glBindTexture(GL_TEXTURE_2D, 0);
    require(queue.retireConsumer(*first), "consumer-use retirement fence was not submitted");
    require(queue.chargedBytes() == input.size() * 2, "retiring texture released its byte charge before proof");
    require(!queue.readyTexture(*first), "retiring resource remained consumable");
    eventually([&] { queue.pollRetirement(*consumer); return queue.liveSlots() == 1; },
        "producer plus consumer fence retirement did not complete");
    require(queue.chargedBytes() == input.size(), "completed retirement did not release exactly one payload charge");

    worker.stop();
    require(queue.beginShutdownConsumer(), "shutdown did not fence outstanding consumer use");
    queue.notifyContextLost();
    require(queue.pollConsumer(*second) == Queue::Poll::Stale, "lost context retained a consumable receipt");
    require(queue.chargedBytes() == input.size(), "loss discarded charge before shared ownership ended");
    require(!queue.releaseAfterBothContextsClosed(), "live share group was incorrectly considered destroyed");
    eventually([&] { queue.pollRetirement(*consumer); return queue.liveSlots() == 0; },
        "shutdown retirement did not complete");
    require(queue.chargedBytes() == 0, "shutdown left admitted payload bytes charged");
    require(glGetError() == GL_NO_ERROR, "consumer visibility/retirement GL error");
    consumer->releaseContext();

    Queue lostQueue(*producer, *consumer, 64u * 1024u, 1);
    std::optional<Queue::Ticket> lostTicket;
    ProducerThread lossWorker(*producer);
    lossWorker.submit([&] { lostTicket = lostQueue.submitRGBA8(32, 32, input.data(), input.size()); }).get();
    lossWorker.stop();
    require(lostTicket && lostQueue.chargedBytes() == input.size(), "context-loss setup failed");
    // Actual native context destruction, not a simulated GPU reset. The other
    // shared context still owns backing storage, so its charge cannot disappear.
    consumer->close(true);
    lostQueue.notifyContextLost();
    require(lostQueue.pollConsumer(*lostTicket) == Queue::Poll::Stale, "destroyed consumer still accepted a lease");
    require(lostQueue.chargedBytes() == input.size(), "consumer destruction released a surviving share-group charge");
    require(!lostQueue.releaseAfterBothContextsClosed(), "surviving producer was mistaken for a destroyed share group");
    producer->close(true);
    require(lostQueue.releaseAfterBothContextsClosed() && lostQueue.chargedBytes() == 0,
        "actual share-group destruction failed to discharge orphaned payload ownership");
    require(queue.releaseAfterBothContextsClosed(), "closed share-group release failed");
    std::cout << "PASS real shared-context preparation: dedicated producer, exact pixels, producer flush/fence, "
                 "consumer zero-time poll/rebind, bounded payload/slots, deferred consumer retirement, "
                 "context-loss rejection, shutdown, real shader variant readiness and negative define/context/epoch guards\n";
}
catch (const std::exception& error)
{
    std::cerr << error.what() << '\n';
    return 1;
}
