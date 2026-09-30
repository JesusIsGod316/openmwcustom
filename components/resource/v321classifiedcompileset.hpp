#pragma once

#include <atomic>
#include <memory>
#include <mutex>

#include <osg/Node>
#include <osgUtil/IncrementalCompileOperation>

namespace Resource
{
    enum class V321CompileClass : unsigned char
    {
        Unknown = 0,
        ObjectPaging,
        Terrain,
        GenericModel,
    };

    // P6 residency/deadline metadata. This describes producer intent rather
    // than current visibility.
    enum class V321CompileUrgency : unsigned char
    {
        Background = 0,
        NearFuture,
        Required,
    };

    inline std::atomic_bool& v321CP2FairnessFlag()
    {
        static std::atomic_bool enabled{ false };
        return enabled;
    }

    inline void initializeV321CP2Fairness(bool enabled)
    {
        static std::once_flag once;
        std::call_once(once, [enabled] { v321CP2FairnessFlag().store(enabled, std::memory_order_release); });
    }

    inline bool v321CP2FairnessEnabled()
    {
        return v321CP2FairnessFlag().load(std::memory_order_acquire);
    }

    // Resource-only producers have no scene node whose reference count can
    // express demand. Their owner explicitly retires the job on cancellation,
    // release or teardown; the ICO completion callback retires successful work.
    // Cache maintenance only reads this atomic state, never draw-owned resources.
    class V321ResourceCompileLifetime
    {
    public:
        bool pending() const { return mState.load(std::memory_order_acquire) == State::Pending; }
        bool cancelled() const { return mState.load(std::memory_order_acquire) == State::Cancelled; }
        void cancel() { mState.store(State::Cancelled, std::memory_order_release); }
        void complete()
        {
            State expected = State::Pending;
            mState.compare_exchange_strong(expected, State::Completed, std::memory_order_acq_rel);
        }

    private:
        enum class State : unsigned char { Pending, Cancelled, Completed };
        std::atomic<State> mState{ State::Pending };
    };

    class V321ClassifiedCompileSet final : public osgUtil::IncrementalCompileOperation::CompileSet
    {
    public:
        V321ClassifiedCompileSet(osg::Node* subgraph, V321CompileClass compileClass,
            V321CompileUrgency urgency = V321CompileUrgency::NearFuture)
            : CompileSet(subgraph)
            , mCompileClass(compileClass)
            , mUrgency(urgency)
        {
        }

        V321ClassifiedCompileSet(const std::shared_ptr<V321ResourceCompileLifetime>& owner,
            V321CompileClass compileClass, V321CompileUrgency urgency)
            : CompileSet(nullptr)
            , mCompileClass(compileClass)
            , mUrgency(urgency)
            , mResourceOnly(true)
            , mResourceOwner(owner)
        {
        }

        V321CompileClass compileClass() const { return mCompileClass; }
        V321CompileUrgency urgency() const { return mUrgency; }
        bool resourceOnly() const { return mResourceOnly; }
        bool resourceCompilePending() const
        {
            const auto owner = mResourceOwner.lock();
            return owner && owner->pending();
        }

    protected:
        ~V321ClassifiedCompileSet() override = default;

    private:
        V321CompileClass mCompileClass;
        V321CompileUrgency mUrgency;
        bool mResourceOnly = false;
        std::weak_ptr<V321ResourceCompileLifetime> mResourceOwner;
    };

    inline V321CompileClass getV321CompileClass(
        const osgUtil::IncrementalCompileOperation::CompileSet* compileSet)
    {
        const auto* classified = dynamic_cast<const V321ClassifiedCompileSet*>(compileSet);
        return classified ? classified->compileClass() : V321CompileClass::Unknown;
    }

    inline V321CompileUrgency getV321CompileUrgency(
        const osgUtil::IncrementalCompileOperation::CompileSet* compileSet)
    {
        const auto* classified = dynamic_cast<const V321ClassifiedCompileSet*>(compileSet);
        return classified ? classified->urgency() : V321CompileUrgency::NearFuture;
    }
}
