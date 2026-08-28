import os
import re
from pathlib import Path

ROOT = Path(os.environ.get("OPENMW_PATCH_ROOT", Path(__file__).resolve().parents[2])).resolve()


def replace_exact(rel, old, new, expected=1):
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} V3.14 match(es), found {count}")
    path.write_text(text.replace(old, new, expected), encoding="utf-8", newline="\n")
    print(f"V3.14 patched {rel} ({count} match(es))")


# V3.14 switch surface.
replace_exact(
    "components/settings/categories/cells.hpp",
    '''        SettingValue<int> mV313ChunkQualityMode{ mIndex, "V3", "v3.13 chunk quality mode",
            makeClampSanitizerInt(0, 2) };''',
    '''        SettingValue<int> mV313ChunkQualityMode{ mIndex, "V3", "v3.13 chunk quality mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<int> mV314LuaDependencyPrecompileMode{ mIndex, "V3", "v3.14 lua dependency precompile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314LuaPackagePrototypeReuse{ mIndex, "V3", "v3.14 lua package prototype reuse" };
        SettingValue<int> mV314GroundcoverCompileMode{ mIndex, "V3", "v3.14 groundcover compile mode",
            makeClampSanitizerInt(0, 2) };
        SettingValue<bool> mV314PostfxCompileWarmup{ mIndex, "V3", "v3.14 postfx compile warmup" };''',
)

replace_exact(
    "files/settings-default.cfg",
    '''v3.13 chunk quality mode = 0

[Cells]''',
    '''v3.13 chunk quality mode = 0

# V3.14 first-use and render preparation layer.
# Lua dependency precompile: 0=off, 1=literal direct requires, 2=recursive literal requires.
v3.14 lua dependency precompile mode = 0
# Reuse one immutable static-package prototype per ScriptsContainer sandbox family.
v3.14 lua package prototype reuse = false
# Groundcover ICO compile: 0=inherited, 1=only compile=true preload chunks, 2=all newly-created chunks.
v3.14 groundcover compile mode = 0
# Feed active PostFX pass state/programs through the existing IncrementalCompileOperation.
v3.14 postfx compile warmup = false

[Cells]''',
)

# Lua first-activation work reduction.
replace_exact(
    "components/lua/luastate.hpp",
    '''        // V3.12: compile configured top-level scripts into the existing bytecode
        // cache without creating a sandbox or executing script code.
        std::size_t precompileConfiguredScripts();

        const ScriptsConfiguration& getConfiguration() const { return *mConf; }''',
    '''        // V3.12: compile configured top-level scripts into the existing bytecode
        // cache without creating a sandbox or executing script code.
        std::size_t precompileConfiguredScripts();

        // V3.14: populate the same bytecode cache for literal require() dependencies.
        // Mode1 scans configured scripts, mode2 recursively scans discovered modules.
        std::size_t precompileConfiguredDependencies(int mode);

        // V3.14: static packages are immutable read-only objects. A container can reuse
        // one prototype table rather than inserting the same package objects for every sandbox.
        sol::main_table makePackagePrototype(const std::map<std::string, sol::main_object>& packages);
        void setPackagePrototypeReuse(bool enabled) { mPackagePrototypeReuse = enabled; }
        bool getPackagePrototypeReuse() const { return mPackagePrototypeReuse; }

        const ScriptsConfiguration& getConfiguration() const { return *mConf; }''',
)

replace_exact(
    "components/lua/luastate.hpp",
    '''        sol::protected_function_result runInNewSandbox(const VFS::Path::Normalized& path,
            const std::string& envName = "unnamed", const std::map<std::string, sol::main_object>& packages = {},
            const sol::main_object& hiddenData = sol::nil);''',
    '''        sol::protected_function_result runInNewSandbox(const VFS::Path::Normalized& path,
            const std::string& envName = "unnamed", const std::map<std::string, sol::main_object>& packages = {},
            const sol::main_object& hiddenData = sol::nil, const sol::main_table* packagePrototype = nullptr);''',
)

replace_exact(
    "components/lua/luastate.hpp",
    '''        const VFS::Manager* mVFS;
''',
    '''        const VFS::Manager* mVFS;
        bool mPackagePrototypeReuse = false;
''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''#include <filesystem>
#include <fstream>''',
    '''#include <filesystem>
#include <fstream>
#include <regex>
#include <set>''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''    sol::function LuaState::loadScriptAndCache(const VFS::Path::Normalized& path)
    {
        auto iter = mCompiledScripts.find(path);''',
    r'''    std::size_t LuaState::precompileConfiguredDependencies(int mode)
    {
        if (mode <= 0)
            return 0;

        std::size_t compiled = 0;
        protectedCall([&](LuaView&) {
            static const std::regex requirePattern(
                R"v314(\brequire\s*(?:\(\s*)?[\"']([^\"']+)[\"']\s*\)?)v314");

            std::vector<VFS::Path::Normalized> scanQueue;
            std::set<VFS::Path::Normalized> queued;
            for (std::size_t i = 0; i < mConf->size(); ++i)
            {
                const VFS::Path::Normalized& path = (*mConf)[i].mScriptPath;
                if (queued.insert(path).second)
                    scanQueue.push_back(path);
            }

            constexpr std::size_t MaxScannedModules = 4096;
            for (std::size_t index = 0; index < scanQueue.size() && index < MaxScannedModules; ++index)
            {
                const VFS::Path::Normalized path = scanQueue[index];
                try
                {
                    std::string source(std::istreambuf_iterator<char>(*mVFS->get(path)), {});
                    for (std::sregex_iterator it(source.begin(), source.end(), requirePattern), end; it != end; ++it)
                    {
                        const std::string packageName = (*it)[1].str();
                        try
                        {
                            const VFS::Path::Normalized dependency = packageNameToVfsPath(packageName, *mVFS);
                            const bool newlyQueued = queued.insert(dependency).second;
                            if (!mCompiledScripts.contains(dependency))
                            {
                                sol::function function = loadFromVFS(dependency);
                                mCompiledScripts[dependency] = function.dump();
                                ++compiled;
                            }
                            if (mode >= 2 && newlyQueued && scanQueue.size() < MaxScannedModules)
                                scanQueue.push_back(dependency);
                        }
                        catch (const std::exception&)
                        {
                            // Comments/dead branches/optional package text are intentionally nonfatal.
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    Log(Debug::Warning) << "V3.14 Lua dependency scan skipped " << path << ": " << e.what();
                }
            }
        });
        return compiled;
    }

    sol::main_table LuaState::makePackagePrototype(const std::map<std::string, sol::main_object>& packages)
    {
        sol::table prototype(mSol, sol::create);
        for (const auto& [key, value] : mCommonPackages)
        {
            if (!value.is<sol::function>())
                prototype[key] = value;
        }
        for (const auto& [key, value] : packages)
        {
            if (!value.is<sol::function>())
                prototype[key] = value;
        }
        return sol::main_table(prototype);
    }

    sol::function LuaState::loadScriptAndCache(const VFS::Path::Normalized& path)
    {
        auto iter = mCompiledScripts.find(path);''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''    sol::protected_function_result LuaState::runInNewSandbox(const VFS::Path::Normalized& path,
        const std::string& envName, const std::map<std::string, sol::main_object>& packages,
        const sol::main_object& hiddenData)''',
    '''    sol::protected_function_result LuaState::runInNewSandbox(const VFS::Path::Normalized& path,
        const std::string& envName, const std::map<std::string, sol::main_object>& packages,
        const sol::main_object& hiddenData, const sol::main_table* packagePrototype)''',
)

replace_exact(
    "components/lua/luastate.cpp",
    '''        sol::table loaded(mSol, sol::create);
        for (const auto& [key, value] : mCommonPackages)
            loaded[key] = maybeRunLoader(value);
        for (const auto& [key, value] : packages)
            loaded[key] = maybeRunLoader(value);
        env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"]);''',
    '''        sol::table loaded(mSol, sol::create);
        if (packagePrototype)
        {
            sol::table meta(mSol, sol::create);
            meta[sol::meta_method::index] = *packagePrototype;
            loaded[sol::metatable_key] = meta;
            for (const auto& [key, value] : mCommonPackages)
                if (value.is<sol::function>())
                    loaded[key] = maybeRunLoader(value);
            for (const auto& [key, value] : packages)
                if (value.is<sol::function>())
                    loaded[key] = maybeRunLoader(value);
        }
        else
        {
            for (const auto& [key, value] : mCommonPackages)
                loaded[key] = maybeRunLoader(value);
            for (const auto& [key, value] : packages)
                loaded[key] = maybeRunLoader(value);
        }
        env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"]);''',
)

replace_exact(
    "components/lua/scriptscontainer.hpp",
    '''        std::map<std::string, sol::main_object> mAPI;
        struct LoadedData''',
    '''        std::map<std::string, sol::main_object> mAPI;
        std::optional<sol::main_table> mV314PackagePrototype;
        struct LoadedData''',
)

replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''        if (!package.is<sol::userdata>())
            throw std::logic_error("Expected package to be read-only: " + packageName);
        mAPI.insert_or_assign(std::move(packageName), std::move(package));''',
    '''        if (!package.is<sol::userdata>())
            throw std::logic_error("Expected package to be read-only: " + packageName);
        mAPI.insert_or_assign(std::move(packageName), std::move(package));
        mV314PackagePrototype.reset();''',
)

replace_exact(
    "components/lua/scriptscontainer.cpp",
    '''            sol::object scriptOutput = mLua.runInNewSandbox(path, debugName, mAPI, script.mHiddenData);''',
    '''            const sol::main_table* packagePrototype = nullptr;
            if (mLua.getPackagePrototypeReuse())
            {
                if (!mV314PackagePrototype)
                    mV314PackagePrototype.emplace(mLua.makePackagePrototype(mAPI));
                packagePrototype = &*mV314PackagePrototype;
            }
            sol::object scriptOutput
                = mLua.runInNewSandbox(path, debugName, mAPI, script.mHiddenData, packagePrototype);''',
)

replace_exact(
    "apps/openmw/mwlua/luamanagerimp.cpp",
    '''        initConfiguration(false);
        if (static_cast<bool>(Settings::cells().mV312LuaPrecompile))
        {
            const std::size_t compiled = mLua.precompileConfiguredScripts();
            Log(Debug::Info) << "V3.12 Lua precompiled " << compiled << " configured script(s)";
        }
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());''',
    '''        initConfiguration(false);
        mLua.setPackagePrototypeReuse(static_cast<bool>(Settings::cells().mV314LuaPackagePrototypeReuse));
        if (static_cast<bool>(Settings::cells().mV312LuaPrecompile))
        {
            const std::size_t compiled = mLua.precompileConfiguredScripts();
            Log(Debug::Info) << "V3.12 Lua precompiled " << compiled << " configured script(s)";
        }
        const int v314DependencyMode = static_cast<int>(Settings::cells().mV314LuaDependencyPrecompileMode);
        if (v314DependencyMode > 0)
        {
            const std::size_t compiledDependencies = mLua.precompileConfiguredDependencies(v314DependencyMode);
            Log(Debug::Info) << "V3.14 Lua precompiled " << compiledDependencies
                             << " dependency module(s), mode " << v314DependencyMode;
        }
        mLoadScripts.setAutoStartConf(mConfiguration.getLoadConf());''',
)

# Groundcover already uses hardware instancing; add missing ICO preparation.
replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''#define OPENMW_MWRENDER_GROUNDCOVER_H

#include <components/esm3/loadcell.hpp>''',
    '''#define OPENMW_MWRENDER_GROUNDCOVER_H

#include <atomic>

#include <components/esm3/loadcell.hpp>''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.hpp",
    '''        const MWWorld::GroundcoverStore& mGroundcoverStore;

        osg::ref_ptr<osg::Node> createChunk''',
    '''        const MWWorld::GroundcoverStore& mGroundcoverStore;
        std::atomic_uint64_t mV314CompileQueued{ 0 };

        osg::ref_ptr<osg::Node> createChunk''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''#include <osgUtil/CullVisitor>''',
    '''#include <osgUtil/CullVisitor>
#include <osgUtil/GLObjectsVisitor>
#include <osgUtil/IncrementalCompileOperation>''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''            osg::ref_ptr<osg::Node> node = createChunk(instances, center);
            mCache->addEntryToObjectCache(id, node.get());
            return node;''',
    '''            osg::ref_ptr<osg::Node> node = createChunk(instances, center);

            const int v314CompileMode = static_cast<int>(Settings::cells().mV314GroundcoverCompileMode);
            const bool v314ShouldCompile = v314CompileMode >= 2 || (v314CompileMode == 1 && compile);
            osgUtil::IncrementalCompileOperation* const ico = mSceneManager->getIncrementalCompileOperation();
            if (v314ShouldCompile && ico)
            {
                osgUtil::GLObjectsVisitor stateToCompile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES
                    | osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS);
                node->accept(stateToCompile);
                if (!stateToCompile.empty())
                {
                    auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(node);
                    compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                    ico->add(compileSet, false);
                    mV314CompileQueued.fetch_add(1, std::memory_order_relaxed);
                }
            }

            mCache->addEntryToObjectCache(id, node.get());
            return node;''',
)

replace_exact(
    "apps/openmw/mwrender/groundcover.cpp",
    '''    void Groundcover::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Groundcover Chunk", frameNumber, mCache->getStats(), *stats);
    }''',
    '''    void Groundcover::reportStats(unsigned int frameNumber, osg::Stats* stats) const
    {
        Resource::reportStats("Groundcover Chunk", frameNumber, mCache->getStats(), *stats);
        stats->setAttribute(frameNumber, "V3.14 Groundcover Compile Queued",
            static_cast<double>(mV314CompileQueued.load(std::memory_order_relaxed)));
    }''',
)

# PostFX: warm active pass state/programs via ICO without changing Rafael shader math.
replace_exact(
    "apps/openmw/mwrender/postprocessor.cpp",
    '''#include <osg/Texture3D>
''',
    '''#include <osg/Texture3D>

#include <osgUtil/GLObjectsVisitor>
#include <osgUtil/IncrementalCompileOperation>
''',
)

replace_exact(
    "apps/openmw/mwrender/postprocessor.cpp",
    '''        mCanvases[frameId]->setPasses(Fx::DispatchArray(mTemplateData));

        if (auto hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())''',
    '''        mCanvases[frameId]->setPasses(Fx::DispatchArray(mTemplateData));

        if (static_cast<bool>(Settings::cells().mV314PostfxCompileWarmup))
        {
            osgUtil::IncrementalCompileOperation* const ico = mRendering.getIncrementalCompileOperation();
            if (ico)
            {
                osg::ref_ptr<osg::Group> compileRoot = new osg::Group;
                for (const auto& dispatch : mTemplateData)
                {
                    osg::ref_ptr<osg::Group> techniqueRoot = new osg::Group;
                    techniqueRoot->setStateSet(dispatch.mRootStateSet);
                    compileRoot->addChild(techniqueRoot);
                    for (const auto& subPass : dispatch.mPasses)
                    {
                        osg::ref_ptr<osg::Group> passNode = new osg::Group;
                        passNode->setStateSet(subPass.mStateSet);
                        techniqueRoot->addChild(passNode);
                    }
                }

                osgUtil::GLObjectsVisitor stateToCompile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES);
                compileRoot->accept(stateToCompile);
                if (!stateToCompile.empty())
                {
                    auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(compileRoot);
                    compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                    ico->add(compileSet, false);
                    Log(Debug::Info) << "V3.14 queued active PostFX chain for ICO compile warmup";
                }
            }
        }

        if (auto hud = MWBase::Environment::get().getWindowManager()->getPostProcessorHud())''',
)

# Launcher modes: 77 exact Mode75 control; 78 balanced; 79 aggressive prep.
launcher = ROOT / "tools/v3/launchers/V3_Lab.ps1"
text = launcher.read_text(encoding="utf-8")

old = "$V313ChunkQualityMode = '0'\n$RendererProfiling"
new = """$V313ChunkQualityMode = '0'
$V314LuaDependencyPrecompileMode = '0'
$V314LuaPackagePrototypeReuse = 'false'
$V314GroundcoverCompileMode = '0'
$V314PostfxCompileWarmup = 'false'
$RendererProfiling"""
if text.count(old) != 1:
    raise RuntimeError("V3.14 launcher defaults anchor mismatch")
text = text.replace(old, new, 1)

old_menu = "Write-Host ' 76 = V3.13 strict quality signature + Lua + spatial experiment'"
new_menu = """Write-Host ' 76 = V3.13 strict quality signature + Lua + spatial experiment'
Write-Host ' 77 = V3.14 exact promoted V3.13 Mode75 control'
Write-Host ' 78 = V3.14 balanced first-use preparation (FIRST TEST)'
Write-Host ' 79 = V3.14 aggressive recursive/groundcover preparation'"""
if text.count(old_menu) != 1:
    raise RuntimeError("V3.14 launcher menu anchor mismatch")
text = text.replace(old_menu, new_menu, 1)

text, n = re.subn(
    r"do \{ \$choice = Read-Host 'Enter 1 through 76' \} until \(\$choice -in @\(([^\n]+)\)\)",
    lambda m: "do { $choice = Read-Host 'Enter 1 through 79' } until ($choice -in @(" + m.group(1) + ",'77','78','79'))",
    text,
    count=1,
)
if n != 1:
    raise RuntimeError("V3.14 launcher choice-range anchor mismatch")

mode76 = re.search(r"(?m)^    '76' \{[^\n]+\}\n\}", text)
if not mode76:
    raise RuntimeError("V3.14 launcher Mode76 anchor not found")
foundation = "$V36PerformanceProfile = 'true'; $V37ActiveEventFastPath = 'true'; $V37RelaxedResourceSweep = 'true'; $V37GpuMemoryManagement = 'true'; $V38WorldBatchingMode = '2'; $V38GpuResidencyMode = '1'; $V38FarShadowMode = '3'; $V38CompilePacingMode = '3'; $V39FrontloadMode = '2'; $V39BatchOptimizerMode = '1'; $V310FreshInitialObjectPaging = 'true'; $V311ActiveGridPrepareMode = '2'; $V312LuaPrecompile = 'true'; $V313ChunkQualityMode = '1'"
addition = f"""{mode76.group(0)[:-2]}
    '77' {{ $Experiment = 'v314-mode75-control'; {foundation} }}
    '78' {{ $Experiment = 'v314-balanced'; {foundation}; $V314LuaDependencyPrecompileMode = '1'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '1'; $V314PostfxCompileWarmup = 'true' }}
    '79' {{ $Experiment = 'v314-aggressive-prep'; {foundation}; $V314LuaDependencyPrecompileMode = '2'; $V314LuaPackagePrototypeReuse = 'true'; $V314GroundcoverCompileMode = '2'; $V314PostfxCompileWarmup = 'true' }}
}}"""
text = text[:mode76.start()] + addition + text[mode76.end():]

old = '    "v313_chunk_quality_mode=$V313ChunkQualityMode",\n    "shadow_distance=$ShadowDistance",'
new = '''    "v313_chunk_quality_mode=$V313ChunkQualityMode",
    "v314_lua_dependency_precompile_mode=$V314LuaDependencyPrecompileMode",
    "v314_lua_package_prototype_reuse=$V314LuaPackagePrototypeReuse",
    "v314_groundcover_compile_mode=$V314GroundcoverCompileMode",
    "v314_postfx_compile_warmup=$V314PostfxCompileWarmup",
    "shadow_distance=$ShadowDistance",'''
if text.count(old) != 1:
    raise RuntimeError("V3.14 launcher run-metadata anchor mismatch")
text = text.replace(old, new, 1)

old = "    Set-IniValue $SettingsPath 'V3' 'v3.13 chunk quality mode' $V313ChunkQualityMode\n    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"
new = """    Set-IniValue $SettingsPath 'V3' 'v3.13 chunk quality mode' $V313ChunkQualityMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 lua dependency precompile mode' $V314LuaDependencyPrecompileMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 lua package prototype reuse' $V314LuaPackagePrototypeReuse
    Set-IniValue $SettingsPath 'V3' 'v3.14 groundcover compile mode' $V314GroundcoverCompileMode
    Set-IniValue $SettingsPath 'V3' 'v3.14 postfx compile warmup' $V314PostfxCompileWarmup
    Set-IniValue $SettingsPath 'Lua' 'v3.3 idle timer fast path' $LuaIdleTimerFastPath"""
if text.count(old) != 1:
    raise RuntimeError("V3.14 launcher settings-write anchor mismatch")
text = text.replace(old, new, 1)
launcher.write_text(text, encoding="utf-8", newline="\n")
print("V3.14 launcher matrix 77-79 patched successfully")

print("V3.14 Lua/GPU first-use efficiency layer completed successfully.")
