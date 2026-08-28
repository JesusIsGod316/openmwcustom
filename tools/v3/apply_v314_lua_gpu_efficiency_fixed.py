from pathlib import Path

# Compatibility wrapper for the mature generated V3 stack. V3.6 attribution already
# instruments LuaState::runInNewSandbox, so rewrite only V3.14's fragile upstream
# package-setup replacement before executing the V3.14 layer. All V3.6 phase scopes
# remain intact.
base = Path(__file__).with_name("apply_v314_lua_gpu_efficiency.py")
text = base.read_text(encoding="utf-8")

start_marker = "replace_exact(\n    \"components/lua/luastate.cpp\",\n    '''        sol::table loaded(mSol, sol::create);"
end_marker = "replace_exact(\n    \"components/lua/scriptscontainer.hpp\","

a = text.find(start_marker)
if a < 0:
    raise RuntimeError("V3.14 compat: package-setup replacement start not found")
b = text.find(end_marker, a)
if b < 0:
    raise RuntimeError("V3.14 compat: package-setup replacement end not found")

replacement = """replace_exact(
    \"components/lua/luastate.cpp\",
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
            env[\"require\"] = mSol[\"requireGen\"](env, loaded, mSol[\"loadFromVFS\"]);
            sol::set_environment(env, script);
        }
        Debug::V36LuaAddScriptTrace::PhaseScope v36ScriptBody(
            Debug::V36LuaAddScriptTrace::Phase::ScriptBody);
        return call(scriptId, script);''',
    '''        sol::table loaded(mSol, sol::create);
        if (packagePrototype)
        {
            sol::table meta(mSol, sol::create);
            meta[sol::meta_method::index] = *packagePrototype;
            loaded[sol::metatable_key] = meta;
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36CommonPackages(
                    Debug::V36LuaAddScriptTrace::Phase::CommonPackages);
                for (const auto& [key, value] : mCommonPackages)
                    if (value.is<sol::function>())
                        loaded[key] = maybeRunLoader(value);
            }
            {
                Debug::V36LuaAddScriptTrace::PhaseScope v36ContainerPackages(
                    Debug::V36LuaAddScriptTrace::Phase::ContainerPackages);
                for (const auto& [key, value] : packages)
                    if (value.is<sol::function>())
                        loaded[key] = maybeRunLoader(value);
            }
        }
        else
        {
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
        }
        {
            Debug::V36LuaAddScriptTrace::PhaseScope v36RequireSetup(
                Debug::V36LuaAddScriptTrace::Phase::RequireSetup);
            env[\"require\"] = mSol[\"requireGen\"](env, loaded, mSol[\"loadFromVFS\"]);
            sol::set_environment(env, script);
        }
        Debug::V36LuaAddScriptTrace::PhaseScope v36ScriptBody(
            Debug::V36LuaAddScriptTrace::Phase::ScriptBody);
        return call(scriptId, script);''',
)

"""

text = text[:a] + replacement + text[b:]

# V3.14 OSG compatibility: do not construct StateToCompile directly. The OSG build
# used by the Windows toolchain exposes CompileSet::buildCompileMap(ContextSet&, Mode)
# as the stable collector entrypoint. Build the map through that overload and then
# queue the already-built CompileSet through ICO.
ground_old = '''                osgUtil::GLObjectsVisitor stateToCompile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES
                    | osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS);
                node->accept(stateToCompile);
                if (!stateToCompile.empty())
                {
                    auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(node);
                    compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                    ico->add(compileSet, false);
                    mV314CompileQueued.fetch_add(1, std::memory_order_relaxed);
                }'''
ground_new = '''                auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(node);
                const auto compileMode = static_cast<osgUtil::GLObjectsVisitor::Mode>(
                    osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES
                    | osgUtil::GLObjectsVisitor::COMPILE_DISPLAY_LISTS);
                compileSet->buildCompileMap(ico->getContextSet(), compileMode);
                ico->add(compileSet, false);
                mV314CompileQueued.fetch_add(1, std::memory_order_relaxed);'''
if text.count(ground_old) != 1:
    raise RuntimeError(f"V3.14 compat: expected 1 groundcover ICO collector block, found {text.count(ground_old)}")
text = text.replace(ground_old, ground_new, 1)

postfx_old = '''                osgUtil::GLObjectsVisitor stateToCompile(osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES);
                compileRoot->accept(stateToCompile);
                if (!stateToCompile.empty())
                {
                    auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(compileRoot);
                    compileSet->buildCompileMap(ico->getContextSet(), stateToCompile);
                    ico->add(compileSet, false);
                    Log(Debug::Info) << "V3.14 queued active PostFX chain for ICO compile warmup";
                }'''
postfx_new = '''                auto compileSet = new osgUtil::IncrementalCompileOperation::CompileSet(compileRoot);
                const auto compileMode = static_cast<osgUtil::GLObjectsVisitor::Mode>(
                    osgUtil::GLObjectsVisitor::COMPILE_STATE_ATTRIBUTES);
                compileSet->buildCompileMap(ico->getContextSet(), compileMode);
                ico->add(compileSet, false);
                Log(Debug::Info) << "V3.14 queued active PostFX chain for ICO compile warmup";'''
if text.count(postfx_old) != 1:
    raise RuntimeError(f"V3.14 compat: expected 1 PostFX ICO collector block, found {text.count(postfx_old)}")
text = text.replace(postfx_old, postfx_new, 1)

exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
