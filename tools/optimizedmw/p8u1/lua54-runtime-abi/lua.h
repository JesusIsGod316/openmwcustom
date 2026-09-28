/* Test-only public Lua 5.4 C API declarations, derived from lua/lua v5.4.6
 * lua.h. This is NOT a complete Lua SDK and must never be used to compile
 * the game or sol. Tests link the real native Lua 5.4 shared library.
 * Lua public API: Copyright (C) 1994-2023 Lua.org, PUC-Rio (MIT).
 */
#ifndef P8U1_LUA54_RUNTIME_ABI_H
#define P8U1_LUA54_RUNTIME_ABI_H
#include <stddef.h>
typedef struct lua_State lua_State;
typedef double lua_Number;
#define LUA_REGISTRYINDEX (-1001000)
#define LUA_TTABLE 5
#define LUA_TNIL 0
#define LUA_GCCOLLECT 2
lua_State* luaL_newstate(void);
void lua_close(lua_State*);
lua_Number lua_version(lua_State*);
int lua_gettop(lua_State*);
void lua_settop(lua_State*,int);
int lua_type(lua_State*,int);
void lua_pushlightuserdata(lua_State*,void*);
void lua_pushnumber(lua_State*,lua_Number);
const char* lua_pushlstring(lua_State*,const char*,size_t);
const char* lua_pushstring(lua_State*,const char*);
void lua_pushnil(lua_State*);
void lua_pushvalue(lua_State*,int);
int lua_rawget(lua_State*,int);
void lua_rawset(lua_State*,int);
void lua_createtable(lua_State*,int,int);
void lua_setfield(lua_State*,int,const char*);
int lua_setmetatable(lua_State*,int);
void lua_rotate(lua_State*,int,int);
int lua_rawequal(lua_State*,int,int);
void* lua_newuserdatauv(lua_State*,size_t,int);
int lua_gc(lua_State*,int,...);
#define lua_pop(L,n) lua_settop(L,-(n)-1)
#define lua_newtable(L) lua_createtable(L,0,0)
#define lua_istable(L,n) (lua_type(L,n)==LUA_TTABLE)
#define lua_isnil(L,n) (lua_type(L,n)==LUA_TNIL)
#define lua_remove(L,n) (lua_rotate(L,n,-1),lua_pop(L,1))
#endif
