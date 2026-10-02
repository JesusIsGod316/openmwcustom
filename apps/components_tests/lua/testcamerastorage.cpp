#include <gtest/gtest.h>
#include <sol/sol.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>

namespace
{
    class CameraStorageTest : public testing::Test
    {
    protected:
        sol::state mLua;
        void SetUp() override
        {
            mLua.open_libraries(sol::lib::base, sol::lib::math, sol::lib::string, sol::lib::table);
            auto setup = mLua.safe_script(R"(
cameraSettings = { maxDistance = 800 }
cameraCallbacks = {}
registeredGroups = {}
thirdPerson = { baseDistance = 100, preferredDistance = 100, standingPreview = false,
    noOffsetControl = {}, update = function() completedUpdates = completedUpdates + 1 end }
completedUpdates = 0
warnings = {}
print = function(value) warnings[#warnings + 1] = value end
zoomAction = 10
local camera = { MODE = { FirstPerson = 1, ThirdPerson = 2, Preview = 3, Static = 4, Vanity = 5 },
    getMode = function() return 2 end, getQueuedMode = function() return nil end,
    getBaseFieldOfView = function() return 60 end,
    setFieldOfView = function() end, setMode = function() end, allowCharacterDeferredRotation = function() end,
    setCollisionType = function() end, showCrosshair = function() end,
    getThirdPersonDistance = function() return thirdPerson.preferredDistance end }
local actor = { CONTROL_SWITCH = {}, STANCE = { Nothing = 0 },
    getControlSwitch = function() return true end, getCurrentSpeed = function() return 0 end,
    getStance = function() return 0 end,
    activeEffects = function() return { getEffect = function() return { magnitude = 0 } end } end }
local packages = {
    ['openmw.camera'] = camera,
    ['openmw.core'] = { isWorldPaused = function() return false end,
        getGMST = function() return 30 end, magic = { EFFECT_TYPE = { Paralyze = 1 } } },
    ['openmw.debug'] = { isGodMode = function() return false end },
    ['openmw.input'] = { ACTION_TYPE = { Boolean = 0, Number = 1 }, registerAction = function() end,
        getNumberActionValue = function() return zoomAction end, isIdle = function() return false end },
    ['openmw.util'] = { bitAnd = function() return 0 end, bitOr = function() return 0 end,
        bitNot = function() return 0 end,
        clamp = function(v, lo, hi) return math.min(math.max(v, lo), hi) end },
    ['openmw.self'] = { controls = {} },
    ['openmw.nearby'] = { COLLISION_TYPE = { Default = 0, Actor = 1, Camera = 2, VisualOnly = 4 } },
    ['openmw.async'] = { callback = function(_, fn) return fn end },
    ['openmw.interfaces'] = { UI = { getMode = function() return nil end } },
    ['openmw.types'] = { Actor = actor, Player = actor },
    ['scripts.omw.camera.third_person'] = thirdPerson,
    ['scripts.omw.camera.head_bobbing'] = { update = function() end },
    ['scripts.omw.camera.first_person_auto_switch'] = {},
    ['scripts.omw.camera.move360'] = { onFrame = function() end },
}
packages['openmw.storage'] = { playerSection = function(name)
    if name == 'OmwSettingGroups' then return { get = function(_, key) return registeredGroups[key] end } end
    return { get = function(_, key) return cameraSettings[key] end,
        subscribe = function(_, fn) cameraCallbacks.settings = fn end }
end }
require = function(name) assert(packages[name], name); return packages[name] end
)", sol::script_pass_on_error);
            ASSERT_TRUE(setup.valid());
        }

        void loadCamera()
        {
            const auto path = std::filesystem::path(OPENMW_PROJECT_SOURCE_DIR)
                / "files/data/scripts/omw/camera/camera.lua";
            std::ifstream input(path);
            ASSERT_TRUE(input.good());
            const std::string source{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
            auto result = mLua.safe_script(source, sol::script_pass_on_error);
            ASSERT_TRUE(result.valid());
            mLua["testedCamera"] = result.get<sol::table>();
            check("testedCamera.engineHandlers.onActive(); testedCamera.interface.disableModeControl('fixture')");
        }

        void check(std::string_view script)
        {
            auto result = mLua.safe_script(script, sol::script_pass_on_error);
            ASSERT_TRUE(result.valid()) << (result.valid() ? "" : result.get<sol::error>().what());
        }
    };

    TEST_F(CameraStorageTest, PreservesNumericUserLimitAndSettingsChanges)
    {
        check("cameraSettings.maxDistance = 425");
        loadCamera();
        check("zoomAction = -1000; testedCamera.engineHandlers.onFrame(.016); "
              "assert(thirdPerson.baseDistance == 425); assert(cameraSettings.maxDistance == 425); assert(#warnings == 0)");
        check("cameraSettings.maxDistance = 600; cameraCallbacks.settings(); zoomAction = -1000; "
              "testedCamera.engineHandlers.onFrame(.016); assert(thirdPerson.baseDistance == 600)");
    }

    TEST_F(CameraStorageTest, MissingStorageDoesNotAbortFrameAndRecoversActualValue)
    {
        check("cameraSettings.maxDistance = nil");
        loadCamera();
        check("testedCamera.engineHandlers.onFrame(.016); testedCamera.engineHandlers.onFrame(.016); "
              "assert(thirdPerson.baseDistance == 100); assert(completedUpdates == 2); assert(#warnings == 1); "
              "assert(warnings[1]:find('registered=false', 1, true)); assert(cameraSettings.maxDistance == nil)");
        // Reproduces a late storage update before its subscription callback.
        check("registeredGroups.SettingsOMWCameraThirdPerson = {}; cameraSettings.maxDistance = 350; zoomAction = -1000; "
              "testedCamera.engineHandlers.onFrame(.016); assert(thirdPerson.baseDistance == 350); "
              "assert(completedUpdates == 3); assert(cameraSettings.maxDistance == 350)");
        check("cameraCallbacks.settings(); testedCamera.engineHandlers.onFrame(.016); "
              "cameraSettings.maxDistance = nil; cameraCallbacks.settings(); testedCamera.engineHandlers.onFrame(.016); "
              "assert(#warnings == 2); assert(cameraSettings.maxDistance == nil)");
    }

    TEST_F(CameraStorageTest, MalformedOrNonFiniteLimitNeverWritesStorage)
    {
        for (const auto* value : { "'invalid'", "0/0", "math.huge", "-1" })
        {
            check(std::string("cameraSettings.maxDistance = ") + value + "; cameraCallbacks.settings = nil");
            loadCamera();
            check("thirdPerson.baseDistance = 100; testedCamera.engineHandlers.onFrame(.016); "
                  "assert(thirdPerson.baseDistance == 100)");
        }
    }
}
