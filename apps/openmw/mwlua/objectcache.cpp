// Adapted from OpenMW dbe2d5a0fd8d2da80f750deee88897df2593426e.
// Keep local/global types separate, weak values, and world-lifetime resets.
#include "object.hpp"
#include <components/lua/weakuserdata.hpp>

namespace MWLua
{
    namespace
    {
        enum Slot { LocalObjects, GlobalObjects, LocalCells, GlobalCells, SlotCount };
        char sSlots[SlotCount]{};
        template<class T>
        int pushObject(lua_State* state, const T& value, Slot slot)
        {
            return LuaUtil::WeakUserdata::push(state, &sSlots[slot], [&] {
                LuaUtil::WeakUserdata::pushObjectKey(state, value.id().mIndex, value.id().mContentFile);
            }, [&] { return sol::stack::push<sol::detail::as_value_tag<T>>(state, value); });
        }
        template<class T>
        int pushCell(lua_State* state, const T& value, Slot slot)
        {
            return LuaUtil::WeakUserdata::push(state, &sSlots[slot], [&] {
                lua_pushlightuserdata(state, value.mStore);
            }, [&] { return sol::stack::push<sol::detail::as_value_tag<T>>(state, value); });
        }
    }
    void initializeObjectCaches(lua_State* state, bool enabled)
    {
        for (char& slot : sSlots) LuaUtil::WeakUserdata::setSlot(state, &slot, enabled);
    }
    void clearObjectCaches(lua_State* state)
    {
        for (char& slot : sSlots) LuaUtil::WeakUserdata::resetSlot(state, &slot);
    }
    int pushCachedObject(lua_State* s, const LObject& v) { return pushObject(s, v, LocalObjects); }
    int pushCachedObject(lua_State* s, const GObject& v) { return pushObject(s, v, GlobalObjects); }
    int pushCachedObject(lua_State* s, const LCell& v) { return pushCell(s, v, LocalCells); }
    int pushCachedObject(lua_State* s, const GCell& v) { return pushCell(s, v, GlobalCells); }
}
