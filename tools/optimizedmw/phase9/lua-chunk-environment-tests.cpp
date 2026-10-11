// Standalone Lua 5.1/LuaJIT environment-semantics groundwork.
// This fixture implements no production loader, cache, pool, lease, or require.
// Passing the narrow closure cases does not certify general chunk reuse.
// Compile/run later against the exact pinned headers, import library and DLL.
// Lua environments: https://www.lua.org/manual/5.1/manual.html#2.9
// LuaJIT lua_setfenv: https://github.com/LuaJIT/LuaJIT/blob/v2.1/src/lj_api.c

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <array>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace
{
    struct Context
    {
        int bodyEffects = 0;
        int nestedRoot = LUA_NOREF;
        int nestedEnvironment = LUA_NOREF;
    };

    unsigned checks = 0;

    void check(bool condition, const char* message)
    {
        ++checks;
        if (!condition)
            throw std::runtime_error(message);
    }

    void balanced(lua_State* state, int before, const char* operation)
    {
        check(lua_gettop(state) == before, operation);
    }

    void stringField(lua_State* state, int table, const char* key, const char* value)
    {
        lua_pushstring(state, value);
        lua_setfield(state, table < 0 ? table - 1 : table, key);
    }

    int bumpBody(lua_State* state)
    {
        auto* context = static_cast<Context*>(lua_touserdata(state, lua_upvalueindex(1)));
        ++context->bodyEffects;
        return 0;
    }

    // C callback deliberately has only trivial locals: lua_error may unwind it.
    // Safe mode uses a distinct ordinary loaded root. Unsafe mode intentionally
    // rebinds the still executing outer root to demonstrate the fenv hazard.
    int nestedCall(lua_State* state)
    {
        auto* context = static_cast<Context*>(lua_touserdata(state, lua_upvalueindex(1)));
        lua_rawgeti(state, LUA_REGISTRYINDEX, context->nestedRoot);
        lua_rawgeti(state, LUA_REGISTRYINDEX, context->nestedEnvironment);
        if (!lua_setfenv(state, -2))
            return luaL_error(state, "fixture nested root has no environment");
        if (lua_pcall(state, 0, 1, 0) != 0)
            return lua_error(state);
        return 1;
    }

    int traceback(lua_State* state)
    {
        // Internal fixture error handler, never exposed in the restricted base.
        lua_getglobal(state, "debug");
        lua_getfield(state, -1, "traceback");
        lua_pushvalue(state, 1);
        lua_pushinteger(state, 2);
        lua_call(state, 2, 1);
        return 1;
    }

    int makeRestrictedBase(lua_State* state, Context& context)
    {
        const int before = lua_gettop(state);
        lua_newtable(state);
        // Mirrors the relevant production safe function/library surface.
        // Read-only wrappers and the engine's utf8 package are not reproduced:
        // this fixture examines function environments and fresh export identity.
        constexpr std::array names{ "assert", "error", "ipairs", "next", "pairs", "pcall", "select",
            "tonumber", "tostring", "type", "unpack", "xpcall", "rawequal", "rawget", "rawset",
            "setmetatable", "coroutine", "math", "string", "table" };
        for (const char* name : names)
        {
            lua_getglobal(state, name);
            lua_setfield(state, -2, name);
        }
        lua_pushlightuserdata(state, &context);
        lua_pushcclosure(state, bumpBody, 1);
        lua_setfield(state, -2, "bump_body");
        lua_pushlightuserdata(state, &context);
        lua_pushcclosure(state, nestedCall, 1);
        lua_setfield(state, -2, "nested_call");
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        balanced(state, before, "restricted base stack balance");
        return reference;
    }

    int makeEnvironment(lua_State* state, int base, const char* owner, int seed, bool nested = false)
    {
        const int before = lua_gettop(state);
        lua_newtable(state);
        stringField(state, -1, "owner", owner);
        lua_pushinteger(state, seed);
        lua_setfield(state, -2, "seed");
        lua_pushboolean(state, nested);
        lua_setfield(state, -2, "enter_nested");
        lua_pushvalue(state, -1);
        lua_setfield(state, -2, "_G");
        lua_newtable(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, base);
        lua_setfield(state, -2, "__index");
        lua_pushboolean(state, false);
        lua_setfield(state, -2, "__metatable");
        lua_setmetatable(state, -2);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        balanced(state, before, "fresh environment stack balance");
        return reference;
    }

    int load(lua_State* state, const char* code, const char* source)
    {
        const int before = lua_gettop(state);
        if (luaL_loadbuffer(state, code, std::char_traits<char>::length(code), source) != 0)
        {
            const char* text = lua_tostring(state, -1);
            const std::string error = text ? text : "unknown compile failure";
            lua_pop(state, 1);
            balanced(state, before, "failed load stack balance");
            throw std::runtime_error(error);
        }
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        balanced(state, before, "chunk load stack balance");
        return reference;
    }

    void bind(lua_State* state, int function, int environment)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, function);
        lua_rawgeti(state, LUA_REGISTRYINDEX, environment);
        check(lua_setfenv(state, -2) == 1, "file root environment assignment");
        lua_pop(state, 1);
        balanced(state, before, "environment assignment stack balance");
    }

    int run(lua_State* state, int function)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, function);
        if (lua_pcall(state, 0, 1, 0) != 0)
        {
            const char* text = lua_tostring(state, -1);
            const std::string error = text ? text : "unknown execution failure";
            lua_pop(state, 1);
            balanced(state, before, "failed invocation stack balance");
            throw std::runtime_error(error);
        }
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        balanced(state, before, "chunk invocation stack balance");
        return reference;
    }

    int fieldReference(lua_State* state, int table, const char* key)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, table);
        lua_getfield(state, -1, key);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pop(state, 1);
        balanced(state, before, "export field stack balance");
        return reference;
    }

    std::string fieldString(lua_State* state, int table, const char* key)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, table);
        lua_getfield(state, -1, key);
        const char* value = lua_tostring(state, -1);
        const std::string result = value ? value : "[nil]";
        lua_pop(state, 2);
        balanced(state, before, "string field stack balance");
        return result;
    }

    bool same(lua_State* state, int first, int second)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, first);
        lua_rawgeti(state, LUA_REGISTRYINDEX, second);
        const bool equal = lua_rawequal(state, -1, -2) != 0;
        lua_pop(state, 2);
        balanced(state, before, "reference comparison stack balance");
        return equal;
    }

    int functionEnvironment(lua_State* state, int function)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, function);
        lua_getfenv(state, -1);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_pop(state, 1);
        balanced(state, before, "function environment inspection balance");
        return reference;
    }

    void child(lua_State* state, int function, int delta, const char* globalOwner, const char* capturedOwner,
        int seed, int counter)
    {
        const int before = lua_gettop(state);
        lua_rawgeti(state, LUA_REGISTRYINDEX, function);
        lua_pushinteger(state, delta);
        check(lua_pcall(state, 1, 5, 0) == 0, "exported child invocation");
        check(std::string(lua_tostring(state, -5)) == globalOwner, "child keeps expected global environment");
        check(std::string(lua_tostring(state, -4)) == capturedOwner, "child keeps captured owner");
        check(lua_tointeger(state, -3) == seed, "child keeps captured local seed");
        check(lua_tointeger(state, -2) == counter, "child has distinct captured counter");
        check(lua_tointeger(state, -1) == counter, "child has distinct mutable export table");
        lua_pop(state, 5);
        balanced(state, before, "exported child stack balance");
    }

    std::string fail(lua_State* state, int function)
    {
        const int before = lua_gettop(state);
        lua_pushcfunction(state, traceback);
        lua_rawgeti(state, LUA_REGISTRYINDEX, function);
        check(lua_pcall(state, 0, 1, before + 1) != 0, "deliberate error must be returned");
        const char* text = lua_tostring(state, -1);
        check(text != nullptr, "error retains string and source traceback");
        const std::string result(text);
        lua_pop(state, 2);
        balanced(state, before, "error handler stack balance");
        return result;
    }

    constexpr const char* rootSource =
        "bump_body()\n"
        "body_count = (body_count or 0) + 1\n"
        "local captured_owner, captured_seed = owner, seed\n"
        "local counter, mutable = 0, { value = 0 }\n"
        "if enter_nested then nested_call() end\n"
        "return { captured_owner = captured_owner, owner_after_nested = owner, mutable = mutable,\n"
        " child = function(delta)\n"
        "  counter = counter + delta; mutable.value = mutable.value + delta\n"
        "  return owner, captured_owner, captured_seed, counter, mutable.value\n"
        " end }\n";
}

int main()
{
    try
    {
        std::unique_ptr<lua_State, decltype(&lua_close)> holder(luaL_newstate(), lua_close);
        check(holder != nullptr, "Lua state creation");
        lua_State* state = holder.get();
        luaL_openlibs(state);
        Context context;
        const int base = makeRestrictedBase(state, context);
        const int neutral = makeEnvironment(state, base, "NEUTRAL", 0);
        const int envA = makeEnvironment(state, base, "A", 10);
        const int envB = makeEnvironment(state, base, "B", 20);
        const int root = load(state, rootSource, "@p9-chunk-environment-root.lua");
        lua_rawgeti(state, LUA_REGISTRYINDEX, root);
        lua_Debug info{};
        check(lua_getinfo(state, ">u", &info) != 0 && info.nups == 0, "file root has zero captured upvalues");
        balanced(state, 0, "root eligibility inspection balance");

        bind(state, root, envA);
        const int resultA = run(state, root);
        const int childA = fieldReference(state, resultA, "child");
        bind(state, root, neutral);
        check(same(state, functionEnvironment(state, childA), envA), "export A retains original fenv");
        check(same(state, functionEnvironment(state, root), neutral), "root no longer holds A environment");

        bind(state, root, envB);
        const int resultB = run(state, root);
        const int childB = fieldReference(state, resultB, "child");
        bind(state, root, neutral);
        check(context.bodyEffects == 2, "every initialization executes body side effects");
        check(!same(state, childA, childB), "different invocation child functions are distinct");
        check(!same(state, fieldReference(state, resultA, "mutable"), fieldReference(state, resultB, "mutable")),
            "mutable result tables are distinct");
        child(state, childA, 1, "A", "A", 10, 1);
        child(state, childB, 2, "B", "B", 20, 2);
        child(state, childA, 3, "A", "A", 10, 4);

        bind(state, root, envA);
        const int resultA2 = run(state, root);
        const int childA2 = fieldReference(state, resultA2, "child");
        bind(state, root, neutral);
        check(context.bodyEffects == 3, "same environment invocation still executes body");
        check(!same(state, childA, childA2), "same fenv still produces fresh child/upvalue cells");
        child(state, childA2, 1, "A", "A", 10, 1);
        child(state, childA, 1, "A", "A", 10, 5);
        lua_rawgeti(state, LUA_REGISTRYINDEX, envA);
        stringField(state, -1, "owner", "A-updated");
        lua_pop(state, 1);
        lua_gc(state, LUA_GCCOLLECT, 0);
        child(state, childA, 1, "A-updated", "A", 10, 6);
        child(state, childB, 1, "B", "B", 20, 3);

        // Nested safe reference uses a separately loaded root; old frames and
        // exported children are untouched. This is the ordinary fallback model.
        const int outerEnv = makeEnvironment(state, base, "OUTER", 30, true);
        const int innerRoot = load(state, rootSource, "@p9-chunk-environment-root.lua");
        context.nestedRoot = innerRoot;
        context.nestedEnvironment = envB;
        bind(state, root, outerEnv);
        const int nestedSafe = run(state, root);
        bind(state, root, neutral);
        bind(state, innerRoot, neutral);
        check(fieldString(state, nestedSafe, "owner_after_nested") == "OUTER", "nested distinct root preserves outer globals");
        child(state, fieldReference(state, nestedSafe, "child"), 1, "OUTER", "OUTER", 30, 1);

        // Positive hazard detection, not a successful reuse case: rebinding a
        // currently executing root changes its globals when the outer resumes.
        context.nestedRoot = root;
        bind(state, root, outerEnv);
        const int nestedUnsafe = run(state, root);
        check(fieldString(state, nestedUnsafe, "captured_owner") == "OUTER", "unsafe nested case captured original local");
        check(fieldString(state, nestedUnsafe, "owner_after_nested") == "B", "active-root rebind hazard must be observed");
        child(state, fieldReference(state, nestedUnsafe, "child"), 1, "B", "OUTER", 30, 1);
        bind(state, root, neutral);

        // A debug/self-retention API is NOT exposed by the production sandbox.
        // Deliberately expose it here to show why unknown/debug contexts cannot
        // be certified by the narrow synchronous exported-closure proof.
        const int unsafeA = makeEnvironment(state, base, "UNSAFE-A", 0);
        const int unsafeB = makeEnvironment(state, base, "UNSAFE-B", 0);
        for (const int env : { unsafeA, unsafeB })
        {
            lua_rawgeti(state, LUA_REGISTRYINDEX, env);
            lua_getglobal(state, "debug");
            lua_setfield(state, -2, "debug");
            lua_pop(state, 1);
        }
        const int selfRoot = load(state, "return { self = debug.getinfo(1, 'f').func, owner = owner }", "@p9-self-retained-root.lua");
        bind(state, selfRoot, unsafeA);
        const int selfA = run(state, selfRoot);
        const int escaped = fieldReference(state, selfA, "self");
        check(same(state, escaped, selfRoot), "debug case retains the exact root object");
        bind(state, selfRoot, unsafeB);
        const int escapedResult = run(state, escaped);
        check(fieldString(state, escapedResult, "owner") == "UNSAFE-B", "escaped-root rebind hazard must be observed");
        bind(state, selfRoot, neutral);

        constexpr const char* errorSource = "local marker = 7\nerror('p9 deliberate failure:' .. owner)\n";
        const int ordinaryError = load(state, errorSource, "@p9-error-root.lua");
        const int reboundError = load(state, errorSource, "@p9-error-root.lua");
        bind(state, ordinaryError, envB);
        const std::string expectedError = fail(state, ordinaryError);
        bind(state, reboundError, envA);
        (void)fail(state, reboundError);
        bind(state, reboundError, neutral);
        bind(state, reboundError, envB);
        const std::string actualError = fail(state, reboundError);
        check(expectedError == actualError, "identical caller preserves error/source/traceback after rebind");
        check(actualError.find("p9-error-root.lua:2:") != std::string::npos, "source path and original line survive");
        check(actualError.find("stack traceback:") != std::string::npos, "error handler retains traceback");
        bind(state, reboundError, neutral);
        balanced(state, 0, "final fixture stack balance");

        lua_getglobal(state, "jit");
        check(lua_istable(state, -1), "fixture must run against LuaJIT, not a substituted Lua runtime");
        lua_getfield(state, -1, "version");
        check(lua_isstring(state, -1), "LuaJIT runtime must report its version");
        std::cout << "runtime=" << lua_tostring(state, -1) << '\n';
        lua_pop(state, 2);
#ifdef _WIN32
        const HMODULE module = GetModuleHandleA("lua51.dll");
        std::array<char, 32768> modulePath{};
        check(module != nullptr && GetModuleFileNameA(module, modulePath.data(), static_cast<DWORD>(modulePath.size())) != 0,
            "exact lua51 DLL provenance must be available to the host qualifier");
        std::cout << "runtime_module=" << modulePath.data() << '\n';
#endif
        std::cout << "LUA_CHUNK_ENVIRONMENT_PASS checks=" << checks
                  << " exported_closure_cases=1 nested_distinct_root_case=1"
                  << " active_root_rebind_hazard_observed=1 retained_root_hazard_observed=1"
                  << " general_pool_eligibility_proven=0 production_loader_changed=0\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "LUA_CHUNK_ENVIRONMENT_FAIL checks=" << checks << " error=" << error.what() << '\n';
        return 1;
    }
}
