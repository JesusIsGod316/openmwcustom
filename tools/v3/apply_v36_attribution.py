import os
from pathlib import Path


ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.6 attribution match(es), found {count}")
    path.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
    print(f"V3.6 attribution patched {rel} ({count} match(es))")


def write_new(rel, text):
    path = ROOT / rel
    if path.exists():
        raise RuntimeError(f"{rel}: refusing to overwrite an existing file")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"V3.6 attribution added {rel}")


# Lua first-materialization attribution. This is deliberately observational: it neither pre-executes script bodies
# nor changes activation, timer, package, or worker-barrier ordering.
write_new(
    "components/debug/v36luaaddscripttrace.hpp",
    r'''#ifndef OPENMW_COMPONENTS_DEBUG_V36LUAADDSCRIPTTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36LUAADDSCRIPTTRACE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36LuaAddScriptTrace
{
    enum class Phase : std::size_t
    {
        HiddenSetup,
        CachedChunkLoad,
        Environment,
        CommonPackages,
        ContainerPackages,
        RequireSetup,
        ModuleLoad,
        ScriptBody,
        HandlerExtraction,
        InterfaceRegistration,
        Count,
    };

    inline constexpr std::size_t PhaseCount = static_cast<std::size_t>(Phase::Count);

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_LUA_ADDSCRIPT_FILE",
            "frame,epoch_ms,container,script,total_ms,hidden_setup_ms,cached_chunk_load_ms,environment_ms,"
            "common_packages_ms,container_packages_ms,require_setup_ms,module_load_ms,script_body_ms,"
            "handler_extraction_ms,interface_registration_ms,other_ms");
        return writer;
    }

    class Recorder
    {
    public:
        void begin(std::string_view container, std::string_view script)
        {
            mContainer.assign(container);
            mScript.assign(script);
            mStart = V3Diagnostics::Clock::now();
        }

        void add(Phase phase, double milliseconds)
        {
            mPhases[static_cast<std::size_t>(phase)] += milliseconds;
        }

        void finish()
        {
            const double total = V3Diagnostics::elapsedMs(mStart);
            if (total < 0.25)
                return;
            // Module loads occur inside the top-level script call. Keep the CSV phases mutually exclusive.
            const std::size_t moduleIndex = static_cast<std::size_t>(Phase::ModuleLoad);
            const std::size_t bodyIndex = static_cast<std::size_t>(Phase::ScriptBody);
            mPhases[bodyIndex] = std::max(0.0, mPhases[bodyIndex] - mPhases[moduleIndex]);
            double accounted = 0.0;
            for (double value : mPhases)
                accounted += value;
            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
                << V3Diagnostics::csvQuote(mContainer) << ',' << V3Diagnostics::csvQuote(mScript) << ','
                << std::fixed << std::setprecision(4) << total;
            for (double value : mPhases)
                row << ',' << value;
            row << ',' << std::max(0.0, total - accounted);
            writer().writeLine(row.str());
        }

    private:
        std::string mContainer;
        std::string mScript;
        V3Diagnostics::Clock::time_point mStart{};
        std::array<double, PhaseCount> mPhases{};
    };

    inline thread_local Recorder* sRecorder = nullptr;

    class ScriptScope
    {
    public:
        ScriptScope(std::string_view container, std::string_view script)
            : mPrevious(sRecorder)
            , mActive(writer().enabled())
        {
            if (mActive)
            {
                sRecorder = &mRecorder;
                mRecorder.begin(container, script);
            }
        }

        ~ScriptScope()
        {
            if (mActive)
            {
                mRecorder.finish();
                sRecorder = mPrevious;
            }
        }

        ScriptScope(const ScriptScope&) = delete;
        ScriptScope& operator=(const ScriptScope&) = delete;

    private:
        Recorder mRecorder;
        Recorder* mPrevious;
        bool mActive;
    };

    class PhaseScope
    {
    public:
        explicit PhaseScope(Phase phase)
            : mRecorder(sRecorder)
            , mPhase(phase)
            , mStart(mRecorder ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }

        ~PhaseScope()
        {
            if (mRecorder)
                mRecorder->add(mPhase, V3Diagnostics::elapsedMs(mStart));
        }

        PhaseScope(const PhaseScope&) = delete;
        PhaseScope& operator=(const PhaseScope&) = delete;

    private:
        Recorder* mRecorder;
        Phase mPhase;
        V3Diagnostics::Clock::time_point mStart;
    };

    inline void add(Phase phase, V3Diagnostics::Clock::time_point start)
    {
        if (sRecorder)
            sRecorder->add(phase, V3Diagnostics::elapsedMs(start));
    }
}

#endif
''',
)

replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''#include <components/debug/v35lualoadtrace.hpp>''',
    '''#include <components/debug/v35lualoadtrace.hpp>
#include <components/debug/v36luaaddscripttrace.hpp>''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''        debugName.append(path);
        debugName.push_back(']');

        Script& script = data.mScripts[scriptId];''',
    '''        debugName.append(path);
        debugName.push_back(']');

        Debug::V36LuaAddScriptTrace::ScriptScope v36ScriptTrace(mNamePrefix, path.value());
        const auto v36HiddenStart = Debug::V3Diagnostics::Clock::now();
        Script& script = data.mScripts[scriptId];''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''        else
            script.mStats.mMemoryUsage = 0;

        try''',
    '''        else
            script.mStats.mMemoryUsage = 0;
        Debug::V36LuaAddScriptTrace::add(Debug::V36LuaAddScriptTrace::Phase::HiddenSetup, v36HiddenStart);

        try''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''            if (scriptOutput == sol::nil)
                return true;
            sol::object engineHandlers = sol::nil, eventHandlers = sol::nil;''',
    '''            if (scriptOutput == sol::nil)
                return true;
            const auto v36HandlersStart = Debug::V3Diagnostics::Clock::now();
            sol::object engineHandlers = sol::nil, eventHandlers = sol::nil;''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''            if (script.mInterfaceName.empty() == script.mInterface.has_value())''',
    '''            Debug::V36LuaAddScriptTrace::add(
                Debug::V36LuaAddScriptTrace::Phase::HandlerExtraction, v36HandlersStart);
            const auto v36InterfaceStart = Debug::V3Diagnostics::Clock::now();
            if (script.mInterfaceName.empty() == script.mInterface.has_value())''',
)
replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''                insertInterface(scriptId, script);
            }

            return true;''',
    '''                insertInterface(scriptId, script);
            }
            Debug::V36LuaAddScriptTrace::add(
                Debug::V36LuaAddScriptTrace::Phase::InterfaceRegistration, v36InterfaceStart);

            return true;''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v36luaaddscripttrace.hpp>''',
)
replace_exact(
    "components/lua/luastate.cpp",
    '''            sol["loadFromVFS"] = [this](std::string_view packageName) {
                return loadScriptAndCache(packageNameToVfsPath(packageName, *mVFS));
            };''',
    '''            sol["loadFromVFS"] = [this](std::string_view packageName) {
                Debug::V36LuaAddScriptTrace::PhaseScope v36ModuleLoad(
                    Debug::V36LuaAddScriptTrace::Phase::ModuleLoad);
                return loadScriptAndCache(packageNameToVfsPath(packageName, *mVFS));
            };''',
)
replace_exact(
    "components/lua/luastate.cpp",
    '''        // TODO
        sol::protected_function script = loadScriptAndCache(path);

        sol::environment env(mSol, sol::create, mSandboxEnv);
        env["print"] = mSol["printGen"](envName + ":");
        env["_G"] = env;
        env[sol::metatable_key]["__metatable"] = false;''',
    '''        sol::protected_function script;
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36CachedChunk(
                Debug::V36LuaAddScriptTrace::Phase::CachedChunkLoad);
            script = loadScriptAndCache(path);
        }

        const auto v36EnvironmentStart = Debug::V3Diagnostics::Clock::now();
        sol::environment env(mSol, sol::create, mSandboxEnv);
        env["print"] = mSol["printGen"](envName + ":");
        env["_G"] = env;
        env[sol::metatable_key]["__metatable"] = false;
        Debug::V36LuaAddScriptTrace::add(
            Debug::V36LuaAddScriptTrace::Phase::Environment, v36EnvironmentStart);''',
)
replace_exact(
    "components/lua/luastate.cpp",
    '''        sol::table loaded(mSol, sol::create);
        for (const auto& [key, value] : mCommonPackages)
            loaded[key] = maybeRunLoader(value);
        for (const auto& [key, value] : packages)
            loaded[key] = maybeRunLoader(value);
        env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"]);

        sol::set_environment(env, script);
        return call(scriptId, script);''',
    '''        sol::table loaded(mSol, sol::create);
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36CommonPackages(
                Debug::V36LuaAddScriptTrace::Phase::CommonPackages);
            for (const auto& [key, value] : mCommonPackages)
                loaded[key] = maybeRunLoader(value);
        }
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36ContainerPackages(
                Debug::V36LuaAddScriptTrace::Phase::ContainerPackages);
            for (const auto& [key, value] : packages)
                loaded[key] = maybeRunLoader(value);
        }
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36RequireSetup(
                Debug::V36LuaAddScriptTrace::Phase::RequireSetup);
            env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"]);
            sol::set_environment(env, script);
        }
        Debug::V36LuaAddScriptTrace::PhaseScope v36ScriptBody(
            Debug::V36LuaAddScriptTrace::Phase::ScriptBody);
        return call(scriptId, script);''',
)

# Detailed controller construction attribution. Existing KeyframeManager storage is already the immutable metadata
# cache; this pass measures per-instance mutable cloning/assignment before attempting any unsafe sharing.
write_new(
    "components/debug/v36controllertrace.hpp",
    r'''#ifndef OPENMW_COMPONENTS_DEBUG_V36CONTROLLERTRACE_H
#define OPENMW_COMPONENTS_DEBUG_V36CONTROLLERTRACE_H

#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <string_view>

#include "v3diagnostics.hpp"
#include "v3hitchtelemetry.hpp"

namespace Debug::V36ControllerTrace
{
    enum class Phase { KeyframeLookup, NodeMap, ControllerClone, SourceAssign };

    inline V3Diagnostics::CsvWriter& writer()
    {
        static V3Diagnostics::CsvWriter writer("OPENMW_V36_CONTROLLER_FILE",
            "frame,epoch_ms,keyframe,base_model,total_ms,keyframe_lookup_ms,node_map_ms,controller_clone_ms,"
            "source_assign_ms,other_ms,controller_count,controller_types");
        return writer;
    }

    class Scope
    {
    public:
        Scope(std::string_view keyframe, std::string_view baseModel)
            : mEnabled(writer().enabled())
            , mKeyframe(mEnabled ? keyframe : std::string_view{})
            , mBaseModel(mEnabled ? baseModel : std::string_view{})
            , mStart(mEnabled ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }

        ~Scope()
        {
            if (!mEnabled)
                return;
            const double total = V3Diagnostics::elapsedMs(mStart);
            if (total < 0.25)
                return;
            const double accounted = mKeyframeMs + mNodeMapMs + mCloneMs + mAssignMs;
            std::ostringstream types;
            bool first = true;
            for (const auto& [name, count] : mTypes)
            {
                if (!first)
                    types << ';';
                first = false;
                types << name << ':' << count;
            }
            std::ostringstream row;
            row << V3HitchTelemetry::currentFrame() << ',' << V3Diagnostics::epochMs() << ','
                << V3Diagnostics::csvQuote(mKeyframe) << ',' << V3Diagnostics::csvQuote(mBaseModel) << ','
                << std::fixed << std::setprecision(4) << total << ',' << mKeyframeMs << ',' << mNodeMapMs << ','
                << mCloneMs << ',' << mAssignMs << ',' << std::max(0.0, total - accounted) << ','
                << mControllerCount << ',' << V3Diagnostics::csvQuote(types.str());
            writer().writeLine(row.str());
        }

        void add(Phase phase, double ms)
        {
            switch (phase)
            {
                case Phase::KeyframeLookup: mKeyframeMs += ms; break;
                case Phase::NodeMap: mNodeMapMs += ms; break;
                case Phase::ControllerClone: mCloneMs += ms; break;
                case Phase::SourceAssign: mAssignMs += ms; break;
            }
        }

        void controller(std::string_view type)
        {
            if (!mEnabled)
                return;
            ++mControllerCount;
            ++mTypes[std::string(type)];
        }

        bool enabled() const { return mEnabled; }

    private:
        bool mEnabled;
        std::string mKeyframe;
        std::string mBaseModel;
        V3Diagnostics::Clock::time_point mStart;
        double mKeyframeMs = 0.0;
        double mNodeMapMs = 0.0;
        double mCloneMs = 0.0;
        double mAssignMs = 0.0;
        unsigned int mControllerCount = 0;
        std::map<std::string, unsigned int> mTypes;
    };

    class PhaseScope
    {
    public:
        PhaseScope(Scope& scope, Phase phase)
            : mScope(scope)
            , mPhase(phase)
            , mStart(scope.enabled() ? V3Diagnostics::Clock::now() : V3Diagnostics::Clock::time_point{})
        {
        }
        ~PhaseScope()
        {
            if (mStart != V3Diagnostics::Clock::time_point{})
                mScope.add(mPhase, V3Diagnostics::elapsedMs(mStart));
        }
    private:
        Scope& mScope;
        Phase mPhase;
        V3Diagnostics::Clock::time_point mStart;
    };
}

#endif
''',
)

replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''#include <components/debug/v32rendererprofiling.hpp>''',
    '''#include <components/debug/v32rendererprofiling.hpp>
#include <components/debug/v36controllertrace.hpp>''',
)
replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''        VFS::Path::Normalized kfname(model);

        if (kfname.extension() == nif)''',
    '''        VFS::Path::Normalized kfname(model);

        if (kfname.extension() == nif)''',
)
replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''        if (!mResourceSystem->getVFS()->exists(kfname))
            return nullptr;

        osg::ref_ptr<const SceneUtil::KeyframeHolder> keyframes = mResourceSystem->getKeyframeManager()->get(kfname);''',
    '''        Debug::V36ControllerTrace::Scope v36ControllerTrace(kfname.value(), baseModel);
        if (!mResourceSystem->getVFS()->exists(kfname))
            return nullptr;

        osg::ref_ptr<const SceneUtil::KeyframeHolder> keyframes;
        {
            Debug::V36ControllerTrace::PhaseScope v36Keyframes(
                v36ControllerTrace, Debug::V36ControllerTrace::Phase::KeyframeLookup);
            keyframes = mResourceSystem->getKeyframeManager()->get(kfname);
        }''',
)
replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''        const NodeMap& nodeMap = getNodeMap();
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
             it != controllerMap.end(); ++it)
        {''',
    '''        const NodeMap* nodeMapPtr = nullptr;
        {
            Debug::V36ControllerTrace::PhaseScope v36NodeMap(
                v36ControllerTrace, Debug::V36ControllerTrace::Phase::NodeMap);
            nodeMapPtr = &getNodeMap();
        }
        const NodeMap& nodeMap = *nodeMapPtr;
        const auto& controllerMap = animsrc->mKeyframes->mKeyframeControllers;
        {
            Debug::V36ControllerTrace::PhaseScope v36ControllerClone(
                v36ControllerTrace, Debug::V36ControllerTrace::Phase::ControllerClone);
            for (SceneUtil::KeyframeHolder::KeyframeControllerMap::const_iterator it = controllerMap.begin();
                 it != controllerMap.end(); ++it)
            {''',
)
replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''            animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
        }

        mAnimSources.push_back(animsrc);''',
    '''                animsrc->mControllerMap[blendMask].insert(std::make_pair(bonename, cloned));
                v36ControllerTrace.controller(it->second->className());
            }
        }

        mAnimSources.push_back(animsrc);''',
)
replace_exact(
    "apps/openmw/mwrender/animation.cpp",
    '''        SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
        mObjectRoot->accept(assignVisitor);''',
    '''        {
            Debug::V36ControllerTrace::PhaseScope v36SourceAssign(
                v36ControllerTrace, Debug::V36ControllerTrace::Phase::SourceAssign);
            SceneUtil::AssignControllerSourcesVisitor assignVisitor(mAnimationTimePtr[0]);
            mObjectRoot->accept(assignVisitor);
        }''',
)

# Cached-image/source-memory attribution. This does not claim that source bytes equal driver allocation and does not
# evict resources; it establishes the largest recoverable assets and cache-recency data needed for a future policy.
replace_exact(
    "components/resource/objectcache.hpp",
    '''        template <class Functor>
        void call(Functor&& f)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                f(k, v.mValue.get());
        }''',
    '''        template <class Functor>
        void call(Functor&& f)
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                f(k, v.mValue.get());
        }

        template <class Functor>
        void callWithUsage(Functor&& f) const
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (const auto& [k, v] : mItems)
                f(k, v.mValue.get(), v.mLastUsage);
        }''',
)
replace_exact(
    "components/resource/imagemanager.cpp",
    '''#include <components/debug/debuglog.hpp>''',
    '''#include <components/debug/debuglog.hpp>
#include <components/debug/v3diagnostics.hpp>''',
)
replace_exact(
    "components/resource/imagemanager.cpp",
    '''    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);
    }''',
    '''    void ImageManager::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Image", frameNumber, mCache->getStats(), *stats);

        static Debug::V3Diagnostics::CsvWriter v36Writer("OPENMW_V36_RESIDENCY_FILE",
            "frame,epoch_ms,event,path,estimated_source_mb,width,height,mip_levels,compressed,ref_count,"
            "cache_entries,last_usage,total_estimated_source_mb");
        if (!v36Writer.enabled() || frameNumber % 300 != 0)
            return;

        struct Entry
        {
            std::string mPath;
            unsigned int mBytes = 0;
            int mWidth = 0;
            int mHeight = 0;
            unsigned int mMipLevels = 0;
            bool mCompressed = false;
            int mRefCount = 0;
            double mLastUsage = 0.0;
        };
        std::vector<Entry> entries;
        std::uint64_t totalBytes = 0;
        mCache->callWithUsage([&](const auto& key, osg::Object* object, double lastUsage) {
            const osg::Image* image = dynamic_cast<const osg::Image*>(object);
            if (!image)
                return;
            const unsigned int bytes = image->getTotalSizeInBytesIncludingMipmaps();
            totalBytes += bytes;
            entries.push_back({ key, bytes, image->s(), image->t(), image->getNumMipmapLevels(),
                image->isCompressed(), image->referenceCount(), lastUsage });
        });
        std::ranges::sort(entries, [](const Entry& left, const Entry& right) { return left.mBytes > right.mBytes; });
        constexpr double MiB = 1024.0 * 1024.0;
        const double totalMb = static_cast<double>(totalBytes) / MiB;
        std::ostringstream summary;
        summary << frameNumber << ',' << Debug::V3Diagnostics::epochMs() << ",summary,"
                << Debug::V3Diagnostics::csvQuote("") << ',' << std::fixed << std::setprecision(3)
                << "0,0,0,0,0,0," << entries.size() << ",0," << totalMb;
        v36Writer.writeLine(summary.str());
        const std::size_t limit = std::min<std::size_t>(entries.size(), 32);
        for (std::size_t i = 0; i < limit; ++i)
        {
            const Entry& entry = entries[i];
            std::ostringstream row;
            row << frameNumber << ',' << Debug::V3Diagnostics::epochMs() << ",largest,"
                << Debug::V3Diagnostics::csvQuote(entry.mPath) << ',' << std::fixed << std::setprecision(3)
                << (static_cast<double>(entry.mBytes) / MiB) << ',' << entry.mWidth << ',' << entry.mHeight << ','
                << entry.mMipLevels << ',' << (entry.mCompressed ? 1 : 0) << ',' << entry.mRefCount << ','
                << entries.size() << ',' << entry.mLastUsage << ',' << totalMb;
            v36Writer.writeLine(row.str());
        }
    }''',
)
replace_exact(
    "components/resource/imagemanager.cpp",
    '''#include <cassert>
#include <osgDB/Registry>''',
    '''#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iomanip>
#include <ranges>
#include <sstream>
#include <vector>
#include <osgDB/Registry>''',
)

print("V3.6 Lua, controller, and residency attribution source patch completed successfully.")
