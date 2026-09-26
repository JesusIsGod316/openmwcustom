#pragma once

#include <atomic>
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

        V321CompileClass compileClass() const { return mCompileClass; }
        V321CompileUrgency urgency() const { return mUrgency; }

    protected:
        ~V321ClassifiedCompileSet() override = default;

    private:
        V321CompileClass mCompileClass;
        V321CompileUrgency mUrgency;
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
