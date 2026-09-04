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
        V321ClassifiedCompileSet(osg::Node* subgraph, V321CompileClass compileClass)
            : CompileSet(subgraph)
            , mCompileClass(compileClass)
        {
        }

        V321CompileClass compileClass() const { return mCompileClass; }

    protected:
        ~V321ClassifiedCompileSet() override = default;

    private:
        V321CompileClass mCompileClass;
    };

    inline V321CompileClass getV321CompileClass(
        const osgUtil::IncrementalCompileOperation::CompileSet* compileSet)
    {
        const auto* classified = dynamic_cast<const V321ClassifiedCompileSet*>(compileSet);
        return classified ? classified->compileClass() : V321CompileClass::Unknown;
    }
}
