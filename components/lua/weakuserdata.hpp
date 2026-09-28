#ifndef OPENMW_LUA_WEAKUSERDATA_H
#define OPENMW_LUA_WEAKUSERDATA_H

#include <array>
#include <cstdint>
#include <limits>
extern "C" {
#include <lua.h>
}

namespace LuaUtil::WeakUserdata
{
    // The registry is local to the Lua state. Call only on the thread owning it.
    inline void setSlot(lua_State* state, void* slot, bool enabled)
    {
        lua_pushlightuserdata(state, slot);
        if (enabled)
        {
            lua_newtable(state);
            lua_newtable(state);
            lua_pushstring(state, "v");
            lua_setfield(state, -2, "__mode");
            lua_setmetatable(state, -2);
        }
        else lua_pushnil(state);
        lua_rawset(state, LUA_REGISTRYINDEX);
    }

    inline void resetSlot(lua_State* state, void* slot)
    {
        lua_pushlightuserdata(state, slot);
        lua_rawget(state, LUA_REGISTRYINDEX);
        const bool enabled = lua_istable(state, -1);
        lua_pop(state, 1);
        setSlot(state, slot, enabled);
    }

    // Retain the donor's exact-double fast path in its valid range. The
    // fallback is an exact 8-byte key, not a lossy conversion of a 64-bit ID.
    inline void pushObjectKey(lua_State* state, std::uint32_t index, std::int32_t contentFile)
    {
        if (std::numeric_limits<lua_Number>::digits >= 53
            && contentFile >= -1048576 && contentFile <= 1048575)
        {
            const auto value = static_cast<std::int64_t>(index)
                + static_cast<std::int64_t>(contentFile) * (std::int64_t{1} << 33);
            lua_pushnumber(state, static_cast<lua_Number>(value));
            return;
        }
        std::array<char,8> bytes{};
        const std::uint64_t value = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(contentFile)) << 32) | index;
        for (unsigned i = 0; i < 8; ++i) bytes[i] = static_cast<char>((value >> (8*i)) & 0xff);
        lua_pushlstring(state, bytes.data(), bytes.size());
    }

    template<class PushKey, class PushValue>
    int push(lua_State* state, void* slot, PushKey&& pushKey, PushValue&& pushValue)
    {
        lua_pushlightuserdata(state, slot);
        lua_rawget(state, LUA_REGISTRYINDEX);
        if (!lua_istable(state, -1))
        {
            lua_pop(state, 1);
            return pushValue();
        }
        pushKey();
        lua_rawget(state, -2);
        if (!lua_isnil(state, -1))
        {
            lua_remove(state, -2);
            return 1;
        }
        lua_pop(state, 1);
        pushValue();
        pushKey();
        lua_pushvalue(state, -2);
        lua_rawset(state, -4);
        lua_remove(state, -2);
        return 1;
    }
}
#endif
