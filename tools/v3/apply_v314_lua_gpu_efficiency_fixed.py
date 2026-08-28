from pathlib import Path

# Compatibility wrapper for the mature generated V3 stack. V3.6 attribution already
# instruments LuaState::runInNewSandbox, so rewrite only V3.14's fragile upstream
# package-setup replacement before executing the V3.14 layer. All V3.6 phase scopes
# remain intact.
base = Path(__file__).with_name("apply_v314_lua_gpu_efficiency.py")
text = base.read_text(encoding="utf-8")

start_marker = '''replace_exact(
    "components/lua/luastate.cpp",
    ''' + "'''        sol::table loaded(mSol, sol::create);"
end_marker = '''replace_exact(
    "components/lua/scriptscontainer.hpp",'''

a = text.find(start_marker)
if a < 0:
    raise RuntimeError("V3.14 compat: package-setup replacement start not found")
b = text.find(end_marker, a)
if b < 0:
    raise RuntimeError("V3.14 compat: package-setup replacement end not found")

replacement = r'''replace_exact(
    "components/lua/luastate.cpp",
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
            env["require"] = mSol["requireGen"](env, loaded, mSol["loadFromVFS"]);
            sol::set_environment(env, script);
        }
        Debug::V36LuaAddScriptTrace::PhaseScope v36ScriptBody(
            Debug::V36LuaAddScriptTrace::Phase::ScriptBody);
        return call(scriptId, script);''',
)

'''

text = text[:a] + replacement + text[b:]
exec(compile(text, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
