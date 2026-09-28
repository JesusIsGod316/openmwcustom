#include <components/lua/weakuserdata.hpp>
#include <iostream>
#include <stdexcept>
#include <memory>
#ifndef P8U1_MINIMAL_LUA54
extern "C" {
#include <lauxlib.h>
#include <luajit.h>
}
#endif
void require(bool x,const char* m){if(!x)throw std::runtime_error(m);}
int main()
try
{
    std::unique_ptr<lua_State,decltype(&lua_close)> a(luaL_newstate(),lua_close), b(luaL_newstate(),lua_close);
    require(a && b,"native Lua state allocation failed");
#ifdef P8U1_MINIMAL_LUA54
    require(lua_version(a.get())==504,"native Lua 5.4 ABI required");
#endif
    char slots[4]{};int made=0;
    auto push=[&](lua_State* L,int slot,std::uint32_t index,std::int32_t file) {
        return LuaUtil::WeakUserdata::push(L,&slots[slot],[&]{LuaUtil::WeakUserdata::pushObjectKey(L,index,file);},[&]{
#ifdef P8U1_MINIMAL_LUA54
            lua_newuserdatauv(L,16,0);
#else
            lua_newuserdata(L,16);
#endif
            ++made;return 1;
        });
    };
    for(char& slot:slots) {LuaUtil::WeakUserdata::setSlot(a.get(),&slot,true);LuaUtil::WeakUserdata::setSlot(b.get(),&slot,true);}
    push(a.get(),0,42,-1);push(a.get(),0,42,-1);
    require(lua_rawequal(a.get(),-1,-2)&&made==1,"same object not reused");
    push(a.get(),1,42,-1);require(!lua_rawequal(a.get(),-1,-2),"local/global wrappers aliased");
    push(b.get(),0,42,-1);require(made==3,"registry table shared across Lua states");
    lua_settop(a.get(),0);lua_gc(a.get(),LUA_GCCOLLECT,0);
    push(a.get(),0,42,-1);require(made==4,"weak values retained an unreferenced object");
    LuaUtil::WeakUserdata::resetSlot(a.get(),&slots[0]);push(a.get(),0,42,-1);
    require(!lua_rawequal(a.get(),-1,-2),"world reset reused an old-world wrapper");
    LuaUtil::WeakUserdata::setSlot(a.get(),&slots[0],false);
    push(a.get(),0,42,-1);push(a.get(),0,42,-1);require(!lua_rawequal(a.get(),-1,-2),"off control cached values");
    LuaUtil::WeakUserdata::setSlot(a.get(),&slots[0],true);
    push(a.get(),0,0xffffffffu,0x7fffffff);push(a.get(),0,0xfffffffeu,0x7fffffff);
    require(!lua_rawequal(a.get(),-1,-2),"large RefNum keys collided");
    push(a.get(),0,0xfffffffeu,0x7fffffff);require(lua_rawequal(a.get(),-1,-2),"large RefNum not stable");
    lua_settop(a.get(),0);LuaUtil::WeakUserdata::pushObjectKey(a.get(),0,-1048576);
    LuaUtil::WeakUserdata::pushObjectKey(a.get(),1,-1048576);
    require(!lua_rawequal(a.get(),-1,-2),"negative exact-double boundary collided");
    std::cout<<"PASS: native Lua userdata identity, weak GC, state/type isolation, reset/off, exact full-domain RefNum keys\n";
}
catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
