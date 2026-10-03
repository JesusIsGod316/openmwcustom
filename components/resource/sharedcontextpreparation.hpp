#ifndef OPENMW_COMPONENTS_RESOURCE_SHAREDCONTEXTPREPARATION_H
#define OPENMW_COMPONENTS_RESOURCE_SHAREDCONTEXTPREPARATION_H

#include <osg/GLExtensions>
#include <osg/GraphicsContext>
#include <osg/State>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace Resource
{
    // Private prerequisite fixture, deliberately not a game-side switch. Only
    // pristine RGBA8 texture objects are transferred. No graph, FBO, VAO,
    // borrowed State, live texture rewrite or OSG per-context cache crosses it.
    // Callers serialize context teardown with these operations and stop their
    // producer thread before shutdown. GL initiation may still block a driver.
    class SharedContextPreparation
    {
    public:
        using Sync = decltype(std::declval<osg::GLExtensions>().glFenceSync(GLenum{}, GLbitfield{}));
        struct Ticket
        {
            std::uint64_t id = 0, generation = 0;
        };
        enum class Poll { Pending, Ready, Stale, Failed };

        SharedContextPreparation(osg::GraphicsContext& producer, osg::GraphicsContext& consumer,
            std::size_t byteLimit, std::size_t slotLimit)
            : mProducer(&producer)
            , mConsumer(&consumer)
            , mByteLimit(byteLimit)
            , mSlotLimit(slotLimit)
        {
            // Actual platform sharing must subsequently be proved by the real
            // fence/texture consumer test; equal OSG context IDs are insufficient.
            mSharingRequested = producer.getTraits() && consumer.getTraits()
                && (producer.getTraits()->sharedContext.get() == &consumer
                    || consumer.getTraits()->sharedContext.get() == &producer);
        }

        SharedContextPreparation(const SharedContextPreparation&) = delete;
        SharedContextPreparation& operator=(const SharedContextPreparation&) = delete;

        std::optional<Ticket> submitRGBA8(int width, int height, const unsigned char* data, std::size_t bytes)
        {
            std::lock_guard lock(mMutex);
            auto* context = mProducer.get();
            if (!mSharingRequested || mStopped || !bothRealized() || !current(context)
                || !data || width <= 0 || height <= 0 || static_cast<std::size_t>(width) > mByteLimit / 4
                || static_cast<std::size_t>(height) > mByteLimit / (static_cast<std::size_t>(width) * 4))
                return {};
            const auto exactBytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
            if (bytes != exactBytes || bytes > mByteLimit - std::min(mBytes, mByteLimit)
                || mSlots.size() >= mSlotLimit)
                return {};
            auto* ext = extensions(*context);
            if (!ext || glGetError() != GL_NO_ERROR)
                return {};
            // Reserve before touching GL. Capacity charges remain through both
            // producer completion and the final consumer-use retirement fence.
            mSlots.emplace_back();
            auto& slot = mSlots.back();
            slot.ticket = { ++mNextId, mGeneration };
            slot.bytes = bytes;
            mBytes += bytes;
            GLint binding = 0, alignment = 0, rowLength = 0, skipRows = 0, skipPixels = 0, pbo = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
            glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
            glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING_ARB, &pbo);
            ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, 0);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
            glGenTextures(1, &slot.texture);
            glBindTexture(GL_TEXTURE_2D, slot.texture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            slot.producerFence = ext->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            // A consumer-context flush cannot submit the producer's commands.
            glFlush();
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(binding));
            ext->glBindBuffer(GL_PIXEL_UNPACK_BUFFER_ARB, static_cast<GLuint>(pbo));
            glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
            glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
            if (!slot.texture || !slot.producerFence || glGetError() != GL_NO_ERROR)
            {
                // GL deletion preserves backing storage needed by submitted
                // commands, but this failed queue never publishes a lease.
                destroy(slot, *ext);
                mSlots.pop_back();
                mStopped = true;
                return {};
            }
            return slot.ticket;
        }

        Poll pollConsumer(Ticket ticket)
        {
            // A slow producer driver initiation must not turn a readiness poll
            // into a CPU mutex wait. Pending preserves ordinary demand fallback.
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return Poll::Pending;
            auto* context = mConsumer.get();
            auto* slot = find(ticket);
            if (ticket.generation != mGeneration || !slot || !bothRealized() || !current(context) || slot->retiring)
                return Poll::Stale;
            auto* ext = extensions(*context);
            if (!ext)
                return Poll::Failed;
            if (slot->ready)
                return Poll::Ready;
            const auto result = ext->glClientWaitSync(slot->producerFence, 0, 0);
            if (result == GL_TIMEOUT_EXPIRED)
                return Poll::Pending;
            if (result != GL_ALREADY_SIGNALED && result != GL_CONDITION_SATISFIED)
                return Poll::Failed;
            if (!glIsTexture(slot->texture))
                return Poll::Failed;
            // Shared-object completion and context attachment are separate. A
            // rebind after completion imports the producer's texture changes.
            GLint previous = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
            glBindTexture(GL_TEXTURE_2D, slot->texture);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previous));
            slot->ready = true;
            return Poll::Ready;
        }

        GLuint readyTexture(Ticket ticket) const
        {
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return 0;
            const auto* slot = find(ticket);
            return ticket.generation == mGeneration && slot && slot->ready && !slot->retiring
                && bothRealized() && current(mConsumer.get()) ? slot->texture : 0;
        }

        bool retireConsumer(Ticket ticket)
        {
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return false;
            auto* context = mConsumer.get();
            auto* slot = find(ticket);
            if (!slot || !current(context) || !context->isRealized())
                return false;
            if (slot->retiring)
                return true;
            auto* ext = extensions(*context);
            if (!ext)
                return false;
            slot->consumerFence = ext->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            glFlush();
            if (!slot->consumerFence)
                return false;
            slot->retiring = true;
            return true;
        }

        std::size_t pollRetirement(osg::GraphicsContext& context)
        {
            std::unique_lock lock(mMutex, std::try_to_lock);
            if (!lock.owns_lock())
                return 0;
            if ((&context != mProducer.get() && &context != mConsumer.get()) || !current(&context))
                return 0;
            auto* ext = extensions(context);
            if (!ext)
                return 0;
            std::size_t removed = 0;
            for (auto it = mSlots.begin(); it != mSlots.end();)
                if (it->retiring && signaled(it->producerFence, *ext) && signaled(it->consumerFence, *ext))
                {
                    destroy(*it, *ext);
                    it = mSlots.erase(it);
                    ++removed;
                }
                else
                    ++it;
            return removed;
        }

        // Stop the producer first. Retirement only begins on the actual consumer
        // owner after its last use; no blocking wait or GL call in the destructor.
        bool beginShutdownConsumer()
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
            auto* context = mConsumer.get();
            if (!current(context))
                return false;
            auto* ext = extensions(*context);
            if (!ext)
                return false;
            bool okay = true;
            for (auto& slot : mSlots)
                if (!slot.retiring)
                {
                    slot.consumerFence = ext->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
                    slot.retiring = slot.consumerFence != nullptr;
                    okay = okay && slot.retiring;
                }
            glFlush();
            return okay;
        }

        void notifyContextLost()
        {
            std::lock_guard lock(mMutex);
            mStopped = true;
            ++mGeneration;
            // Do not issue GL on a lost owner or release charges while a surviving
            // shared context may still own its texture backing allocation.
            for (auto& slot : mSlots)
                slot.ready = false;
        }

        bool releaseAfterBothContextsClosed()
        {
            std::lock_guard lock(mMutex);
            if ((mProducer.valid() && mProducer->isRealized()) || (mConsumer.valid() && mConsumer->isRealized()))
                return false;
            mStopped = true;
            ++mGeneration;
            mSlots.clear();
            mBytes = 0;
            return true;
        }

        std::size_t chargedBytes() const { std::lock_guard lock(mMutex); return mBytes; }
        std::size_t liveSlots() const { std::lock_guard lock(mMutex); return mSlots.size(); }

    private:
        struct Slot
        {
            Ticket ticket;
            GLuint texture = 0;
            Sync producerFence = nullptr, consumerFence = nullptr;
            std::size_t bytes = 0;
            bool ready = false, retiring = false;
        };
        static bool current(osg::GraphicsContext* context)
        { return context && context->isRealized() && context->isCurrent(); }
        bool bothRealized() const
        { return mProducer.valid() && mConsumer.valid() && mProducer->isRealized() && mConsumer->isRealized(); }
        static osg::GLExtensions* extensions(osg::GraphicsContext& context)
        {
            if (!context.getState())
                return nullptr;
            auto* ext = context.getState()->get<osg::GLExtensions>();
            return ext && ext->glBindBuffer && ext->glFenceSync && ext->glClientWaitSync && ext->glDeleteSync
                ? ext : nullptr;
        }
        static bool signaled(Sync fence, osg::GLExtensions& ext)
        {
            if (!fence)
                return false;
            const auto result = ext.glClientWaitSync(fence, 0, 0);
            return result == GL_ALREADY_SIGNALED || result == GL_CONDITION_SATISFIED;
        }
        Slot* find(Ticket ticket)
        {
            const auto found = std::find_if(mSlots.begin(), mSlots.end(),
                [&](const Slot& slot) { return slot.ticket.id == ticket.id && slot.ticket.generation == ticket.generation; });
            return found == mSlots.end() ? nullptr : &*found;
        }
        const Slot* find(Ticket ticket) const { return const_cast<SharedContextPreparation*>(this)->find(ticket); }
        void destroy(Slot& slot, osg::GLExtensions& ext)
        {
            if (slot.producerFence)
                ext.glDeleteSync(slot.producerFence);
            if (slot.consumerFence)
                ext.glDeleteSync(slot.consumerFence);
            if (slot.texture)
                glDeleteTextures(1, &slot.texture);
            mBytes -= slot.bytes;
            slot = {};
        }
        osg::observer_ptr<osg::GraphicsContext> mProducer, mConsumer;
        const std::size_t mByteLimit, mSlotLimit;
        bool mSharingRequested = false, mStopped = false;
        std::uint64_t mNextId = 0, mGeneration = 1;
        std::size_t mBytes = 0;
        std::vector<Slot> mSlots;
        mutable std::mutex mMutex;
    };
}

#endif
