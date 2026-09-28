// Real production adapter + real sol/LuaJIT, without a synthetic Lua ABI.
#include <apps/openmw/mwlua/object.hpp>
#include <luajit.h>
#include <iostream>
#include <stdexcept>

namespace
{
    void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    template <class T>
    void push(lua_State* state, const T& value) { sol::stack::push(state, value); }
}

int main()
try
{
    static_assert(LUAJIT_VERSION_NUM >= 20000);
    sol::state first, second;
    first.open_libraries(sol::lib::base, sol::lib::table);
    auto* a=first.lua_state(); auto* b=second.lua_state();
    MWLua::initializeObjectCaches(a,true);
    MWLua::initializeObjectCaches(b,true);
    MWLua::LObject local(ESM::FormId{42,-1});
    MWLua::GObject global(ESM::FormId{42,-1});
    push(a,local); push(a,local);
    require(lua_rawequal(a,-1,-2),"real LObject pusher does not reuse userdata");
    require(sol::stack::get<MWLua::LObject>(a,-1).id()==local.id(),"cached wrapper lost RefNum");
    push(a,global);
    require(!lua_rawequal(a,-1,-2),"real local/global objects alias");
    push(b,local);
    require(lua_touserdata(a,1)!=lua_touserdata(b,1),"states share userdata");
    // Use null and stable sentinel addresses only as keys; no CellStore is dereferenced.
    int cellTag=0;
    auto* cell=reinterpret_cast<MWWorld::CellStore*>(&cellTag);
    push(a,MWLua::LCell{cell}); push(a,MWLua::LCell{cell});
    require(lua_rawequal(a,-1,-2),"local cell wrapper not reused");
    push(a,MWLua::GCell{cell});
    require(!lua_rawequal(a,-1,-2),"local/global cells alias");
    MWLua::clearObjectCaches(a);
    push(a,local);
    require(!lua_rawequal(a,1,-1),"world reset reused retained old-world wrapper");
    MWLua::initializeObjectCaches(a,false);
    push(a,local); push(a,local);
    require(!lua_rawequal(a,-1,-2),"disabled control still caches wrappers");
    // Verify Lua-visible weak ownership with real registered cell userdata.
    lua_settop(a,0);
    MWLua::initializeObjectCaches(a,true);
    push(a,MWLua::LCell{cell});
    lua_newtable(a); lua_newtable(a); lua_pushliteral(a,"v"); lua_setfield(a,-2,"__mode");
    lua_setmetatable(a,-2); lua_pushvalue(a,1); lua_rawseti(a,-2,1); lua_setglobal(a,"weak");
    lua_settop(a,0); lua_gc(a,LUA_GCCOLLECT,0); lua_gc(a,LUA_GCCOLLECT,0);
    lua_getglobal(a,"weak"); lua_rawgeti(a,-1,1);
    require(lua_isnil(a,-1),"production weak cache kept a discarded cell alive");
    lua_settop(a,0);
    std::cout << "PASS: " << LUAJIT_VERSION
        << " production sol object/cell pushers, state/type separation, off/reset and weak collection\n";
}
catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
